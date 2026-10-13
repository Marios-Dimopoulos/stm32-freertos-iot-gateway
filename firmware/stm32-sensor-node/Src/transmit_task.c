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
 * transaction, and nothing needs that yet.
 *
 * PRIORITY 2, ABOVE THE SENSOR TASK (priority 1): the refill must finish before the
 * Pi's next chip select, so when the interrupt wakes this task it must run at once,
 * even if the sensor task is in the middle of an I2C transfer.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "transmit_task.h"

/* ==========================================================================
 * MEMORY AND PARAMETERS
 * ==========================================================================
 *
 * Static allocation (ADR-0001): the stack and the TCB are ours, file-level static so
 * they live in .bss for the whole run (as for the sensor task).
 *
 * The stack is configMINIMAL_STACK_SIZE words (128, 512 bytes) for now: the body is
 * tiny. It is NOT a measured size. Per ADR-0005 it has to be measured once the task
 * does its real work (uxTaskGetStackHighWaterMark), and the figure recorded there.
 */
#define TRANSMIT_TASK_STACK_WORDS    ( configMINIMAL_STACK_SIZE )
#define TRANSMIT_TASK_PRIORITY       ( 2 )

static StackType_t  xTransmitTaskStack[ TRANSMIT_TASK_STACK_WORDS ];
static StaticTask_t xTransmitTaskTCB;

/* The handle the interrupt needs to notify the task. Private to this file: the
 * interrupt gets only the function declared in the header. */
static TaskHandle_t xTransmitTaskHandle;

/* How many times the task has woken up. For this step only, to be watched in the
 * debugger next to ulNssRisingCount in spi.c: after each read made by the Pi both go up
 * by one. It can be lower than the number of ends if two ended before the task ran
 * (the count is coalesced, see above). */
static volatile uint32_t ulWakeUps;

/* ==========================================================================
 * THE TASK
 * ========================================================================== */
static void prvTransmitTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        /* Blocks (no CPU used) until the interrupt gives a notification. pdTRUE: reset
         * the count to 0 on the way out. The returned count is not needed. */
        ( void ) ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

        ulWakeUps++;

        /* The re-arm goes here (the next piece). */
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
