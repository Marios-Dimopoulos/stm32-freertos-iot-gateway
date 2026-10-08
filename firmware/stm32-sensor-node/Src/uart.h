/*
 * uart.h — USART2 driver: debug text to the PC over the ST-LINK's virtual COM
 * port (PA2 = TX), shown in Tera Term.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

typedef enum
{
    UART_OK = 0,
    UART_ERR_FULL       /* not enough room in the transmit buffer: NOTHING was stored (counted as a drop) */
} uart_status_t;

/* Call once from main(), before the scheduler starts and AFTER
 * SystemCoreClockUpdate(): the baud-rate divisor is computed from the real bus
 * clock at that moment. */
void uart_init( void );

/*
 * Queues a message for transmission and returns immediately: the bytes are
 * copied into a ring buffer and the USART2 interrupt sends them in the
 * background.
 *
 * ALL OR NOTHING. If the whole message fits it is stored completely; if it does
 * not, NOTHING is stored, the drop counter is incremented and UART_ERR_FULL is
 * returned. A message is never cut in the middle, and messages from different
 * tasks never interleave inside the buffer. A message longer than the whole
 * buffer (256 bytes) can never fit and is always dropped. A length of 0 does
 * nothing.
 *
 * Call it from TASKS only, and only after the scheduler has started: it uses a
 * FreeRTOS critical section, which must not be used from an interrupt handler
 * and is not valid before vTaskStartScheduler(). It never waits.
 */
uart_status_t uart_write( const uint8_t * pucData, uint32_t ulLength );

/* Number of messages dropped so far because the buffer had no room. */
uint32_t uart_dropped_messages( void );

#endif /* UART_H */
