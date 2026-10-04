/*
 * uart.h — USART2 driver: debug text to the PC over the ST-LINK's virtual COM
 * port (PA2 = TX), shown in Tera Term.
 *
 * WORK IN PROGRESS, built one step at a time (see uart.c). So far uart_init()
 * sets up the clocks and the TX pin, configures 115200 8N1 and enables the
 * transmitter, and bytes are sent in the background by the TX interrupt
 * through a ring buffer.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

typedef enum
{
    UART_OK = 0,
    UART_ERR_FULL       /* the transmit buffer had no room; the byte was NOT stored */
} uart_status_t;

/* Call once from main(), before the scheduler starts and AFTER
 * SystemCoreClockUpdate(): the baud-rate divisor is computed from the real bus
 * clock at that moment. */
void uart_init( void );

/*
 * Queues one byte for transmission and returns immediately: the byte is copied
 * into a ring buffer and the USART2 interrupt sends it in the background.
 *
 * Call it from TASKS only, and only after the scheduler has started: it uses a
 * FreeRTOS critical section, which must not be used from an interrupt handler
 * and is not valid before vTaskStartScheduler(). Several tasks may call it;
 * each BYTE is stored atomically, but bytes of different callers can still
 * interleave in the text (whole-message writes come in a later step).
 *
 * If the buffer is full the byte is dropped and UART_ERR_FULL is returned.
 */
uart_status_t uart_write_byte( uint8_t ucByte );

#endif /* UART_H */
