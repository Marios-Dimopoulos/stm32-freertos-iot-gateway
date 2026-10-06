/*
 * main.c — application entry point.
 *
 * CURRENT STATE: TEMPORARY bring-up code, written only to prove that the
 * RTOS works on the real board before any real driver exists.
 *
 *   One task blinks the Nucleo's user LED (LD2, on PA5) with a period of
 *   exactly 1.000 s (500 ms on, 500 ms off).
 *
 * If it does, then all of this works at once: the linker script and start-up
 * code, the vector table and the handler mapping (SVC / PendSV / SysTick),
 * the SysTick tick rate, vTaskDelayUntil(), static task creation, and the
 * Idle task. Measuring the period on the logic analyzer also proves the
 * tick is really 1 ms (ADR-0003).
 *
 * Remove this blink task once the real sensor task exists. Also note that PA5
 * is SPI1_SCK in the Phase 2 design, so it cannot stay an LED pin.
 */

#include "FreeRTOS.h"
#include "task.h"

/* CMSIS device header: register definitions (RCC, GPIOA, ...), their bit
 * masks, and the declaration of SystemCoreClockUpdate(). */
#include "stm32f4xx.h"

#include "clock.h"
#include "uart.h"
#include "i2c.h"

/* ==========================================================================
 * COMPILE-TIME CONSISTENCY CHECK
 * ==========================================================================
 *
 * configPRIO_BITS in FreeRTOSConfig.h (4) is typed by hand, and the BASEPRI
 * value is derived from it (ADR-0004). The device header knows the real
 * number for this chip. If the two ever disagree (e.g. after switching to a
 * different STM32), the priority threshold would silently be wrong, so make
 * that a build error instead.
 */
_Static_assert( __NVIC_PRIO_BITS == configPRIO_BITS,
                "configPRIO_BITS in FreeRTOSConfig.h does not match the chip" );

/* ==========================================================================
 * BLINK TASK: MEMORY AND PARAMETERS
 * ==========================================================================
 *
 * With static allocation (ADR-0001) we declare the task's memory ourselves
 * and hand it to xTaskCreateStatic():
 *   - the stack: a StackType_t array. The size is in WORDS (4 bytes each),
 *     so 128 means 512 bytes. A starting value to be measured with
 *     uxTaskGetStackHighWaterMark(), not a calculated one. The worst-case
 *     saved context alone is 51 words (see FreeRTOSConfig.h); this task does
 *     almost nothing else.
 *   - the TCB: a StaticTask_t, the kernel's record of the task.
 * Both are file-scope `static`, so they live in .bss for the whole program
 * (they would be destroyed if they were locals of main(): the scheduler
 * resets the main stack when it starts).
 *
 * Priority 1 is the level planned for the sensor task (priority 0 is Idle,
 * priority 2 is reserved for the Phase 2 transmit task).
 */
/* Raised from 128 to 256 words (1 KB) while the temporary I2C tests run: they call
 * deeper than the plain blink (a 24-byte buffer, the driver, the critical-section
 * and tick functions, the UART writes). To be measured (ADR-0005), not guessed. */
#define BLINK_TASK_STACK_WORDS    ( 256 )
#define BLINK_TASK_PRIORITY       ( 1 )

static StackType_t  xBlinkTaskStack[ BLINK_TASK_STACK_WORDS ];
static StaticTask_t xBlinkTaskTCB;

/* Result of the clock setup, kept where the debugger can see it. volatile so
 * the compiler does not remove a variable that is only written. */
static volatile clock_status_t xClockStatus;

/* ==========================================================================
 * LED PIN SETUP (PA5), AT REGISTER LEVEL
 * ==========================================================================
 *
 * Three things are needed before PA5 can drive the LED.
 *
 * 1) Enable GPIOA's clock (RCC->AHB1ENR, bit GPIOAEN).
 *    After reset every peripheral's clock is OFF, to save power. While a
 *    peripheral has no clock, writes to its registers are silently ignored:
 *    no error, no fault. "My pin does nothing" is very often this.
 *
 *    The dummy read of RCC->AHB1ENR afterwards is deliberate. The clock takes
 *    a moment to reach the peripheral, and an access straight after the
 *    enable can be ignored (an STM32F4 erratum; ST's own HAL does the same
 *    read-back). The (void) cast keeps the compiler from complaining about an
 *    unused value; the read happens because the register is volatile.
 *
 * 2) Set PA5 as a general purpose OUTPUT.
 *    GPIOA->MODER has 2 bits per pin: 00 input, 01 output, 10 alternate
 *    function, 11 analog. PA5 uses bits 11:10 and must become 01.
 *
 *    The register is changed with read-modify-write (clear this pin's two
 *    bits, then set them), NOT by assigning a whole new value. After reset
 *    GPIOA->MODER is NOT zero: pins PA13, PA14 and PA15 start in
 *    alternate-function mode because they carry the SWD debug interface
 *    (and JTAG). Writing a whole word would reset them to input and cut the
 *    debugger connection.
 *
 *    The other settings keep their reset values, which are what we want:
 *    OTYPER = push-pull, PUPDR = no pull on PA5, OSPEEDR = low speed (enough
 *    for a 1 Hz LED).
 *
 * 3) Drive the pin. See the task below: GPIOA->BSRR.
 */
static void prvLedInit( void )
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    ( void ) RCC->AHB1ENR;

    GPIOA->MODER = ( GPIOA->MODER & ~GPIO_MODER_MODER5 ) | GPIO_MODER_MODER5_0;
}

/* ==========================================================================
 * TEMPORARY: BMP280 I2C TESTS
 * ==========================================================================
 *
 * Once, when the blink task starts: write the soft-reset value 0xB6 into register
 * 0xE0 of the BMP280, wait for it to restart, then read back:
 *   - 1 byte from 0xD0: the chip ID, expected 0x58;
 *   - 6 bytes from 0xF7: the data registers, expected 80 00 00 80 00 00 (no
 *     measurement yet after a reset);
 *   - 24 bytes from 0x88: the trimming parameters (only the status is checked).
 * And once per second: the chip ID again.
 * Each result is printed as text through the UART: OK, MISMATCH (the transfer
 * worked but the bytes are not the expected ones), or the driver's error. With the
 * sensor unplugged every line must report NACK, and the task must NOT hang. To be
 * removed when the real sensor task exists (the constants then live in bmp280.c).
 */
#define BMP280_I2C_ADDRESS      ( 0x76U )
#define BMP280_REG_RESET        ( 0xE0U )
#define BMP280_RESET_VALUE      ( 0xB6U )
#define BMP280_REG_CHIP_ID      ( 0xD0U )
#define BMP280_CHIP_ID          ( 0x58U )
#define BMP280_REG_DATA         ( 0xF7U )
#define BMP280_REG_TRIMMING     ( 0x88U )

static const char * prvStatusText( i2c_status_t xStatus )
{
    switch( xStatus )
    {
        case I2C_OK:
            return "OK";

        case I2C_ERR_NACK:
            return "NACK";

        case I2C_ERR_TIMEOUT:
            return "TIMEOUT";

        case I2C_ERR_BUS:
            return "BUS ERROR";

        default:
            return "?";
    }
}

static void prvSoftResetTest( void )
{
    static const uint8_t ucResetValue = BMP280_RESET_VALUE;
    const char * pcText = prvStatusText( i2c_write_reg( BMP280_I2C_ADDRESS, BMP280_REG_RESET, &ucResetValue, 1U ) );

    ( void ) uart_write_string( "BMP280 soft reset write: " );
    ( void ) uart_write_string( pcText );
    ( void ) uart_write_string( "\r\n" );
}

static void prvReport( const char * pcWhat, const char * pcResult )
{
    ( void ) uart_write_string( pcWhat );
    ( void ) uart_write_string( ": " );
    ( void ) uart_write_string( pcResult );
    ( void ) uart_write_string( "\r\n" );
}

static void prvChipIdTest( void )
{
    uint8_t ucId = 0U;
    const i2c_status_t xStatus = i2c_read_reg( BMP280_I2C_ADDRESS, BMP280_REG_CHIP_ID, &ucId, 1U );

    prvReport( "BMP280 chip ID",
               ( xStatus != I2C_OK ) ? prvStatusText( xStatus ) : ( ( ucId == BMP280_CHIP_ID ) ? "OK" : "MISMATCH" ) );
}

static void prvDataRegistersTest( void )
{
    static const uint8_t ucExpected[ 6 ] = { 0x80U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U };
    uint8_t ucData[ 6 ] = { 0U };
    uint32_t ulIndex;
    BaseType_t xSame = pdTRUE;
    const i2c_status_t xStatus = i2c_read_reg( BMP280_I2C_ADDRESS, BMP280_REG_DATA, ucData, 6U );

    for( ulIndex = 0U; ulIndex < 6U; ulIndex++ )
    {
        if( ucData[ ulIndex ] != ucExpected[ ulIndex ] )
        {
            xSame = pdFALSE;
        }
    }

    prvReport( "BMP280 data registers after reset (6 bytes)",
               ( xStatus != I2C_OK ) ? prvStatusText( xStatus ) : ( ( xSame == pdTRUE ) ? "OK" : "MISMATCH" ) );
}

static void prvTrimmingTest( void )
{
    uint8_t ucData[ 24 ] = { 0U };
    const i2c_status_t xStatus = i2c_read_reg( BMP280_I2C_ADDRESS, BMP280_REG_TRIMMING, ucData, 24U );

    prvReport( "BMP280 trimming read (24 bytes)", prvStatusText( xStatus ) );
}

/* ==========================================================================
 * BLINK TASK
 * ==========================================================================
 *
 * Why GPIOA->BSRR and not GPIOA->ODR:
 *   BSRR (bit set/reset register) is write-only. Writing a 1 in its low half
 *   (BS5) sets pin 5; writing a 1 in its high half (BR5) clears it; zeros do
 *   nothing. That is ONE write, and it only touches the pin we name, so it is
 *   atomic: no interrupt or other task can get in the middle.
 *   Toggling with ODR (`GPIOA->ODR ^= (1 << 5)`) is a read, an XOR and a
 *   write: if an ISR changes another pin of port A between the read and the
 *   write, that change is overwritten and lost. No error, no crash: a rare,
 *   silent bug. With BSRR the problem cannot happen.
 *
 * Why vTaskDelayUntil() and not vTaskDelay():
 *   vTaskDelay( 500 ) waits 500 ticks from the moment it is called, so any
 *   time spent between delays is added to the period and the schedule drifts.
 *   vTaskDelayUntil( &xLastWakeTime, 500 ) wakes the task at
 *   xLastWakeTime + 500 and moves xLastWakeTime forward, i.e. at fixed
 *   points in time. Here the work is a single register write, so the
 *   difference is tiny, but this is the call the periodic sensor task will
 *   use, so we start with the right habit.
 *
 * Why there is no `return`:
 *   A task function must never return. If it did, it would land in
 *   prvTaskExitError() in port.c, which disables interrupts and loops forever.
 *   Each task is an infinite loop with at least one blocking call, here the
 *   two vTaskDelayUntil() calls, which give the CPU away to other tasks
 *   (Idle, in this case) instead of busy-waiting.
 *
 * Expected result (hypothesis to check on the board):
 *   PA5 high for 500 ms, low for 500 ms: a 1.000 s period. It is also
 *   Arduino header pin D13, easy to probe with the logic analyzer.
 */
static void prvBlinkTask( void * pvParameters )
{
    TickType_t xLastWakeTime;

    ( void ) pvParameters;

    /* The first reference point for vTaskDelayUntil(): "now". */
    /* TEMPORARY: the soft-reset write test, once. The sensor needs a few
     * milliseconds to restart afterwards (about 2 ms, from the datasheet as I
     * remember it), so wait before any further access. */
    prvSoftResetTest();
    vTaskDelay( pdMS_TO_TICKS( 10 ) );
    prvChipIdTest();
    prvDataRegistersTest();
    prvTrimmingTest();

    xLastWakeTime = xTaskGetTickCount();

    for( ;; )
    {
        GPIOA->BSRR = GPIO_BSRR_BS5;                                  /* LED on  */
        ( void ) uart_write_string( "LED on\r\n" );     /* TEMPORARY test of the UART */
        prvChipIdTest();                                  /* TEMPORARY test of the I2C read */
        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS( 500 ) );

        GPIOA->BSRR = GPIO_BSRR_BR5;                                  /* LED off */
        ( void ) uart_write_string( "LED off\r\n" );    /* TEMPORARY test of the UART */

        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS( 500 ) );
    }
}

/* ==========================================================================
 * MAIN
 * ==========================================================================
 */
int main( void )
{
    TaskHandle_t xBlinkTaskHandle;

    /*
     * Before main(), Reset_Handler has already run SystemInit() and the C
     * library start-up (it copied .data from flash and zeroed .bss).
     *
     * ADR-0003 rule: clock setup -> SystemCoreClockUpdate() ->
     * vTaskStartScheduler(), in that order. SystemCoreClockUpdate() READS the
     * RCC registers and stores the real CPU frequency in SystemCoreClock; the
     * port then derives the SysTick reload value from it.
     *
     * clock_init_84mhz() moves the CPU from the 16 MHz HSI to 84 MHz through the
     * PLL (see clock.c). If any step fails it stops and returns an error code,
     * and the CPU stays on the HSI, so the system still works, only slower.
     * Either way SystemCoreClockUpdate() below reads what the hardware actually
     * ended up with: 84,000,000 on success (SysTick LOAD = 83,999), 16,000,000
     * on failure (LOAD = 15,999). The result is kept in a volatile variable so
     * the debugger can show it (0 = CLOCK_OK); once USART2 exists it will be
     * printed.
     */
    xClockStatus = clock_init_84mhz();

    SystemCoreClockUpdate();

    prvLedInit();

    /* USART2 debug output, built step by step (see uart.c). It goes after the
     * clock set-up because its baud rate, added in a later step, depends on the
     * final clock. So far it enables the clocks, sets up the TX pin and
     * configures 115200 baud with the transmitter on and sets up the TX interrupt
     * (messages are queued with uart_write() from tasks). */
    uart_init();

    /* I2C1 for the BMP280, built step by step (see i2c.c). So far it only
     * enables the clocks, resets the peripheral, sets up PB8/PB9, programs the
     * 100 kHz timing and enables I2C1; i2c_write_reg() is used by the temporary test
     * in the blink task. */
    i2c_init();

    /*
     * Create the blink task. Arguments, in order: the task function, a name
     * (debugging only), the stack size in words, a parameter for the task (R0
     * on its first run, unused here), the priority, the stack array and the
     * TCB. It returns the task's handle. With static allocation it can only
     * fail if one of the two buffers is NULL, which would be a programming
     * error, so it is an assert and not run-time error handling.
     *
     * Nothing runs yet: the task is only registered as READY.
     */
    xBlinkTaskHandle = xTaskCreateStatic( prvBlinkTask,
                                          "Blink",
                                          BLINK_TASK_STACK_WORDS,
                                          NULL,
                                          BLINK_TASK_PRIORITY,
                                          xBlinkTaskStack,
                                          &xBlinkTaskTCB );
    configASSERT( xBlinkTaskHandle != NULL );

    /*
     * Start the scheduler. In order, inside this call:
     *   - the kernel asks vApplicationGetIdleTaskMemory() (freertos_callbacks.c)
     *     for the Idle task's memory and creates it;
     *   - the port checks the interrupt-priority and vector-table setup (the
     *     asserts), sets PendSV and SysTick to the least urgent level,
     *     starts SysTick, and executes `svc 0`;
     *   - the SVC handler starts the highest-priority ready task (the blink
     *     task) by "returning" into the fake exception frame prepared when it
     *     was created.
     * This call does not return.
     */
    vTaskStartScheduler();

    /*
     * Only reached if the scheduler could not start (e.g. the Idle task could
     * not be created). Stop here, where the debugger can see it.
     */
    for( ;; )
    {
    }
}
