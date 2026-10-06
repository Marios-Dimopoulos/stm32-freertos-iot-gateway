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
 *   2) timing (CR2.FREQ, CCR, TRISE) and enable (CR1.PE)               <- DONE
 *   3) probe: START + address, tell ACK from NACK (testable without the sensor)   <- DONE
 *   4) write to a register
 *   5) read from a register (1 byte, then N bytes) -> chip ID 0x58
 *   6) error handling and recovery, tick-based timeouts
 */

#include "FreeRTOS.h"
#include "task.h"
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
 * STEP 2) TIMING AND ENABLE
 * ==========================================================================
 *
 * The peripheral generates the SCL ticks from its input clock (PCLK1, the APB1
 * clock), so it has to be told that clock's frequency and the speed we want. Three
 * registers, with the formulas from the reference manual:
 *
 *   CR2.FREQ   bits 5:0  PCLK1 in MHz, as a whole number (valid 2..50 MHz).
 *                        42 MHz -> 42.  The peripheral uses it to derive its
 *                        internal timing (and to validate TRISE).
 *   CCR        bits 11:0 the clock divisor. In standard mode (bit F/S = 0) SCL
 *                        is high for CCR clocks of PCLK1 and low for CCR clocks:
 *                        a period of 2 * CCR * Tpclk1, so
 *                            CCR = PCLK1 / (2 * 100 kHz) = 42 MHz / 200 kHz = 210.
 *                        100 kHz -> a bit lasts 10 microseconds.
 *   TRISE      bits 5:0  the maximum rise time of SCL/SDA, in PCLK1 clocks, plus
 *                        one. Standard mode allows 1000 ns, i.e. 1000 ns / 23.8 ns
 *                        = 42 clocks, so TRISE = 42 + 1 = 43 (= FREQ + 1). It does
 *                        not slow anything down: it only tells the peripheral how
 *                        long to wait for SCL to actually rise before it starts
 *                        counting the high time, because SCL is lifted by the
 *                        pull-up resistor and rises slowly, and a slave may hold it
 *                        low (clock stretching).
 *
 * PCLK1 is computed from the real clock registers, exactly as in uart.c (the
 * CMSIS SystemCoreClock is HCLK, and APB1 sits behind its own prescaler), so the
 * result is right whatever clock the system ended up with. This is why i2c_init()
 * must run AFTER SystemCoreClockUpdate().
 *
 * CCR and TRISE can only be written while the peripheral is DISABLED (PE = 0);
 * after the reset in step 1 it is. PE is set LAST, once everything is in place.
 *
 * Other registers:
 *   CR2   is ASSIGNED, so its other fields (interrupt enables, DMA, LAST) are 0:
 *         this driver polls, with no interrupts and no DMA.
 *   OAR1  our own slave address. A master does not use one, but the reference manual
 *         asks software to keep bit 14 of this register at 1, and the ST library
 *         does that too (it writes 0x4000 | own address). We write exactly that,
 *         with a zero address.
 *   CR1.ACK, START, STOP are not touched here: ACK is cleared by the hardware
 *         while PE = 0 and is managed per transaction when receiving (step 5).
 *
 * A bad clock must not be accepted silently: if PCLK1 were outside 2..50 MHz or CCR
 * under its minimum of 4, the bus would be mis-timed. This cannot be checked at build
 * time (the value comes from the registers at run time), so it is an assertion: a
 * wrong clock set-up is a programming error, not something to recover from.
 *
 * Expected values afterwards (I2C1 base 0x40005400):
 *   CR1    (0x40005400) = 0x00000001   PE
 *   CR2    (0x40005404) = 0x0000002A   FREQ = 42
 *   OAR1   (0x40005408) = 0x00004000
 *   CCR    (0x4000541C) = 0x000000D2   210, standard mode (F/S = 0)
 *   TRISE  (0x40005420) = 0x0000002B   43
 *   SR1    (0x40005414) = 0x00000000
 *   SR2    (0x40005418) = 0x00000000   not busy, not master (BUSY would mean one of
 *                                      the lines is being held low)
 */
#define I2C_BUS_SPEED_HZ        ( 100000U )
#define I2C_OAR1_KEEP_AT_ONE    ( 1UL << 14 )    /* reference manual: "should always be kept at 1 by software" */

static void prvI2cTimingAndEnable( void )
{
    uint32_t ulPclk1Hz;
    uint32_t ulFreqMhz;
    uint32_t ulCcr;

    ulPclk1Hz = SystemCoreClock >> APBPrescTable[ ( RCC->CFGR & RCC_CFGR_PPRE1 ) >> RCC_CFGR_PPRE1_Pos ];
    ulFreqMhz = ulPclk1Hz / 1000000U;
    ulCcr = ulPclk1Hz / ( 2U * I2C_BUS_SPEED_HZ );

    configASSERT( ( ulFreqMhz >= 2U ) && ( ulFreqMhz <= 50U ) );
    configASSERT( ulCcr >= 4U );

    I2C1->CR1 &= ~I2C_CR1_PE;

    I2C1->CR2 = ulFreqMhz;
    I2C1->CCR = ulCcr;
    I2C1->TRISE = ulFreqMhz + 1U;
    I2C1->OAR1 = I2C_OAR1_KEEP_AT_ONE;

    I2C1->CR1 |= I2C_CR1_PE;
}

/* ==========================================================================
 * STEP 3) THE PROBE: START + ADDRESS, ACK OR NACK
 * ==========================================================================
 *
 * On the wire (see the protocol notes): START, then the address byte (7-bit
 * address plus the R/W bit, here 0 = write), then the 9th clock on which the
 * addressed device pulls SDA low (ACK) or nobody does (NACK). A probe stops
 * right there with a STOP. The peripheral generates the signals; the software
 * orders each phase and waits for the flag that says it happened:
 *
 *   1. wait until the bus is free (SR2.BUSY = 0), then set CR1.START;
 *   2. wait for SR1.SB ("start bit sent"), then write the address byte to DR.
 *      (SB is cleared by reading SR1 and then writing DR; the wait loop's read
 *      of SR1 is the first half, the write to DR the second.)
 *   3. wait for ONE of: SR1.ADDR (the address was acknowledged), SR1.AF
 *      (acknowledge failure: NACK), SR1.BERR / SR1.ARLO (bus error, arbitration
 *      lost);
 *   4. ADDR is cleared by READING SR1 and then READING SR2 (not by writing
 *      anything; both reads are volatile register accesses, so the compiler
 *      keeps them). AF, BERR and ARLO are cleared by writing 0 to them.
 *   5. in every case set CR1.STOP (after a NACK the manual requires software to
 *      generate it; it also releases the bus after an ACK or an error), and wait
 *      until SR2.BUSY drops, so the next transaction starts on a free bus.
 *
 * TIMEOUTS ARE IN REAL TIME, not in loop passes. This driver runs in a task, after
 * the scheduler has started, so every wait uses the RTOS tick: at most
 * I2C_WAIT_TIMEOUT_TICKS ticks, i.e. between 9 and 10 ms with the 1 ms tick. A
 * phase takes well under a millisecond (a byte with its ACK lasts 90 microseconds
 * at 100 kHz), so this is a limit about a hundred times larger than needed: it
 * only fires when something is really stuck. The elapsed time is computed with an
 * unsigned subtraction, which stays correct when the tick counter wraps. Two
 * consequences: the waits must not be placed inside a critical section (the tick
 * does not advance while interrupts are masked), and the functions work only after
 * the scheduler has started.
 *
 * A wait that expires checks the flag one last time before giving up, so a flag
 * that arrived at the very last moment still counts.
 *
 * On a TIMEOUT the driver only releases what it can (STOP, flags). Resetting the
 * peripheral and recovering a stuck bus is step 6.
 *
 * The caller passes the 7-bit address (0x76); the driver shifts it left by one to
 * make room for the R/W bit. An address above 0x7F is a programming error.
 */
#define I2C_WAIT_TIMEOUT_TICKS    ( 10U )

/* Waits until at least one bit of ulMask is set in the register. */
static BaseType_t prvWaitSet( volatile uint32_t * pulRegister, uint32_t ulMask )
{
    const TickType_t xStart = xTaskGetTickCount();

    while( ( *pulRegister & ulMask ) == 0U )
    {
        if( ( xTaskGetTickCount() - xStart ) >= I2C_WAIT_TIMEOUT_TICKS )
        {
            return ( ( *pulRegister & ulMask ) != 0U ) ? pdTRUE : pdFALSE;
        }
    }

    return pdTRUE;
}

/* Waits until every bit of ulMask is clear in the register. */
static BaseType_t prvWaitClear( volatile uint32_t * pulRegister, uint32_t ulMask )
{
    const TickType_t xStart = xTaskGetTickCount();

    while( ( *pulRegister & ulMask ) != 0U )
    {
        if( ( xTaskGetTickCount() - xStart ) >= I2C_WAIT_TIMEOUT_TICKS )
        {
            return ( ( *pulRegister & ulMask ) == 0U ) ? pdTRUE : pdFALSE;
        }
    }

    return pdTRUE;
}

i2c_status_t i2c_probe( uint8_t ucDevice )
{
    i2c_status_t xStatus = I2C_OK;
    uint32_t ulSr1;

    configASSERT( ucDevice <= 0x7FU );

    /* A bus that stays busy cannot be used: nothing was started yet, so there is
     * nothing to clean up. */
    if( prvWaitClear( &I2C1->SR2, I2C_SR2_BUSY ) == pdFALSE )
    {
        return I2C_ERR_TIMEOUT;
    }

    I2C1->CR1 |= I2C_CR1_START;

    if( prvWaitSet( &I2C1->SR1, I2C_SR1_SB ) == pdFALSE )
    {
        xStatus = I2C_ERR_TIMEOUT;
    }
    else
    {
        I2C1->DR = ( uint32_t ) ucDevice << 1;    /* address + R/W bit = 0 (write) */

        if( prvWaitSet( &I2C1->SR1, I2C_SR1_ADDR | I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO ) == pdFALSE )
        {
            xStatus = I2C_ERR_TIMEOUT;
        }
        else
        {
            ulSr1 = I2C1->SR1;

            if( ( ulSr1 & I2C_SR1_AF ) != 0U )
            {
                xStatus = I2C_ERR_NACK;
            }
            else if( ( ulSr1 & ( I2C_SR1_BERR | I2C_SR1_ARLO ) ) != 0U )
            {
                xStatus = I2C_ERR_BUS;
            }
            else
            {
                /* ADDR is set: SR1 was just read, reading SR2 now clears it. */
                ( void ) I2C1->SR2;
            }
        }
    }

    I2C1->SR1 &= ~( I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO );
    I2C1->CR1 |= I2C_CR1_STOP;

    if( ( prvWaitClear( &I2C1->SR2, I2C_SR2_BUSY ) == pdFALSE ) && ( xStatus == I2C_OK ) )
    {
        xStatus = I2C_ERR_TIMEOUT;
    }

    return xStatus;
}

/* ==========================================================================
 * PUBLIC FUNCTION
 * ========================================================================== */
void i2c_init( void )
{
    /* Steps 1) and 2). Step 3) is i2c_probe(); steps 4) onward are added one at
     * a time. */
    prvI2cClocksResetAndPins();
    prvI2cTimingAndEnable();
}
