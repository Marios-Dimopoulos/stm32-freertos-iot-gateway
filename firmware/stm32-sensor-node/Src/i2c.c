/*
 * i2c.c — I2C1 master driver (BMP280 on PB8 = SCL, PB9 = SDA).
 *
 * Design decisions (to be recorded in an ADR once the driver exists):
 *   - register-oriented API (read/write N bytes starting at a register of a
 *     device) plus a probe, so the repeated START and the time-critical receive
 *     sequence live in ONE place;
 *   - the caller passes the 7-bit device address (0x76); the driver adds the R/W bit;
 *   - distinct error codes (no answer, timeout, bus error);
 *   - polling with BOUNDED waits, in the calling task, using the RTOS tick for
 *     real-time limits (the driver is task-only, after the scheduler starts);
 *     a blocking loop, therefore flagged as such;
 *   - the few register operations that must meet the byte window are protected
 *     by a short critical section;
 *   - ONE task owns the bus (the sensor task): no mutex;
 *   - after an error the driver cleans up by itself (STOP, flags, reset of the
 *     peripheral) so the next call starts from a clean state;
 *   - 100 kHz standard mode.
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) clocks, peripheral reset, pins (PB8/PB9 = AF4, open-drain)      <- DONE
 *   2) timing (CR2.FREQ, CCR, TRISE) and enable (CR1.PE)
 *   3) probe: START + address, tell ACK from NACK (testable without the sensor)
 *   4) write to a register
 *   5) read from a register (1 byte, then N bytes) -> chip ID 0x58
 *   6) error handling and recovery, tick-based timeouts
 */

#include "i2c.h"
#include "stm32f4xx.h"

/* ==========================================================================
 * STEP 1) CLOCKS, RESET AND PINS
 * ==========================================================================
 *
 * Clocks (writes to a peripheral without a clock are silently ignored):
 *   - GPIOB, on the AHB1 bus: RCC->AHB1ENR bit GPIOBEN (PB8 and PB9 are port B);
 *   - I2C1, on the APB1 bus: RCC->APB1ENR bit I2C1EN (bit 21).
 * Each enable is followed by a dummy read of the same register: the clock takes a
 * moment to reach the peripheral, and an access straight after the enable can be
 * ignored (an STM32F4 erratum). See uart.c.
 *
 * Peripheral reset. I2C1->CR1 bit SWRST puts the whole peripheral back to its
 * initial state; it must be set and then cleared again. As I understand the
 * reference manual, the I2C lines must be released and the bus free when the
 * reset bit is cleared, so the reset is done BEFORE the pins are handed to the
 * peripheral: at that point they are plain inputs (their reset state), and
 * nothing of ours drives them. This also leaves the peripheral in a known state
 * after a debugger reset in the middle of a transfer.
 *
 * The pins. An I2C line is OPEN-DRAIN: a device can only pull the line low; it
 * never drives it high. A resistor to 3.3 V (a pull-up) lifts the line to 1 when
 * everyone lets go. Two devices can therefore never short-circuit each other,
 * and the pull-ups are essential.
 *   OTYPER  1 = open-drain (the reset value 0 would be push-pull, which would
 *           actively drive the line high and fight with the slave).
 *   PUPDR   01 = internal pull-up ON, as a BACKUP. Most BMP280 breakout boards
 *           carry their own pull-ups (4.7-10 kOhm); in parallel with these the
 *           internal ones (about 40 kOhm, from memory) are harmless. If the board
 *           has none, they at least give the bus a chance to work, but they are
 *           weak: with about 50 pF of wiring the rise time would be about 1.7
 *           microseconds, against the 1 microsecond allowed in standard mode.
 *           If the bus is flaky, add external 4.7 kOhm resistors from SDA and
 *           SCL to 3.3 V.
 *   OSPEEDR 11 = very high. The speed setting shapes the FALLING edge of an
 *           open-drain output; the I2C specification limits the fall time, and
 *           ST's own examples use the highest setting for I2C pins (from memory).
 *           The rising edge is set by the pull-up, so there is no EMI concern.
 *   AFR[1]  alternate function 4 = I2C1 on PB8 (bits 3:0) and PB9 (bits 7:4);
 *           pins 8-15 are in AFR[1] (AFR[0] has pins 0-7). From the alternate
 *           function table of the datasheet, as I remember it.
 *   MODER   10 = alternate function: the pin is handed to the peripheral.
 *
 * ORDER matters: open-drain and the pull-up first, then the function number, and
 * MODER LAST. If MODER were switched first, the pin would be connected to the
 * peripheral for a moment while still configured push-pull with function 0, and
 * could drive the bus line high against another device. All of these registers
 * are changed with read-modify-write, because they hold the settings of the
 * other pins of port B too (PB3 and PB4 are debug pins whose settings must not
 * be disturbed).
 *
 * Nothing is enabled yet: the peripheral is still off (PE = 0), so both lines
 * stay released.
 *
 * Expected values afterwards (reset values quoted from memory; only the bits of
 * PB8 and PB9 are ours):
 *   RCC->AHB1ENR     (0x40023830)  0x00000003   GPIOA + GPIOB clocks
 *   RCC->APB1ENR     (0x40023840)  0x00220000   USART2 (bit 17) + I2C1 (bit 21)
 *   GPIOB->MODER     (0x40020400)  0x000A0280   PB8, PB9 = 10 (reset 0x280: PB3, PB4)
 *   GPIOB->OTYPER    (0x40020404)  0x00000300   PB8, PB9 open-drain
 *   GPIOB->OSPEEDR   (0x40020408)  0x000F00C0   PB8, PB9 = 11 (reset 0xC0: PB3)
 *   GPIOB->PUPDR     (0x4002040C)  0x00050100   PB8, PB9 = 01 (reset 0x100: PB4)
 *   GPIOB->AFR[1]    (0x40020424)  0x00000044   AF4 on PB8 and PB9
 *   I2C1->CR1        (0x40005400)  0x00000000   reset released, peripheral off
 */
#define I2C_PIN_ALTERNATE_FUNCTION    ( 4U )    /* AF4 = I2C1..3 on PB8/PB9 (datasheet table) */

static void prvI2cClocksResetAndPins( void )
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    ( void ) RCC->AHB1ENR;

    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    ( void ) RCC->APB1ENR;

    I2C1->CR1 |= I2C_CR1_SWRST;
    I2C1->CR1 &= ~I2C_CR1_SWRST;

    GPIOB->OTYPER |= GPIO_OTYPER_OT8 | GPIO_OTYPER_OT9;

    GPIOB->PUPDR = ( GPIOB->PUPDR & ~( GPIO_PUPDR_PUPD8 | GPIO_PUPDR_PUPD9 ) ) |
                   ( GPIO_PUPDR_PUPD8_0 | GPIO_PUPDR_PUPD9_0 );

    GPIOB->OSPEEDR |= GPIO_OSPEEDR_OSPEED8 | GPIO_OSPEEDR_OSPEED9;

    GPIOB->AFR[ 1 ] = ( GPIOB->AFR[ 1 ] & ~( GPIO_AFRH_AFSEL8 | GPIO_AFRH_AFSEL9 ) ) |
                       ( ( I2C_PIN_ALTERNATE_FUNCTION << GPIO_AFRH_AFSEL8_Pos ) |
                         ( I2C_PIN_ALTERNATE_FUNCTION << GPIO_AFRH_AFSEL9_Pos ) );

    GPIOB->MODER = ( GPIOB->MODER & ~( GPIO_MODER_MODER8 | GPIO_MODER_MODER9 ) ) |
                   ( GPIO_MODER_MODER8_1 | GPIO_MODER_MODER9_1 );
}

/* ==========================================================================
 * PUBLIC FUNCTION
 * ========================================================================== */
void i2c_init( void )
{
    /* Step 1). Steps 2) onward are added one at a time. */
    prvI2cClocksResetAndPins();
}
