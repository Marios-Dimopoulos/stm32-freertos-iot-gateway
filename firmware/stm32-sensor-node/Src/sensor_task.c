/*
 * sensor_task.c — the sensor task: reads the BMP280 once a second and prints it.
 *
 * WHAT IT DOES, EVERY PERIOD (one second):
 *
 *   not initialised -> bmp280_init(); on success print "BMP280 init: OK".
 *   initialised     -> bmp280_read(); on success print "T=23.64 C  P=1019.69 hPa".
 *   any failure     -> count it, print "BMP280 error: <cause> (failures: n)", and
 *                      go back to "not initialised": the next period runs
 *                      bmp280_init() again.
 *
 * ONE FRAME PER PERIOD, WHATEVER HAPPENED. At the end of every period the task builds
 * one 14-byte frame for the gateway (frame.h, ADR-0010) and stores it as the latest frame:
 *
 *   init just succeeded -> FRAME_STATUS_NO_READING, temperature and pressure 0
 *   reading OK          -> FRAME_STATUS_OK, the reading
 *   any failure         -> the matching FRAME_STATUS_SENSOR_..., temperature and
 *                          pressure 0 (see prvStatusFromBmp280ToFrame())
 *
 * The sequence number goes up by one per period, also while the sensor is failing, so
 * the gateway can use it as a heartbeat: a number that stops moving means this task is
 * not running (a failing sensor still moves it). The first frame carries 1; 0 is kept
 * for the placeholder that the transmit task sends before this task has produced
 * anything.
 *
 * WHY INITIALISE AGAIN AFTER EVERY ERROR. A sensor that stopped answering and comes
 * back (its supply was interrupted, or it was power-cycled after CSB locked it out of
 * I2C, see ADR-0007) returns with its DEFAULT settings, in sleep mode. Reading it
 * would then report "no data yet" forever; only a new init (reset, calibration,
 * configuration) puts it back to work. The rule is the same for every cause, so there
 * are no counters and no special cases that nothing has shown to be needed.
 *
 * WHY THE FIRST READ IS A FULL PERIOD AFTER THE INIT. The sensor's first measurement
 * takes tens of milliseconds (x16 oversampling). The task initialises, then sleeps
 * until the next period, so the first read finds a real result instead of "no data
 * yet".
 *
 * THIS TASK OWNS THE BMP280 AND THE I2C BUS (ADR-0007: one task, no mutex). Nothing
 * else may call bmp280_* or i2c_*.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "sensor_task.h"
#include "bmp280.h"
#include "frame.h"
#include "format.h"
#include "uart.h"

/* ==========================================================================
 * MEMORY AND PARAMETERS
 * ==========================================================================
 *
 * With static allocation (ADR-0001) we declare the task's memory ourselves and hand
 * it to xTaskCreateStatic():
 *   - the stack: a StackType_t array. The size is in WORDS (4 bytes each), so 256
 *     means 1 KB. MEASURED (ADR-0005). Phase 1, before the frame code:
 *     uxTaskGetStackHighWaterMark() reported 167 words free, so the deepest path used
 *     89 words (356 bytes), the same with the sensor connected and disconnected (the
 *     failing init returns at its first call, so it is shallower than the successful
 *     one, which runs at start-up). Phase 2, with the frame code (2026-10-18, the 0xA5
 *     pattern in the debugger after 60 reads from the Pi): 640 bytes untouched, so the
 *     peak is 96 words (384 bytes) and 256 words is 2.7 times that. The ADR-0005 rule
 *     (peak plus at least 50%) alone would allow 144; 256 is kept on purpose, because
 *     RAM is plentiful. The task calls the BMP280 module, the I2C
 *     driver (with its critical sections and tick reads), the formatter, a 64-byte
 *     line buffer and the UART; the worst-case saved context alone is 51 words (see
 *     FreeRTOSConfig.h). Repeat the measurement if the task's code changes.
 *   - the TCB: a StaticTask_t, the kernel's record of the task.
 * Both are file-scope `static`, so they live in .bss for the whole program (they
 * would be destroyed if they were locals of main(): the scheduler resets the main
 * stack when it starts).
 *
 * Priority 1 is the sensor task's level (priority 0 is Idle, priority 2 is the
 * transmit task).
 *
 * THE PERIOD. The sensor measures by itself many times per second (standby 0.5 ms,
 * x16 oversampling, IIR filter), so reading once per second is far from its limit,
 * and plenty for a slowly changing temperature and pressure. The period is 1000 ticks
 * of the 1 kHz tick; because the clock is about 1.1% fast (ADR-0006), a logic analyzer
 * measured 988 to 989 ms from one read to the next.
 */
#define SENSOR_TASK_STACK_WORDS      ( 256 )
#define SENSOR_TASK_PRIORITY         ( 1 )
#define SENSOR_PERIOD_MS             ( 1000U )

static StackType_t  xSensorTaskStack[ SENSOR_TASK_STACK_WORDS ];
static StaticTask_t xSensorTaskTCB;

/*
 * THE LATEST FRAME (ADR-0010). A queue of length 1 that holds the most recent frame.
 * Each period the task overwrites its content with xQueueOverwrite(), so it never fills
 * up and the task never waits. Whoever reads it (the transmit task)
 * gets the newest frame, or the one before if a new one is not there yet, never a mix: the
 * kernel copies the item inside a critical section (queue.c), so a task that preempts
 * this one sees the old frame or the new one, not a half-written one.
 *
 * The task builds the frame in its own local array first and hands over the finished
 * frame with ONE call. The queue is private to this file: the readers get an
 * accessor function, not the handle.
 *
 * With static allocation (ADR-0001) the queue's memory is ours: the kernel's record of
 * the queue (StaticQueue_t) and the storage for its items (length 1 x FRAME_SIZE bytes).
 */
static QueueHandle_t xLatestFrame;
static StaticQueue_t xLatestFrameRecord;
static uint8_t       ucLatestFrameStorage[ FRAME_SIZE ];

/*
 * THE LINE BUFFER. Every line is built here, piece by piece with the formatter, and
 * sent with ONE uart_write(), so it is a single message (a line is never cut by the
 * UART's "whole message or nothing" rule, and two tasks' lines never interleave). The
 * buffer is sized for the worst case of every line the task can print, so the
 * formatter never meets a piece that does not fit:
 *
 *   init line     "BMP280 init: OK\r\n"                                                   = 17
 *   values line   "T=" 2 + temperature 12 + " C  P=" 6 + pressure 12 + " hPa\r\n" 6   = 38
 *   error line    "BMP280 error: " 14 + longest status text 13 + " (failures: " 12
 *                 + a 32-bit counter 10 + ")\r\n" 3                                    = 52
 *
 * 64 bytes cover all three, and the buffer lives on the task's stack.
 */
#define SENSOR_LINE_SIZE    ( 64U )

/* Sends a finished line as ONE message. While a line is being built, each formatter
 * function returns the number of characters it wrote, and that moves the end of the
 * line: ulLength += format_...( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, ... ).
 * A message that does not fit in the UART's ring buffer is dropped whole (the result is
 * not used here: with one short line per second the buffer is always empty). */
static void prvSendLine( const char * pcLine, uint32_t ulLength )
{
    ( void ) uart_write( ( const uint8_t * ) pcLine, ulLength );
}

/*
 * THE TWO STATUS VOCABULARIES MEET HERE. The BMP280 module reports how it went with
 * bmp280_status_t; the frame for the gateway has its own list of codes, frame_status_t
 * (frame.h), because those numbers are a contract with another machine and must not
 * depend on the order of another module's enum. This function is the only place that
 * knows both, and says explicitly which BMP280 outcome becomes which frame code.
 *
 * There is no `default:` on purpose. With every enumerator listed and no default, the
 * compiler warns when a status is added to bmp280_status_t and not handled here (a
 * `default:` would silence that warning).
 *
 * BMP280_OK maps to FRAME_STATUS_OK, which says the numbers are valid: the caller
 * must pass the real reading then, and zeros for every other code (frame.h).
 */
static frame_status_t prvStatusFromBmp280ToFrame( bmp280_status_t xStatus )
{
    switch( xStatus )
    {
        case BMP280_OK:
            return FRAME_STATUS_OK;

        case BMP280_ERR_NACK:
            return FRAME_STATUS_SENSOR_NACK;

        case BMP280_ERR_TIMEOUT:
            return FRAME_STATUS_SENSOR_TIMEOUT;

        case BMP280_ERR_BUS:
            return FRAME_STATUS_SENSOR_BUS;

        case BMP280_ERR_ID:
            return FRAME_STATUS_SENSOR_ID;

        case BMP280_ERR_NO_DATA:
            return FRAME_STATUS_SENSOR_NO_DATA;
    }

    /* Only a value outside the enum gets here: a programming error (or corrupted
     * memory), not something to report on the wire. */
    configASSERT( pdFALSE );
    return FRAME_STATUS_SENSOR_NO_DATA;
}

/* ==========================================================================
 * THE TASK
 * ==========================================================================
 *
 * Why vTaskDelayUntil() and not vTaskDelay():
 *   vTaskDelay( 1000 ) waits 1000 ticks from the moment it is called, so the time the
 *   task spends working (the I2C transfer, the printing) is added to the period and
 *   the schedule drifts. vTaskDelayUntil( &xLastWakeTime, 1000 ) wakes the task at
 *   xLastWakeTime + 1000 and moves xLastWakeTime forward, i.e. at fixed points in
 *   time, whatever the work took.
 *
 * Why there is no `return`:
 *   A task function must never return. If it did, it would land in
 *   prvTaskExitError() in port.c, which disables interrupts and loops forever. Each
 *   task is an infinite loop with at least one blocking call, here
 *   vTaskDelayUntil(), which gives the CPU away to other tasks (Idle, in this case)
 *   instead of busy-waiting. (The I2C driver does busy-wait during a transfer, about
 *   1 ms, flagged in i2c.c.)
 *
 * The task is the ONLY one that touches the I2C bus and the BMP280, so it starts
 * the sensor itself, here, after the scheduler is running: bmp280_init() sleeps with
 * vTaskDelay() and the I2C driver asserts that the scheduler has started, so neither
 * could be called from main().
 */
static void prvSensorTask( void * pvParameters )
{
    TickType_t xLastWakeTime;
    BaseType_t xSensorReady = pdFALSE;
    uint32_t ulFailures = 0U;
    bmp280_reading_t xReading;
    bmp280_status_t xStatus;
    char acLine[ SENSOR_LINE_SIZE ];
    uint32_t ulLength;
    uint8_t aucFrame[ FRAME_SIZE ];
    frame_status_t xFrameStatus;
    uint16_t usSequence = 0U;
    int32_t lTemperatureCentiC;
    uint32_t ulPressurePa;

    ( void ) pvParameters;

    /* The first reference point for vTaskDelayUntil(): "now". */
    xLastWakeTime = xTaskGetTickCount();

    for( ;; )
    {
        /* Zero unless a reading is taken below: a frame with a status other than OK
         * carries zeros in its numbers (frame.h). */
        lTemperatureCentiC = 0;
        ulPressurePa = 0U;

        if( xSensorReady == pdFALSE )
        {
            xStatus = bmp280_init();

            if( xStatus == BMP280_OK )
            {
                xSensorReady = pdTRUE;
                xFrameStatus = FRAME_STATUS_NO_READING;
                ulLength = format_text( acLine, SENSOR_LINE_SIZE, "BMP280 init: OK\r\n" );
                prvSendLine( acLine, ulLength );
            }
        }
        else
        {
            xStatus = bmp280_read( &xReading );

            if( xStatus == BMP280_OK )
            {
                xFrameStatus = FRAME_STATUS_OK;
                lTemperatureCentiC = xReading.lTemperatureCentiC;
                ulPressurePa = xReading.ulPressurePa;

                ulLength = format_text( acLine, SENSOR_LINE_SIZE, "T=" );
                ulLength += format_fixed2( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, xReading.lTemperatureCentiC );
                ulLength += format_text( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, " C  P=" );
                ulLength += format_fixed2( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, ( int32_t ) xReading.ulPressurePa );
                ulLength += format_text( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, " hPa\r\n" );
                prvSendLine( acLine, ulLength );
            }
        }

        if( xStatus != BMP280_OK )
        {
            /* Whatever failed (the init, or the read), the next period starts with
             * a new init. */
            xSensorReady = pdFALSE;
            ulFailures++;
            xFrameStatus = prvStatusFromBmp280ToFrame( xStatus );

            ulLength = format_text( acLine, SENSOR_LINE_SIZE, "BMP280 error: " );
            ulLength += format_text( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, bmp280_status_text( xStatus ) );
            ulLength += format_text( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, " (failures: " );
            ulLength += format_u32( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, ulFailures );
            ulLength += format_text( &acLine[ ulLength ], SENSOR_LINE_SIZE - ulLength, ")\r\n" );
            prvSendLine( acLine, ulLength );
        }

        /* The frame of this period, in every case (see the top of the file). The
         * counter goes up BEFORE the frame is built, so the first frame carries 1 and 0
         * stays free for the transmit task's placeholder; it wraps from 65535 to 0 by
         * itself. xQueueOverwrite() cannot fail: it writes even when the queue is full,
         * and pdPASS is its only possible result (queue.h), so the result is not
         * checked. */
        usSequence++;
        frame_build( aucFrame, xFrameStatus, usSequence, lTemperatureCentiC, ulPressurePa );
        ( void ) xQueueOverwrite( xLatestFrame, aucFrame );

        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS( SENSOR_PERIOD_MS ) );
    }
}

/*
 * The newest frame, for the transmit task (the queue handle stays private to this
 * file). xQueuePeek() copies the item out and leaves it in the queue; the wait of 0
 * ticks means "do not block": if the queue is empty it returns at once with pdFALSE
 * (errQUEUE_EMPTY). The copy happens inside the kernel's critical section (queue.c), so
 * it cannot interleave with xQueueOverwrite() of the sensor task.
 */
BaseType_t sensor_task_latest_frame( uint8_t * pucDestination )
{
    return xQueuePeek( xLatestFrame, pucDestination, 0 );
}

/*
 * Creating the task. Arguments of xTaskCreateStatic(), in order: the task function, a
 * name (debugging only), the stack size in words, a parameter for the task (R0 on its
 * first run, unused here), the priority, the stack array and the TCB. It returns the
 * task's handle. With static allocation it can only fail if one of the two buffers is
 * NULL, which would be a programming error, so it is an assert and not run-time error
 * handling.
 *
 * Nothing runs yet: the task is only registered as READY. It starts when main() calls
 * vTaskStartScheduler().
 */
void sensor_task_create( void )
{
    TaskHandle_t xSensorTaskHandle;

    /* The latest-frame queue first: the task uses it from its first period. Length 1,
     * one item of FRAME_SIZE bytes. With static memory it can only fail on a NULL
     * buffer, so it is an assert, as for the task below. */
    xLatestFrame = xQueueCreateStatic( 1U, FRAME_SIZE, ucLatestFrameStorage, &xLatestFrameRecord );
    configASSERT( xLatestFrame != NULL );

    xSensorTaskHandle = xTaskCreateStatic( prvSensorTask,
                                           "Sensor",
                                           SENSOR_TASK_STACK_WORDS,
                                           NULL,
                                           SENSOR_TASK_PRIORITY,
                                           xSensorTaskStack,
                                           &xSensorTaskTCB );
    configASSERT( xSensorTaskHandle != NULL );
}
