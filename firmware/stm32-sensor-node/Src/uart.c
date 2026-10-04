/*
 * uart.c — USART2 driver (TX only for now).
 *
 * The USART2 peripheral shares the ST-LINK's virtual COM port: PA2 (TX) goes
 * to the ST-LINK chip, which forwards it over the USB cable to the PC, where
 * Tera Term shows it. (On the Nucleo-64 the connection PA2/PA3 <-> ST-LINK is
 * made by solder bridges, closed by default as far as I know. If no text ever
 * appears, check that first.)
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) USART2 clock + PA2 as alternate function 7                   <- DONE
 *   2) baud rate (USART2->BRR) and enable (CR1: UE, TE)             <- DONE
 *   3) send one byte (wait for TXE, write DR)                       <- DONE
 *   4) interrupt-driven send with a ring buffer
 *   5) a small decimal formatter, no printf and no malloc
 */

#include "uart.h"
#include "stm32f4xx.h"

/* ==========================================================================
 * STEP 1) CLOCKS AND THE TX PIN
 * ==========================================================================
 *
 * Two clocks must be on before any register of these peripherals can be
 * written (writes while a peripheral has no clock are silently ignored):
 *
 *   - GPIOA, on the AHB1 bus: RCC->AHB1ENR bit GPIOAEN. main.c's LED code also
 *     enables it, but this driver must not depend on that: enabling an already
 *     enabled clock is harmless.
 *   - USART2, on the APB1 bus: RCC->APB1ENR bit USART2EN (bit 17).
 *
 * Each enable is followed by a dummy read of the same register. The clock
 * takes a moment to reach the peripheral, and an access straight after the
 * enable can be ignored (an STM32F4 erratum; ST's own code does the same
 * read-back). The (void) keeps the compiler quiet; the read happens because
 * the register is volatile.
 *
 * PA2 as USART2_TX. A pin can be wired to different internal functions through
 * a multiplexer, selected in two steps:
 *
 *   MODER (2 bits per pin): 00 input, 01 output, 10 ALTERNATE FUNCTION,
 *                           11 analog. PA2 uses bits 5:4 and must become 10:
 *                           the pin is then driven by a peripheral instead of
 *                           by the ODR/BSRR registers.
 *   AFR   (4 bits per pin): WHICH peripheral function. AFR[0] covers pins 0-7,
 *                           and PA2 uses bits 11:8. The table of alternate
 *                           functions in the datasheet lists PA2 -> AF7 =
 *                           USART2_TX.
 *
 * AFR is written BEFORE MODER. The reset value of AFR is 0 (AF0, "system
 * function"), so switching MODER first would connect the pin to the wrong
 * function for a moment, with a possible glitch on the line.
 *
 * Both registers are changed with read-modify-write, not assigned: they hold
 * the settings of the other pins of port A too (PA5 is the LED, and PA13-PA15
 * are the debug interface, whose mode must not be disturbed). They are also
 * shared with main.c's LED setup; both run in main() before the scheduler
 * starts, so there is no concurrent access. As with any configuration
 * register, pin set-up belongs in one-time initialisation, not in running
 * tasks.
 *
 * What is left at reset values (as far as I know, to be confirmed in the
 * manual): output type push-pull, speed low, no pull-up/down. That is fine for
 * 115200 baud: the edges are slow. The TX line idles high, the receiver on the
 * ST-LINK side has its own pull-up.
 *
 * Expected values afterwards (with the LED setup from main.c already done,
 * which is the case when the scheduler is running):
 *   RCC->APB1ENR  (0x40023840) bit 17 set        -> 0x00020000
 *   GPIOA->AFR[0] (0x40020020) bits 11:8 = 7     -> 0x00000700
 *   GPIOA->MODER  (0x40020000)                   -> 0xA8000420
 *     reset 0xA8000000 (PA13-PA15 as alternate function for SWD),
 *     + PA5 as output (01 at bits 11:10 = 0x400), + PA2 as AF (10 at bits 5:4 = 0x20).
 */
#define UART_TX_ALTERNATE_FUNCTION    ( 7U )    /* AF7 = USART1..3 on PA2 (datasheet table) */

static void prvUartPinsAndClocks( void )
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    ( void ) RCC->AHB1ENR;

    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    ( void ) RCC->APB1ENR;

    GPIOA->AFR[ 0 ] = ( GPIOA->AFR[ 0 ] & ~GPIO_AFRL_AFSEL2 ) |
                      ( UART_TX_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL2_Pos );

    GPIOA->MODER = ( GPIOA->MODER & ~GPIO_MODER_MODER2 ) | GPIO_MODER_MODER2_1;
}

/* ==========================================================================
 * STEP 2) FRAME FORMAT, BAUD RATE AND ENABLE
 * ==========================================================================
 *
 * What goes on the wire for each byte (8N1): the line idles high; a START bit
 * (low) announces the byte; 8 data bits follow, least significant first; a
 * STOP bit (high) ends it. That is 10 bits per byte. The receiver has no clock
 * wire: it synchronises on the start bit's falling edge and samples each bit in
 * the middle of its time, so both sides must agree on the bit duration (the
 * baud rate) closely enough that the error does not add up to half a bit over
 * the 10 bits of a frame.
 *
 * Baud rate = bits per second ON THE LINE, start and stop bits included. At
 * 115200 one bit lasts 8.68 microseconds, a byte (10 bits) 86.8 microseconds,
 * so at most about 11,520 bytes per second.
 *
 * USART2->BRR holds the divisor of the peripheral's input clock (PCLK1, the
 * APB1 clock). With the default oversampling by 16, the manual defines
 *     USARTDIV = PCLK1 / (16 * baud)
 * and BRR stores USARTDIV as a fixed-point number: mantissa in bits 15:4,
 * fraction (in sixteenths) in bits 3:0. That means the whole register is just
 * USARTDIV * 16 = PCLK1 / baud, so no mantissa/fraction juggling is needed:
 *     BRR = round( PCLK1 / baud )
 * For PCLK1 = 42 MHz: 42,000,000 / 115,200 = 364.58, rounded to 365 = 0x16D
 * (mantissa 22, fraction 13). The real rate is then 42,000,000 / 365 = 115,068
 * baud: -0.11 % error, far inside what the receiver tolerates.
 *
 * PCLK1 is NOT SystemCoreClock. SystemCoreClock is HCLK (the CPU clock), and
 * USART2 sits on APB1, whose prescaler (set in clock.c) divides it again. It is
 * computed here from the real registers: HCLK >> (the prescaler's shift from
 * the standard CMSIS table, indexed by the PPRE1 field of RCC->CFGR). A useful
 * consequence: if the PLL had failed and the CPU were still on the 16 MHz HSI,
 * PCLK1 would be 8 MHz and the divisor 69 (a 0.64 % error), so the output would
 * stay readable at 115200 and could even report the failure. This means
 * uart_init() must run AFTER SystemCoreClockUpdate().
 *
 * The three control registers are ASSIGNED here, not read-modify-written. That
 * is deliberate and differs from the shared GPIO/RCC registers above: nothing
 * else uses USART2, it has only just been clocked, and writing every field
 * states the configuration explicitly instead of trusting reset values:
 *   CR1 = 0  8 data bits (M = 0), no parity (PCE = 0), oversampling by 16
 *            (OVER8 = 0), everything disabled
 *   CR2 = 0  1 stop bit
 *   CR3 = 0  no flow control, no DMA, no error interrupts
 * Then, following the order in the reference manual's transmitter procedure:
 * the baud rate is written while the USART is still disabled, the transmitter
 * enable (TE) is set, and finally the USART enable (UE). Setting TE makes the
 * peripheral send an idle frame as its first transmission (from the manual, as
 * I remember it); the receiver side is not enabled (no RE), since this driver
 * only sends.
 *
 * Nothing is sent yet: that is step 3.
 *
 * Expected values afterwards (USART2 base 0x40004400):
 *   USART2->BRR  (0x40004408) = 0x0000016D
 *   USART2->CR1  (0x4000440C) = 0x00002008   UE (bit 13) + TE (bit 3)
 *   USART2->SR   (0x40004400) = 0x000000C0   TXE (bit 7) and TC (bit 6) set:
 *                                            the data register is empty and
 *                                            nothing is being transmitted
 */
#define UART_BAUD_RATE    ( 115200U )

static void prvUartConfigure( void )
{
    uint32_t ulPclk1Hz;

    ulPclk1Hz = SystemCoreClock >> APBPrescTable[ ( RCC->CFGR & RCC_CFGR_PPRE1 ) >> RCC_CFGR_PPRE1_Pos ];

    USART2->CR1 = 0U;
    USART2->CR2 = 0U;
    USART2->CR3 = 0U;

    USART2->BRR = ( ulPclk1Hz + ( UART_BAUD_RATE / 2U ) ) / UART_BAUD_RATE;

    USART2->CR1 = USART_CR1_TE;
    USART2->CR1 |= USART_CR1_UE;
}

/* ==========================================================================
 * STEP 3) SEND ONE BYTE (TEMPORARY, BLOCKING)
 * ==========================================================================
 *
 * How a byte leaves the chip:
 *
 *   your code -> USART2->DR (data register) -> shift register -> PA2, bit by bit
 *
 * Writing a byte to DR hands it to the hardware, which moves it into an
 * internal shift register and clocks it out with the start and stop bits added.
 * As soon as the byte has moved into the shift register, DR is free again and
 * the status flag TXE ("transmit data register empty", USART2->SR bit 7) goes
 * high: the NEXT byte may be written while the previous one is still being
 * sent. Writing DR clears TXE. The other flag, TC (transmission complete, bit
 * 6), rises only after the very last bit has left the wire; it matters when
 * something must wait until everything has really been sent, which a single
 * byte does not need.
 *
 * So the rule is: wait until TXE = 1, then write DR. One byte takes 86.8
 * microseconds on the wire at 115200 baud, so if the previous byte is still in
 * the shift register the wait can last up to about that long.
 *
 * The wait is bounded, like every wait in this project: a counter of loop
 * passes, not a real time. If the USART were ever not enabled, TXE would never
 * rise and a bare `while( !TXE )` would freeze the calling task forever (and,
 * since it never blocks, starve every task below it, including Idle). A pass is
 * about 10 CPU cycles by the rough estimate used elsewhere: about 0.12
 * microseconds at 84 MHz (0.6 at 16 MHz), so 10000 passes are roughly 1.2 ms at
 * 84 MHz, some 14 times a whole frame, and still short enough not to matter.
 * On a timeout the byte is dropped and an error is returned.
 *
 * This is a busy-wait loop (the CPU spins while the hardware sends), written
 * "to get something working first" and therefore TEMPORARY: it is replaced by
 * the interrupt-driven sender in step 4. Until then it is only safe from one
 * task (see uart.h).
 *
 * The transmit path itself is not protected against a second caller: two tasks
 * could both see TXE high, and the second write to DR would overwrite the
 * first byte.
 */
#define UART_TX_TIMEOUT_PASSES    ( 10000U )

uart_status_t uart_write_byte_blocking( uint8_t ucByte )
{
    uint32_t ulTries;

    for( ulTries = UART_TX_TIMEOUT_PASSES; ulTries > 0U; ulTries-- )
    {
        if( ( USART2->SR & USART_SR_TXE ) != 0U )
        {
            USART2->DR = ucByte;
            return UART_OK;
        }
    }

    return UART_ERR_TIMEOUT;
}

/* ==========================================================================
 * PUBLIC FUNCTION
 * ========================================================================== */
void uart_init( void )
{
    /* Steps 1) and 2). Step 3) is uart_write_byte_blocking() above; step 4)
     * onward are added one at a time. */
    prvUartPinsAndClocks();
    prvUartConfigure();
}
