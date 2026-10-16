/*
 * i2c.c — I2C1 master driver (BMP280 on PB8 = SCL, PB9 = SDA).
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) clocks, peripheral reset, pins (PB8/PB9 = AF4, open-drain)      <- DONE
 *   2) timing (CR2.FREQ, CCR, TRISE) and enable (CR1.PE)               <- DONE
 *   3) START + address, tell ACK from NACK, waits bounded by the RTOS tick       <- DONE
 *      (first exercised through a probe function, removed once write/read
 *      existed: every real transaction already reports a NACK itself)
 *   4) write to a register                                              <- DONE
 *   5) read from a register (1 byte, then N >= 3 bytes) -> chip ID 0x58   <- DONE
 *   6) error handling and recovery (the tick-based timeouts were done in step 3)   <- DONE
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
 * STEP 3) START + ADDRESS, ACK OR NACK, AND THE TIMEOUTS
 * ==========================================================================
 *
 * On the wire (see the protocol notes): START, then the address byte (7-bit
 * address plus the R/W bit, here 0 = write), then the 9th clock on which the
 * addressed device pulls SDA low (ACK) or nobody does (NACK). This is the start
 * of every transaction (see prvStartAndAddress below). The peripheral generates
 * the signals; the software orders each phase and waits for the flag that says
 * it happened:
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

/* ==========================================================================
 * STEP 6) ERROR RECOVERY AND THE SCHEDULER CHECK
 * ==========================================================================
 *
 * Some errors need only cleanup, and prvStop() already does it: after a NACK the
 * flags are cleared and a STOP releases the bus. Others suggest that the PERIPHERAL
 * itself may be left in a bad state:
 *   - a TIMEOUT (a flag never came: a line held low, or a stuck state machine);
 *   - a BUS error or lost arbitration;
 *   - BUSY that never clears (a known quirk of this peripheral, and the typical
 *     result of a restart in the middle of a transfer).
 * For these the driver RECOVERS: it pulses CR1.SWRST (the software reset of the
 * peripheral) and sets it up again with the same code as at start-up
 * (prvI2cTimingAndEnable). The pins are not touched: they stay connected to the
 * peripheral. It happens in two places:
 *   - prvStop(): when the final status of a transaction is TIMEOUT or BUS;
 *   - at the start of a transaction (prvWaitBusFree): if the bus is still busy,
 *     reset once and look again; only if it is STILL busy is TIMEOUT returned.
 * The call that hit the error still returns its error (the caller learns about it);
 * the NEXT call starts from a clean peripheral, and the caller never has to know how
 * to recover the bus. Every recovery is counted (i2c_recovery_count) so it can be
 * seen in the debugger instead of going unnoticed.
 *
 * What it cannot fix. SWRST resets OUR side only. If a SLAVE is holding SDA low (for
 * example the board was reset in the middle of a byte and the sensor, which stays
 * powered, is still waiting for clock pulses), the lines are not released, the bus
 * shows BUSY again right after the reset, and the calls keep returning TIMEOUT.
 * Freeing it takes a "bus clear": clocking SCL up to nine times by hand with the
 * pins temporarily as GPIO. That is NOT written, because it has not been needed; if
 * it ever is, it belongs in prvRecover().
 *
 * The scheduler check. The public functions assert that the scheduler has started
 * (xTaskGetSchedulerState, enabled by INCLUDE_xTaskGetSchedulerState in
 * FreeRTOSConfig.h). The tick never advances before vTaskStartScheduler(), so a
 * wait bounded by the tick could not time out there; the assertion turns that
 * mistake into an immediate stop at the offending call instead of a possible hang.
 *
 * How to test it on the board: with the sensor reading once per second, touch the SDA
 * wire (PB9) to GND for a couple of seconds and release it (an open-drain line shorted
 * to ground is safe). The test lines must report TIMEOUT or BUS ERROR while it is held,
 * and OK again after the release without resetting the board; ulI2cRecoveries must have
 * gone up.
 */
static volatile uint32_t ulI2cRecoveries;

static void prvRecover( void )
{
    I2C1->CR1 |= I2C_CR1_SWRST;
    I2C1->CR1 &= ~I2C_CR1_SWRST;

    prvI2cTimingAndEnable();

    ulI2cRecoveries++;
}

uint32_t i2c_recovery_count( void )
{
    return ulI2cRecoveries;
}

/* Waits for a free bus at the start of a transaction. A bus that stays busy is reset
 * once and checked again. Nothing was started, so there is no STOP to send. */
static i2c_status_t prvWaitBusFree( void )
{
    if( prvWaitClear( &I2C1->SR2, I2C_SR2_BUSY ) == pdFALSE )
    {
        prvRecover();

        if( prvWaitClear( &I2C1->SR2, I2C_SR2_BUSY ) == pdFALSE )
        {
            return I2C_ERR_TIMEOUT;
        }
    }

    return I2C_OK;
}

/* --------------------------------------------------------------------------
 * Pieces shared by every transaction
 * --------------------------------------------------------------------------
 *
 * A transaction is always: [wait for a free bus] -> START + address -> the
 * data phase -> STOP. The first and the last parts are the same for every
 * function, so they live here once.
 */

/* Waits for the flag ulFlag in SR1, but also watches the error flags, and tells
 * which one came:
 *   I2C_OK       ulFlag is set (and no error)
 *   I2C_ERR_NACK AF: a NACK where an ACK was expected (checked first: an error
 *                can arrive together with TXE or BTF, and must not be hidden)
 *   I2C_ERR_BUS  BERR / ARLO
 *   I2C_ERR_TIMEOUT nothing arrived in time
 * The read of SR1 here is also the first half of the sequences that clear SB
 * and ADDR. */
static i2c_status_t prvWaitFlag( uint32_t ulFlag )
{
    uint32_t ulSr1;

    if( prvWaitSet( &I2C1->SR1, ulFlag | I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO ) == pdFALSE )
    {
        return I2C_ERR_TIMEOUT;
    }

    ulSr1 = I2C1->SR1;

    if( ( ulSr1 & I2C_SR1_AF ) != 0U )
    {
        return I2C_ERR_NACK;
    }

    if( ( ulSr1 & ( I2C_SR1_BERR | I2C_SR1_ARLO ) ) != 0U )
    {
        return I2C_ERR_BUS;
    }

    return I2C_OK;
}

#define I2C_DIRECTION_WRITE    ( 0U )
#define I2C_DIRECTION_READ     ( 1U )

/* START (or repeated START), then the address byte with the direction bit; waits
 * for the device's ACK (the ADDR flag). It does NOT clear ADDR: after a write
 * address that is done at once (prvClearAddr), but after a read address the
 * moment of clearing is the time-critical one and is decided by the caller.
 * Error flags from an earlier transaction are cleared first, so a stale flag can
 * not be mistaken for a new error. */
static i2c_status_t prvStartAndSendAddress( uint8_t ucDevice, uint32_t ulDirection )
{
    i2c_status_t xStatus;

    I2C1->SR1 &= ~( I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO );

    I2C1->CR1 |= I2C_CR1_START;

    xStatus = prvWaitFlag( I2C_SR1_SB );

    if( xStatus == I2C_OK )
    {
        I2C1->DR = ( ( uint32_t ) ucDevice << 1 ) | ulDirection;

        xStatus = prvWaitFlag( I2C_SR1_ADDR );
    }

    return xStatus;
}

/* ADDR is cleared by reading SR1 and then SR2. In master receive mode the clock
 * (SCL) is held low until this is done, and the first byte starts to be clocked
 * in right after it. */
static void prvClearAddr( void )
{
    ( void ) I2C1->SR1;
    ( void ) I2C1->SR2;
}

/* The end of every transaction that was started, whatever happened: clear the
 * error flags (AF, BERR, ARLO are cleared by writing 0), generate STOP (after a
 * NACK the manual requires software to do it; it also releases the bus after an
 * error), and wait until the bus is free again so the next transaction starts
 * cleanly. Returns xStatus, or I2C_ERR_TIMEOUT if everything else went well but
 * the bus did not become free.
 *
 * The receive sequences (step 5) request the STOP themselves, at a precise moment;
 * then xStopAlreadyRequested is pdTRUE and no second request is made: a STOP
 * requested after the STOP condition has already been generated could stay set and
 * disturb the next transaction. */
static i2c_status_t prvStop( i2c_status_t xStatus, BaseType_t xStopAlreadyRequested )
{
    I2C1->SR1 &= ~( I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO );

    if( xStopAlreadyRequested == pdFALSE )
    {
        I2C1->CR1 |= I2C_CR1_STOP;
    }

    if( ( prvWaitClear( &I2C1->SR2, I2C_SR2_BUSY ) == pdFALSE ) && ( xStatus == I2C_OK ) )
    {
        xStatus = I2C_ERR_TIMEOUT;
    }

    /* A timeout or a bus error may have left the peripheral in a bad state: reset it,
     * so that the next transaction starts clean (see step 6). */
    if( ( xStatus == I2C_ERR_TIMEOUT ) || ( xStatus == I2C_ERR_BUS ) )
    {
        prvRecover();
    }

    return xStatus;
}

/* ==========================================================================
 * STEP 4) WRITE TO A REGISTER
 * ==========================================================================
 *
 * On the wire:
 *   START | device + W | ACK | register | ACK | data 1 | ACK | ... | data N | ACK | STOP
 * The device uses the "register" byte to set its internal register pointer, and
 * stores the following data bytes there, incrementing the pointer after each one.
 *
 * What comes after the address phase is the DATA PHASE:
 *
 *   - Before each byte, wait for TXE ("transmit data register empty"), then write
 *     the byte into DR. TXE rises as soon as the previous byte has moved into the
 *     shift register, so the next byte can be queued while the previous one is
 *     still going out on the wire.
 *   - After the LAST byte, wait for BTF ("byte transfer finished") BEFORE the STOP.
 *     TXE only says the byte left DR; BTF says it has been completely clocked out
 *     (both DR and the shift register are empty). Setting STOP right after
 *     writing the last byte could cut it off before its 9th clock.
 *   - A NACK can arrive after ANY byte, not only after the address: the device
 *     refuses a register or a value. So every wait goes through prvWaitFlag(),
 *     which watches AF (and the bus-error flags) as well. At the first error the
 *     loop stops, and prvStop() releases the bus.
 *
 * Nothing in this function is time-critical: during a transmit the peripheral
 * simply stretches SCL (holds it low) if software is late with the next byte, so
 * a preemption just delays the transfer. (Receiving is different, see step 5.)
 *
 * First test: write 0xB6 to register 0xE0 of the BMP280, the soft reset (as a
 * driver does at start-up): the call must return I2C_OK with the sensor connected
 * and I2C_ERR_NACK without it. The sensor needs a short time to restart afterwards;
 * the register can only be read back in step 5.
 */
i2c_status_t i2c_write_reg( uint8_t ucDevice, uint8_t ucRegister,
                            const uint8_t * pucData, uint32_t ulLength )
{
    i2c_status_t xStatus;
    uint32_t ulIndex;

    configASSERT( ucDevice <= 0x7FU );
    configASSERT( ( pucData != NULL ) || ( ulLength == 0U ) );
    configASSERT( xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED );

    if( prvWaitBusFree() != I2C_OK )
    {
        return I2C_ERR_TIMEOUT;
    }

    xStatus = prvStartAndSendAddress( ucDevice, I2C_DIRECTION_WRITE );

    if( xStatus == I2C_OK )
    {
        prvClearAddr();
        xStatus = prvWaitFlag( I2C_SR1_TXE );
    }

    if( xStatus == I2C_OK )
    {
        I2C1->DR = ucRegister;
    }

    for( ulIndex = 0U; ( ulIndex < ulLength ) && ( xStatus == I2C_OK ); ulIndex++ )
    {
        xStatus = prvWaitFlag( I2C_SR1_TXE );

        if( xStatus == I2C_OK )
        {
            I2C1->DR = pucData[ ulIndex ];
        }
    }

    if( xStatus == I2C_OK )
    {
        xStatus = prvWaitFlag( I2C_SR1_BTF );
    }

    return prvStop( xStatus, pdFALSE );
}

/* ==========================================================================
 * STEP 5) READ FROM A REGISTER
 * ==========================================================================
 *
 * On the wire:
 *   START | device + W | ACK | register | ACK | repeated START | device + R | ACK |
 *   byte 1 | ACK | ... | byte N | NACK | STOP
 *
 * Phase 1 is a write that only carries the register address: the device sets its
 * register pointer. We wait for BTF so the register byte is completely sent, and
 * then, instead of a STOP, generate a repeated START (the bus stays ours) and send
 * the address again with the READ bit. From then on the DEVICE drives SDA and the
 * master only provides the clock.
 *
 * WHY RECEIVING IS DELICATE. The master must acknowledge every byte except the last,
 * which it must NACK ("that is enough"), and then generate STOP. The peripheral
 * decides ACK or NACK at the 9th clock of each byte from the state of CR1.ACK at
 * that moment, so software has to clear ACK (and request STOP) BEFORE the 9th clock
 * of the last byte. The window is about one byte time, roughly 90 microseconds at
 * 100 kHz. The sequence depends on how many bytes are read. The
 * one-byte case also matches the teacher's i2c_fixed.c, which wraps the same
 * operations in __disable_irq(). They are to be checked against the manual.
 *
 * N = 1 (the chip ID):
 *   after ADDR is set (the read address was acknowledged), and with the clock still
 *   held low:   clear ACK;  clear ADDR (read SR1, then SR2);  set STOP.
 *   Then wait for RXNE and read the byte. Clearing ADDR releases the clock and the
 *   byte starts to arrive, so ACK must already be clear and STOP requested: these
 *   operations are done in a CRITICAL SECTION so that nothing (another task, an
 *   interrupt) can come between them.
 *
 * N >= 3 (the 6-byte data burst, the 24 trimming bytes):
 *   clear ADDR (ACK stays set; not time-critical). Read bytes 1 ... N-3 as they
 *   arrive (wait RXNE, read DR). When byte N-2 is in DR and byte N-1 is in the shift
 *   register, BTF is set and the clock is held low. Then:
 *       clear ACK;  read DR (byte N-2);  set STOP     <- the CRITICAL SECTION
 *   so that the NEXT byte that finishes, byte N, is NACKed and followed by STOP.
 *   Finally read bytes N-1 and N (wait RXNE, read DR).
 *
 * N = 2 would need a third sequence (with the POS bit). Nothing in this project reads
 * exactly two bytes (the BMP280 module reads 1, 6 or 24), so it is not written and
 * not tested: it stops at an assertion. To read two bytes, read three.
 *
 * Only those few register operations are in the critical section (a few
 * microseconds). All the waits are outside: the RTOS tick does not advance while
 * interrupts are masked, so a wait there could never time out. The rest of the
 * transfer is not at risk from a preemption: if software is late reading a byte
 * (RXNE with BTF), the peripheral holds SCL low until DR is read.
 *
 * Expected results with the BMP280 (values from my memory of its datasheet):
 *   1 byte from 0xD0         0x58   (the chip ID)
 *   6 bytes from 0xF7        80 00 00 80 00 00 right after a reset (no measurement
 *                            yet: pressure and temperature "skipped" values)
 *   24 bytes from 0x88       the trimming parameters, device specific
 */

/* Receives ONE byte. Called with the read address acknowledged and ADDR pending. */
static i2c_status_t prvReceiveOne( uint8_t * pucBuffer, BaseType_t * pxStopRequested )
{
    i2c_status_t xStatus;

    taskENTER_CRITICAL();
    {
        I2C1->CR1 &= ~I2C_CR1_ACK;
        prvClearAddr();
        I2C1->CR1 |= I2C_CR1_STOP;
    }
    taskEXIT_CRITICAL();

    *pxStopRequested = pdTRUE;

    xStatus = prvWaitFlag( I2C_SR1_RXNE );

    if( xStatus == I2C_OK )
    {
        *pucBuffer = ( uint8_t ) I2C1->DR;
    }

    return xStatus;
}

/* Receives ulLength >= 3 bytes. Called with the read address acknowledged and ADDR pending. */
static i2c_status_t prvReceiveMany( uint8_t * pucBuffer, uint32_t ulLength, BaseType_t * pxStopRequested )
{
    i2c_status_t xStatus = I2C_OK;
    uint32_t ulIndex;

    prvClearAddr();

    /* Bytes 1 ... N-3: ACK is still set, the peripheral acknowledges each one. */
    for( ulIndex = 0U; ( ulIndex < ( ulLength - 3U ) ) && ( xStatus == I2C_OK ); ulIndex++ )
    {
        xStatus = prvWaitFlag( I2C_SR1_RXNE );

        if( xStatus == I2C_OK )
        {
            pucBuffer[ ulIndex ] = ( uint8_t ) I2C1->DR;
        }
    }

    /* Byte N-2 is in DR and byte N-1 in the shift register: BTF, clock held low. */
    if( xStatus == I2C_OK )
    {
        xStatus = prvWaitFlag( I2C_SR1_BTF );
    }

    if( xStatus == I2C_OK )
    {
        taskENTER_CRITICAL();
        {
            I2C1->CR1 &= ~I2C_CR1_ACK;
            pucBuffer[ ulLength - 3U ] = ( uint8_t ) I2C1->DR;
            I2C1->CR1 |= I2C_CR1_STOP;
        }
        taskEXIT_CRITICAL();

        *pxStopRequested = pdTRUE;

        /* Bytes N-1 and N. */
        xStatus = prvWaitFlag( I2C_SR1_RXNE );

        if( xStatus == I2C_OK )
        {
            pucBuffer[ ulLength - 2U ] = ( uint8_t ) I2C1->DR;
            xStatus = prvWaitFlag( I2C_SR1_RXNE );
        }

        if( xStatus == I2C_OK )
        {
            pucBuffer[ ulLength - 1U ] = ( uint8_t ) I2C1->DR;
        }
    }

    return xStatus;
}

i2c_status_t i2c_read_reg( uint8_t ucDevice, uint8_t ucRegister,
                           uint8_t * pucBuffer, uint32_t ulLength )
{
    i2c_status_t xStatus;
    BaseType_t xStopRequested = pdFALSE;

    configASSERT( ucDevice <= 0x7FU );
    configASSERT( pucBuffer != NULL );
    configASSERT( ( ulLength == 1U ) || ( ulLength >= 3U ) );
    configASSERT( xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED );

    if( prvWaitBusFree() != I2C_OK )
    {
        return I2C_ERR_TIMEOUT;
    }

    /* Phase 1: write the register address, and wait until it has fully left. */
    xStatus = prvStartAndSendAddress( ucDevice, I2C_DIRECTION_WRITE );

    if( xStatus == I2C_OK )
    {
        prvClearAddr();
        xStatus = prvWaitFlag( I2C_SR1_TXE );
    }

    if( xStatus == I2C_OK )
    {
        I2C1->DR = ucRegister;
        xStatus = prvWaitFlag( I2C_SR1_BTF );
    }

    /* Phase 2: repeated START and the read address. ACK is set so the bytes are
     * acknowledged; the receive sequences clear it for the last byte. */
    if( xStatus == I2C_OK )
    {
        I2C1->CR1 |= I2C_CR1_ACK;
        xStatus = prvStartAndSendAddress( ucDevice, I2C_DIRECTION_READ );
    }

    if( xStatus == I2C_OK )
    {
        if( ulLength == 1U )
        {
            xStatus = prvReceiveOne( pucBuffer, &xStopRequested );
        }
        else
        {
            xStatus = prvReceiveMany( pucBuffer, ulLength, &xStopRequested );
        }
    }

    return prvStop( xStatus, xStopRequested );
}

/* ==========================================================================
 * PUBLIC FUNCTION
 * ========================================================================== */
void i2c_init( void )
{
    /* Steps 1) and 2). Step 3) is the shared start/address/stop code, step 4) is
     * i2c_write_reg(), step 5) is i2c_read_reg(), step 6) is the recovery. */
    prvI2cClocksResetAndPins();
    prvI2cTimingAndEnable();
}
