/*
 * uart.h — USART2 driver: debug text to the PC over the ST-LINK's virtual COM
 * port (PA2 = TX), shown in Tera Term.
 *
 * WORK IN PROGRESS, built one step at a time (see uart.c). So far uart_init()
 * sets up the clocks and the TX pin, configures 115200 8N1 and enables the
 * transmitter, and one byte can be sent with a temporary blocking call.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

typedef enum
{
    UART_OK = 0,
    UART_ERR_TIMEOUT    /* the transmitter did not become ready in time; the byte was NOT sent */
} uart_status_t;

/* Call once from main(), before the scheduler starts and AFTER
 * SystemCoreClockUpdate(): the baud-rate divisor is computed from the real bus
 * clock at that moment. */
void uart_init( void );

/*
 * Sends one byte, waiting (busy-waiting) until the transmitter can take it.
 *
 * TEMPORARY, BLOCKING. This is a polling loop, kept only to prove the wiring
 * and the baud rate; it is replaced in a later step by the interrupt-driven
 * sender with a ring buffer. Rules until then:
 *   - call it from ONE task only: two callers could both see the data register
 *     empty and the second byte would overwrite the first;
 *   - never from an interrupt handler.
 * The wait is bounded, so it can never freeze the caller: on a timeout it
 * returns UART_ERR_TIMEOUT and the byte is dropped.
 */
uart_status_t uart_write_byte_blocking( uint8_t ucByte );

#endif /* UART_H */
