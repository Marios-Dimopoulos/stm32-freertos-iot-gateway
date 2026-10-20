/*
 * main.c — application entry point.
 *
 * main() only brings the system up and hands over to the RTOS:
 *
 *   1. the CPU clock (84 MHz, clock.c)
 *   2. the USART2 debug output (uart.c)
 *   3. the I2C1 bus (i2c.c)
 *   4. SPI1 as a slave towards the gateway (spi.c): clock, pins and configuration,
 *      still DISABLED
 *   5. the sensor task (sensor_task.c), which owns the BMP280 and the I2C bus, builds
 *      a frame every second and keeps the newest one in the latest-frame queue
 *   6. the transmit task (transmit_task.c), which after every SPI transaction loads
 *      the newest frame for the next one
 *   7. the start of the SPI link (spi_start() in spi.c): the DMA is armed with a
 *      placeholder frame, the NSS interrupt is enabled, and SPI1 is enabled
 *   8. the scheduler (it never returns)
 *
 * The order is not free: the clocks come before the peripherals that derive their
 * timing from them, and the transmit task must exist before spi_start() enables the
 * interrupt that notifies it. See the comments below.
 */

#include "FreeRTOS.h"
#include "task.h"

/* CMSIS device header: register definitions, their bit masks, and the declaration
 * of SystemCoreClockUpdate(). */
#include "stm32f4xx.h"

#include "clock.h"
#include "uart.h"
#include "i2c.h"
#include "spi.h"
#include "sensor_task.h"
#include "transmit_task.h"

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

/* Result of the clock setup, kept where the debugger can see it. volatile so
 * the compiler does not remove a variable that is only written. */
static volatile clock_status_t xClockStatus;

/* Same for the SPI start-up (0 = SPI_OK). */
static volatile spi_status_t xSpiStatus;

/* ==========================================================================
 * MAIN
 * ==========================================================================
 */
int main( void )
{
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
     * the debugger can show it (0 = CLOCK_OK).
     */
    xClockStatus = clock_init_84mhz();

    SystemCoreClockUpdate();

    /* USART2 debug output (see uart.c). It goes after the clock set-up because its
     * baud rate is computed from the final bus clock. It enables the clocks, sets up
     * the TX pin, configures 115200 baud with the transmitter on and sets up the TX
     * interrupt (messages are queued with uart_write() from tasks). */
    uart_init();

    /* I2C1 for the BMP280 (see i2c.c): clocks, peripheral reset, PB8/PB9 and the
     * 100 kHz timing, also computed from the bus clock. The transfers
     * (i2c_read_reg / i2c_write_reg) are made from the sensor task, after the
     * scheduler has started. */
    i2c_init();

    /* SPI1 as a slave towards the gateway (see spi.c): clock, pins PA4 to PA7 and the
     * peripheral's configuration. It is left DISABLED: it is enabled by the code
     * that arms the DMA, because a slave must have its data ready before the master's
     * first clock edge. Nothing happens on those pins until then. */
    spi_init();

    /* The sensor task, with static memory (see sensor_task.c). Nothing runs yet:
     * the task is only registered as READY. */
    sensor_task_create();

    /* The transmit task (see transmit_task.c): it re-arms the DMA after every
     * transaction. It must exist BEFORE spi_start(), because that enables the interrupt
     * that notifies it. */
    transmit_task_create();

    /* Arm the SPI's DMA with the placeholder frame and enable SPI1 (see spi.c). From
     * here on the Pi can clock a frame out. It needs no scheduler. */
    xSpiStatus = spi_start();

    /*
     * Start the scheduler. In order, inside this call:
     *   - the kernel asks vApplicationGetIdleTaskMemory() (freertos_callbacks.c)
     *     for the Idle task's memory and creates it;
     *   - the port checks the interrupt-priority and vector-table setup (the
     *     asserts), sets PendSV and SysTick to the least urgent level,
     *     starts SysTick, and executes `svc 0`;
     *   - the SVC handler starts the highest-priority ready task by "returning"
     *     into the fake exception frame prepared when it was created. That is the
     *     transmit task (priority 2): it runs first and at once blocks, waiting for
     *     its notification, so the sensor task (priority 1) runs next.
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
