/*
 * transmit_task.c — the transmit task (ADR-0010, decisions 5 and 6).
 *
 * WHAT IT IS FOR. The frame the Pi reads is prepared in a buffer that the DMA reads on
 * its own. After every transaction the buffer must be refilled and the DMA started
 * again (normal mode: the stream stops by itself after 14 bytes). That is the job of
 * this task, and it is a task and not code in the interrupt so that the interrupt stays
 * a few lines long (the work involves a bounded wait, a copy and several register
 * writes).
 *
 * THE TRIGGER. The end of a transaction is the NSS pin rising. EXTI line 4 turns that
 * edge into an interrupt (spi.c); the handler calls
 * transmit_task_end_of_transaction_from_isr(), which sends this task a NOTIFICATION.
 *
 * WHY A NOTIFICATION AND NOT A QUEUE OR A SEMAPHORE. Every task already has a
 * notification slot inside its TCB (ulNotifiedValue and ucNotifyState), so nothing has
 * to be created: no queue record, no storage. And there is no data to carry, only "a
 * transaction ended". The slot is a counter: each "give" adds 1, and the task's "take"
 * blocks while it is 0.
 *
 * WHY THE TAKE CLEARS THE COUNT (xClearCountOnExit = pdTRUE). If two transactions end
 * before this task has run, one wake-up is enough: refilling the buffer is the same
 * whatever the number of ends. The number of ends is still counted, in spi.c
 * (ulNssRisingCount).
 *
 * WHY IT WAITS FOREVER (portMAX_DELAY). The rhythm of the system is the Pi's polling.
 * A timeout would only make sense if the task had something to do without a
 * transaction, and nothing needs that.
 *
 * PRIORITY 2, ABOVE THE SENSOR TASK (priority 1): the refill must finish before the
 * Pi's next chip select, so when the interrupt wakes this task it must run at once,
 * even if the sensor task is in the middle of an I2C transfer.
 *
 * HOW MUCH TIME IT HAS (logic analyzer, 2026-10-20, ADR-0010). With back-to-back reads
 * from Python, NSS stays high for 154 us at the least (209 us typical) between two
 * transactions, and 30000 such reads were all good, so the refill completes inside that
 * time. The refill time itself was not measured.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "transmit_task.h"
#include "sensor_task.h"
#include "frame.h"
#include "spi.h"

/* ==========================================================================
 * MEMORY AND PARAMETERS
 * ==========================================================================
 *
 * Static allocation (ADR-0001): the stack and the TCB are ours, file-level static so
 * they live in .bss for the whole run (as for the sensor task).
 *
 * The stack is configMINIMAL_STACK_SIZE words (128, 512 bytes). MEASURED (ADR-0005,
 * 2026-10-18, by the 0xA5 pattern in the debugger after 60 reads from the Pi): 356 bytes
 * untouched, so the peak is 39 words (156 bytes) and 128 words is 3.3 times that. The
 * rule of ADR-0005 (peak plus 50%) would allow 59 words; I keep 128 because RAM is
 * plentiful. The placeholder path (frame_build() when the queue is still empty) did not
 * run in that test; the sensor task, which also calls frame_build(), covers its depth.
 * Repeat the measurement if the task's code changes.
 */
#define TRANSMIT_TASK_STACK_WORDS    ( configMINIMAL_STACK_SIZE )
#define TRANSMIT_TASK_PRIORITY       ( 2 )

static StackType_t  xTransmitTaskStack[ TRANSMIT_TASK_STACK_WORDS ];
static StaticTask_t xTransmitTaskTCB;

/* The handle the interrupt needs to notify the task. Private to this file: the
 * interrupt gets only the function declared in the header. */
static TaskHandle_t xTransmitTaskHandle;

/* How many times the task has woken up: a statistic, to be watched in the debugger next
 * to ulNssRisingCount in spi.c (after each read made by the Pi both go up by one). It
 * can be lower than the number of ends if two ended before the task ran (the count is
 * coalesced, see above). */
static volatile uint32_t ulWakeUps;

/* ==========================================================================
 * THE TASK
 * ========================================================================== */
static void prvTransmitTask( void * pvParameters )
{
    /* The frame is fetched into this local array and then handed to spi_load_frame(),
     * which copies it into the DMA buffer: each module only sees its own memory. */
    uint8_t aucFrame[ FRAME_SIZE ];

    ( void ) pvParameters;

    for( ;; )
    {
        /* Blocks (no CPU used) until the interrupt gives a notification. pdTRUE: reset
         * the count to 0 on the way out. The returned count is not needed. */
        ( void ) ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

        ulWakeUps++;

        /* The newest frame of the sensor task. If it has produced none yet (a
         * transaction ended in the first ~20 ms after start-up), send the placeholder
         * again: "no reading yet", sequence 0. */
        if( sensor_task_latest_frame( aucFrame ) == pdFALSE )
        {
            frame_build( aucFrame, FRAME_STATUS_NO_READING, 0U, 0, 0U );
        }

        /* Stop the stream, copy the frame into the DMA buffer, start the stream again.
         * The result is not used here: a failure (the stream did not stop in time) is
         * counted inside spi.c (spi_reload_failures()) and nothing else can be done; the
         * next transaction tries again. */
        ( void ) spi_load_frame( aucFrame );
    }
}

void transmit_task_create( void )
{
    xTransmitTaskHandle = xTaskCreateStatic( prvTransmitTask,
                                             "Transmit",
                                             TRANSMIT_TASK_STACK_WORDS,
                                             NULL,
                                             TRANSMIT_TASK_PRIORITY,
                                             xTransmitTaskStack,
                                             &xTransmitTaskTCB );
    configASSERT( xTransmitTaskHandle != NULL );
}

void transmit_task_end_of_transaction_from_isr( BaseType_t * pxHigherPriorityTaskWoken )
{
    vTaskNotifyGiveFromISR( xTransmitTaskHandle, pxHigherPriorityTaskWoken );
}
