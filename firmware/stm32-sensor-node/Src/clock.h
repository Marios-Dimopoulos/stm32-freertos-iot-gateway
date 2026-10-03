/*
 * clock.h — CPU clock setup (HSI -> PLL -> 84 MHz).
 *
 * Sets the CPU clock to 84 MHz from the internal 16 MHz HSI through the PLL.
 * See clock.c for the steps and the reasoning behind each of them.
 */

#ifndef CLOCK_H
#define CLOCK_H

typedef enum
{
    CLOCK_OK = 0,
    CLOCK_ERR_FLASH_LATENCY,    /* the flash wait-state setting did not take effect */
    CLOCK_ERR_PLL_CONFIG,       /* the PLL configuration register did not keep what was written */
    CLOCK_ERR_PLL_LOCK,         /* the PLL did not report "locked" in time (it was switched off again) */
    CLOCK_ERR_SWITCH            /* the CPU did not move to the PLL in time (the request was withdrawn) */
} clock_status_t;

/*
 * Configures the system clock for 84 MHz. Must be called from main() BEFORE
 * SystemCoreClockUpdate() and vTaskStartScheduler() (ADR-0003): the kernel
 * derives the tick from the clock frequency that is current when the
 * scheduler starts.
 *
 * It never hangs: on any failure it stops and returns an error code, leaving
 * the chip on the 16 MHz HSI, which is a working configuration.
 */
clock_status_t clock_init_84mhz( void );

#endif /* CLOCK_H */
