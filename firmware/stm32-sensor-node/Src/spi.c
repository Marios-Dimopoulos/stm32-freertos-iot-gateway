/*
 * spi.c — SPI1 as a slave, towards the Raspberry Pi (ADR-0010).
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) clock, pins, SPI1 configured as a slave, left disabled        <- THIS FILE NOW
 *   2) DMA2 stream 3 feeding SPI1->DR, armed once, SPI enabled
 *   3) re-arming after every transaction (EXTI on NSS + the transmit task)
 *
 * WHY A SLAVE. The Pi's Linux SPI driver (Phase 4) sits behind a master controller, so
 * the Pi clocks and the STM32 answers (ADR-0010, decision 1). In slave mode the clock
 * comes from the master, and the baud-rate bits BR[2:0] of CR1 do not affect the
 * transfer rate (RM0383 p. 555). The 6 MHz limit of the project is therefore enforced
 * on the Pi's side, not here.
 */

#include "FreeRTOS.h"
#include "spi.h"
#include "stm32f4xx.h"

/* ==========================================================================
 * STEP 1) CLOCK, PINS AND THE SPI CONFIGURATION
 * ==========================================================================
 *
 * Clocks. SPI1 sits on the APB2 bus: RCC->APB2ENR bit SPI1EN (bit 12, RM0383). The
 * GPIOA clock is already on (uart_init() enabled it), but this module does not rely on
 * that: it enables it itself, as the I2C driver does for GPIOB. Each enable is followed
 * by a dummy read of the same register (an STM32F4 erratum: an access straight after
 * the enable can be ignored), as in the other drivers.
 *
 * No peripheral reset (RCC_APB2RSTR) is done: SPI1 has just been clocked for the first
 * time since reset, so it is in its reset state.
 *
 * THE FOUR PINS (all on port A, alternate function 5 = SPI1:
 *
 *   PA4 NSS   input  PULL-UP. NSS is active low, so "high" means "not selected". With
 *                    the Pi disconnected a floating NSS could read as selected at
 *                    random; the pull-up makes it a defined "not selected".
 *   PA5 SCK   input  PULL-DOWN. The manual (p. 554) says the idle state of SCK must
 *                    match the polarity selected in CR1: pulling it down if CPOL = 0.
 *                    (PA5 is also wired to the Nucleo's user LED LD2, as far as I
 *                    remember; once SCK is driven by the Pi the LED will flicker,
 *                    which is harmless.)
 *   PA6 MISO  OUTPUT push-pull, no pull, FAST speed. The only pin we drive. The speed
 *                    setting limits how fast the output edges can be. The low setting
 *                    is meant for slow signals (such as the 115200 baud UART), so for
 *                    a clock of several MHz a faster setting is needed; "fast" is
 *                    enough for 4 to 6 MHz, and radiates less than the "high" setting.
 *                    (The frequencies behind each setting are in the datasheet, which
 *                    is not on this PC: not verified.)
 *   PA7 MOSI  input  no pull. The master always drives it while we are selected.
 *
 * Output type and speed only matter for an OUTPUT, so they are set for PA6 only. The
 * pulls are set for all four so each pin's state is stated, not inherited.
 *
 * ORDER (same as the UART and I2C drivers): everything that describes the pin (output
 * type, pull, speed, and the function number in AFR) is written BEFORE MODER, and
 * MODER is written LAST, because MODER is what connects the pin to the peripheral.
 * Here the SPI's own registers (CR1, CR2) are written before MODER too, so that by the
 * time the pins are connected to SPI1 it is already configured (and disabled).
 *
 * All the GPIO registers are changed with read-modify-write, never assigned: they
 * also hold the settings of the other pins of port A (PA2 is the UART's TX, PA13 to
 * PA15 the debug interface).
 *
 * CR1 and CR2 of SPI1 are ASSIGNED, as the UART's control registers are: nothing else
 * uses SPI1, and writing every field states the whole configuration. Their reset
 * value is 0 for CR1, which is already what we want, but the intent is written out:
 *
 *   CR1 = 0x0000
 *     CPHA = 0, CPOL = 0   mode 0: SCK idles low, data sampled on the RISING edge and
 *                          changed on the falling edge (the Pi's spidev default too)
 *     MSTR = 0             SLAVE
 *     BR   = 000           irrelevant in slave mode (see above)
 *     SPE  = 0             DISABLED for now (see spi.h)
 *     LSBFIRST = 0         most significant bit first
 *     SSM = 0, SSI = 0     hardware NSS: the pin PA4 is the chip select
 *     RXONLY = 0, BIDIMODE = 0, BIDIOE = 0   ordinary two-wire full duplex
 *     DFF = 0              8-bit data frames
 *     CRCEN = 0            the CRC is done in software, in the frame (frame.c)
 *   CR2 = 0x0002
 *     TXDMAEN = 1          the SPI asks the DMA for the next byte every time its
 *                          transmit register empties (RM0383 p. 571)
 *     everything else 0    no interrupts (TXEIE, RXNEIE, ERRIE), SSOE = 0 (that one is
 *                          for masters), FRF = 0 (Motorola format, not TI)
 *
 * Expected values afterwards (reset values of port A quoted from RM0383):
 *   RCC->APB2ENR   (0x40023844) bit 12 set
 *   GPIOA->MODER   (0x40020000) -> 0xA800AA20
 *     = 0xA8000020 (the UART's PA2 and the debug pins) + PA4..PA7 as AF (10 each = 0xAA00)
 *   GPIOA->OTYPER  (0x40020004) -> 0x00000000  PA6 push-pull
 *   GPIOA->OSPEEDR (0x40020008) -> 0x0C002000  reset 0x0C000000 + PA6 fast (10 at 13:12)
 *   GPIOA->PUPDR   (0x4002000C) -> 0x64000900  reset 0x64000000 + PA4 pull-up (0x100)
 *                                              + PA5 pull-down (0x800)
 *   GPIOA->AFR[0]  (0x40020020) -> 0x55550700  PA2 = 7 (UART), PA4..PA7 = 5
 *   SPI1->CR1      (0x40013000) -> 0x0000
 *   SPI1->CR2      (0x40013004) -> 0x0002
 *   SPI1->SR       (0x40013008) -> 0x0002      TXE set (not meaningful while disabled)
 *
 * NOT KNOWN: what level MISO has while the pin is connected to SPI1 but the SPI is
 * disabled; I did not find it in the manual. Nothing is connected in this step.
 */
#define SPI_PIN_ALTERNATE_FUNCTION    ( 5U )    /* AF5 = SPI1 on PA4..PA7 */

void spi_init( void )
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    ( void ) RCC->AHB1ENR;

    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    ( void ) RCC->APB2ENR;

    /* MISO is the only output. */
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT6;

    GPIOA->OSPEEDR = ( GPIOA->OSPEEDR & ~GPIO_OSPEEDR_OSPEED6 ) | GPIO_OSPEEDR_OSPEED6_1;

    /* NSS pull-up, SCK pull-down, MISO and MOSI no pull. */
    GPIOA->PUPDR = ( GPIOA->PUPDR & ~( GPIO_PUPDR_PUPD4 | GPIO_PUPDR_PUPD5 |
                                       GPIO_PUPDR_PUPD6 | GPIO_PUPDR_PUPD7 ) ) |
                   GPIO_PUPDR_PUPD4_0 | GPIO_PUPDR_PUPD5_1;

    GPIOA->AFR[ 0 ] = ( GPIOA->AFR[ 0 ] & ~( GPIO_AFRL_AFSEL4 | GPIO_AFRL_AFSEL5 |
                                             GPIO_AFRL_AFSEL6 | GPIO_AFRL_AFSEL7 ) ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL4_Pos ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL5_Pos ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL6_Pos ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL7_Pos );

    SPI1->CR1 = 0U;
    SPI1->CR2 = SPI_CR2_TXDMAEN;

    /* MODER last: this connects the four pins to SPI1. */
    GPIOA->MODER = ( GPIOA->MODER & ~( GPIO_MODER_MODER4 | GPIO_MODER_MODER5 |
                                       GPIO_MODER_MODER6 | GPIO_MODER_MODER7 ) ) |
                   GPIO_MODER_MODER4_1 | GPIO_MODER_MODER5_1 |
                   GPIO_MODER_MODER6_1 | GPIO_MODER_MODER7_1;
}
