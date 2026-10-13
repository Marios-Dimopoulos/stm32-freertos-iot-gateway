/*
 * transmit_task.h — the task that prepares the next frame for the Pi after every
 * transaction (ADR-0010).
 *
 * It sleeps until the end of a transaction (NSS rising, signalled by the EXTI
 * interrupt in spi.c) and then re-arms the DMA with the newest frame. See
 * transmit_task.c.
 */

#ifndef TRANSMIT_TASK_H
#define TRANSMIT_TASK_H

#include "FreeRTOS.h"

/* Creates the task (static memory only: ADR-0001). Call it once from main(), before
 * spi_start() and before vTaskStartScheduler(). Nothing runs until the scheduler
 * starts. It MUST be called before spi_start(): that is what enables the interrupt that
 * notifies this task. */
void transmit_task_create( void );

/*
 * Tells the task that a transaction has ended. For the EXTI interrupt handler ONLY (it
 * uses a ...FromISR() kernel call, so the caller must be an interrupt at level 5 or
 * numerically higher, ADR-0004). *pxHigherPriorityTaskWoken is set to pdTRUE if this
 * woke a task more urgent than the one that was running: pass it to
 * portYIELD_FROM_ISR() at the end of the handler.
 */
void transmit_task_end_of_transaction_from_isr( BaseType_t * pxHigherPriorityTaskWoken );

#endif /* TRANSMIT_TASK_H */
