/*
 * clock.c — CPU clock setup: HSI (16 MHz) -> PLL -> 84 MHz.
 *
 * Steps, in the order they must run (RM0383, chapters "Reset and
 * clock control" and "Embedded Flash memory interface"):
 *
 *   a) FLASH->ACR   flash wait states = 2, prefetch + caches on, read back  <- DONE
 *   b) RCC->CFGR    bus prescalers: AHB /1, APB1 /2, APB2 /1               <- DONE
 *   c) RCC->PLLCFGR M = 8, N = 168, P = /4, source HSI (PLL still OFF)      <- DONE
 *   d) RCC->CR      PLLON, wait for PLLRDY (bounded wait)                   <- DONE
 *   e) RCC->CFGR    SW = PLL, wait for SWS = PLL (bounded wait)             <- DONE
 *   f) main()       SystemCoreClockUpdate() reads the result
 *
 * Steps a) and b) deliberately come first: they are harmless at 16 MHz, and
 * doing them first means that the one dangerous moment, the switch in e),
 * happens with everything already prepared.
 */

#include "clock.h"
#include "stm32f4xx.h"

/* ==========================================================================
 * STEP a) FLASH WAIT STATES
 * ==========================================================================
 *
 * The program is stored in flash, which needs a fixed amount of TIME per
 * read (roughly 30-35 ns, inferred from the reference manual's table of about
 * 30 MHz per wait state), no matter how fast the CPU clock is. At 16 MHz one
 * CPU cycle is 62.5 ns, longer than that, so the flash keeps up. At 84 MHz
 * one cycle is 11.9 ns, and the flash needs three of them: the first cycle
 * plus 2 "wait states" (extra cycles spent waiting). If the CPU were switched
 * to 84 MHz with 0 wait states, its next instruction fetch would be done too
 * early and would read garbage, so it would fault right at the switch.
 *
 * Wait states are therefore set BEFORE the clock is raised. Setting 2 while
 * still at 16 MHz is harmless: it only wastes a cycle per uncached read.
 *
 * (Supply 2.7-3.6 V, the Nucleo's 3.3 V. Wait states needed, from the manual:
 *  0-30 MHz: 0 | 30-60 MHz: 1 | 60-90 MHz: 2 | 90-100 MHz: 3.)
 *
 * FLASH->ACR fields used (bit positions from stm32f411xe.h):
 *   LATENCY [2:0]  number of wait states
 *   PRFTEN  bit 8  prefetch: read the next instructions ahead of the CPU
 *   ICEN    bit 9  instruction cache
 *   DCEN    bit 10 data cache (constants stored in flash)
 * Prefetch and the caches (ST calls them the ART accelerator) hide most of
 * the wait states: sequential code and loops mostly come from the prefetch
 * buffer and the cache, with no waiting at all. They are an optimisation; the
 * wait states are what makes the clock change CORRECT.
 *
 * The LATENCY field is changed with a read-modify-write (clear the field, then
 * set it) so the other bits of the register are not disturbed. It is written
 * first and read back before anything else is enabled, because the hardware
 * may need a moment to apply it, and the clock must not be raised until it has.
 * The read-back loop is bounded: a loop counter, not a real time (no timer
 * exists yet). It is generous compared with the one or two reads that
 * should be needed, and if it runs out we report an error and stay at 16 MHz
 * instead of risking the switch.
 */
#define CLOCK_FLASH_WAIT_STATES       ( FLASH_ACR_LATENCY_2WS )
#define CLOCK_FLASH_READBACK_TRIES    ( 1000U )

static clock_status_t prvFlashInit( void )
{
    uint32_t ulTries;

    FLASH->ACR = ( FLASH->ACR & ~FLASH_ACR_LATENCY ) | CLOCK_FLASH_WAIT_STATES;

    for( ulTries = CLOCK_FLASH_READBACK_TRIES; ulTries > 0U; ulTries-- )
    {
        if( ( FLASH->ACR & FLASH_ACR_LATENCY ) == CLOCK_FLASH_WAIT_STATES )
        {
            break;
        }
    }

    if( ulTries == 0U )
    {
        return CLOCK_ERR_FLASH_LATENCY;
    }

    FLASH->ACR |= FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    return CLOCK_OK;
}

/* ==========================================================================
 * STEP b) BUS PRESCALERS
 * ==========================================================================
 *
 * After the system clock (SYSCLK), three prescalers in RCC->CFGR decide the
 * clock of each part of the chip:
 *
 *   HPRE   bits 7:4    AHB prescaler  -> HCLK: the CPU, SysTick, DMA, GPIO
 *   PPRE1  bits 12:10  APB1 prescaler -> the slow peripheral bus: USART2, I2C1
 *   PPRE2  bits 15:13  APB2 prescaler -> the fast peripheral bus: SPI1
 *
 * Each bus has its own maximum frequency (from the reference manual)
 * AHB 100 MHz, APB1 50 MHz, APB2 100 MHz.
 *
 * With SYSCLK = 84 MHz:
 *
 *   AHB  /1 -> HCLK = 84 MHz   (the CPU runs at full speed)
 *   APB1 /2 -> 42 MHz          (84 would exceed APB1's 50 MHz limit)
 *   APB2 /1 -> 84 MHz          (within its 100 MHz limit)
 *
 * Why this step comes BEFORE the clock is raised: the prescalers apply to the
 * bus clock from the moment they are written. Setting APB1 to /2 first means
 * APB1 is 8 MHz now (16 / 2) and 42 MHz after the switch, and so never above
 * its limit, not even for a moment. Nothing uses APB1 yet (USART2 and I2C1
 * come later), so this changes nothing observable today. ST's HAL takes a
 * more conservative route (it sets the highest dividers first and the final
 * ones after the switch); that is only needed when the final dividers could
 * be out of spec in the old state, which is not our case.
 *
 * What the bus clocks mean for later code:
 *   - SystemCoreClock (CMSIS) reports HCLK ONLY. It does not include the APB
 *     prescalers, so USART2's baud-rate divisor and I2C1's timing must use
 *     42 MHz, not SystemCoreClock.
 *   - SPI1 is on APB2 (84 MHz). The 6 MHz limit from CLAUDE.md then needs an
 *     SPI prescaler of /16 (84 / 16 = 5.25 MHz); /8 would give 10.5 MHz.
 *
 * All three fields are written explicitly (not only APB1) so the result does
 * not depend on the reset state. HPRE and PPRE2 /1 are the reset values, so
 * for those two this is a statement of intent. The register is changed with a
 * read-modify-write that touches only these three fields: the SW and SWS
 * fields (the clock source selection) in the same register stay as they are.
 *
 * Expected RCC->CFGR afterwards (still on the HSI, so SW = SWS = 00):
 * 0x00001000. RCC->CFGR is at 0x40023808 (RCC base 0x40023800 + 0x08).
 */
static void prvBusPrescalersInit( void )
{
    RCC->CFGR = ( RCC->CFGR & ~( RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 ) ) |
                ( RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1 );
}

/* ==========================================================================
 * STEP c) PLL NUMBERS
 * ==========================================================================
 *
 * The PLL (phase-locked loop) is a circuit with its own fast oscillator (the
 * VCO) that keeps adjusting itself until it runs at an exact multiple of a
 * slow reference. Three numbers set that multiple:
 *
 *   source -> /M -> (VCO input) -> xN -> (VCO output) -> /P -> SYSCLK
 *   16 MHz    8       2 MHz      168      336 MHz        4     84 MHz
 *
 * The chosen values:
 *
 *   M = 8    VCO input  = 16 MHz / 8 = 2 MHz       limit 1..2 MHz (2 preferred:
 *                                                   the loop compares phases at
 *                                                   this rate; higher = less jitter)
 *   N = 168  VCO output = 2 MHz x 168 = 336 MHz    limit 100..432 MHz (the analog
 *                                                   oscillator only works reliably
 *                                                   inside its tuning range)
 *   P = 4    SYSCLK     = 336 MHz / 4 = 84 MHz     limit 100 MHz (chip rating)
 *
 * Other combinations also give 84 MHz (e.g. M = 16, N = 336 has a 1 MHz VCO
 * input, the bottom of the range; M = 8, N = 84, P = 2 uses a 168 MHz VCO).
 * This one keeps the VCO input at the preferred 2 MHz, and a 336 MHz VCO is
 * also the value from which the Q divider (not used here) gives exactly
 * 48 MHz (336 / 7) should USB ever be needed.
 *
 * The numbers are written as plain divider values below, and the register
 * fields are DERIVED from them, because the P field is ENCODED, not raw:
 *   PLLP bits 17:16 = 00 -> /2, 01 -> /4, 10 -> /6, 11 -> /8
 * i.e. field = P/2 - 1. Writing the raw number 4 into that field would
 * select bits 00 (/2, 168 MHz: not what we want). M and N are raw:
 * PLLM bits 5:0, PLLN bits 14:6.
 *
 * The limits above are checked with _Static_assert: if someone changes a
 * number to an illegal combination, the BUILD fails instead of the PLL
 * silently failing to lock (or running outside its rating) on the board.
 *
 * Other PLLCFGR fields:
 *   PLLSRC bit 22: 0 = HSI, 1 = HSE. We use the HSI (no dependence on the
 *                  board, no HSE_VALUE to keep consistent).
 *   PLLQ  bits 27:24: divider for USB/SDIO, unused here. It is left exactly
 *                  as it is (reset value, a legal one), which is why the
 *                  register is changed with a read-modify-write.
 *
 * The PLL must be OFF when this register is written; writes while it runs
 * are ignored (as I understand the manual). After reset it is off, and step
 * d) turns it on only afterwards. The write is read back and compared, so an
 * ignored write is reported as an error instead of going unnoticed.
 *
 * Expected RCC->PLLCFGR afterwards (RCC base 0x40023800 + 0x04 = 0x40023804):
 * the low 20 bits must read 0x12A08, which decodes as
 *   M = 0x12A08 & 0x3F            = 8
 *   N = (0x12A08 >> 6) & 0x1FF    = 0x0A8 = 168
 *   P = (0x12A08 >> 16) & 3       = 01    -> /4
 *   PLLSRC (bit 22)               = 0     -> HSI
 * With the reset value I remember (0x24003010) the whole register would then
 * read 0x24012A08; the upper digits (PLLQ and a reserved bit) are not ours.
 */
#define CLOCK_PLL_M            ( 8U )
#define CLOCK_PLL_N            ( 168U )
#define CLOCK_PLL_P            ( 4U )      /* divider VALUE; the field is P/2 - 1 */

/* The HSI frequency the PLL input is derived from. system_stm32f4xx.c has its
 * own HSI_VALUE with the same number, but that macro is private to that file
 * (it is not in any header), so the figure is repeated here. It is nominal:
 * the real HSI is only accurate to about 1%. */
#define CLOCK_HSI_HZ           ( 16000000UL )

#define CLOCK_PLL_VCO_IN_HZ    ( CLOCK_HSI_HZ / CLOCK_PLL_M )
#define CLOCK_PLL_VCO_HZ       ( CLOCK_PLL_VCO_IN_HZ * CLOCK_PLL_N )
#define CLOCK_SYSCLK_HZ        ( CLOCK_PLL_VCO_HZ / CLOCK_PLL_P )

_Static_assert( ( CLOCK_PLL_M >= 2U ) && ( CLOCK_PLL_M <= 63U ), "PLL M out of range" );
_Static_assert( ( CLOCK_PLL_N >= 50U ) && ( CLOCK_PLL_N <= 432U ), "PLL N out of range" );
_Static_assert( ( CLOCK_PLL_P == 2U ) || ( CLOCK_PLL_P == 4U ) ||
                ( CLOCK_PLL_P == 6U ) || ( CLOCK_PLL_P == 8U ), "PLL P must be 2, 4, 6 or 8" );
_Static_assert( ( CLOCK_PLL_VCO_IN_HZ >= 1000000UL ) && ( CLOCK_PLL_VCO_IN_HZ <= 2000000UL ),
                "PLL VCO input must be 1..2 MHz" );
_Static_assert( ( CLOCK_PLL_VCO_HZ >= 100000000UL ) && ( CLOCK_PLL_VCO_HZ <= 432000000UL ),
                "PLL VCO output must be 100..432 MHz" );
_Static_assert( CLOCK_SYSCLK_HZ == 84000000UL, "this module is meant to produce 84 MHz" );

static clock_status_t prvPllConfigure( void )
{
    const uint32_t ulFieldsMask = RCC_PLLCFGR_PLLM | RCC_PLLCFGR_PLLN |
                                  RCC_PLLCFGR_PLLP | RCC_PLLCFGR_PLLSRC;
    const uint32_t ulFields = ( CLOCK_PLL_M << RCC_PLLCFGR_PLLM_Pos ) |
                              ( CLOCK_PLL_N << RCC_PLLCFGR_PLLN_Pos ) |
                              ( ( ( CLOCK_PLL_P / 2U ) - 1U ) << RCC_PLLCFGR_PLLP_Pos ) |
                              RCC_PLLCFGR_PLLSRC_HSI;

    RCC->PLLCFGR = ( RCC->PLLCFGR & ~ulFieldsMask ) | ulFields;

    if( ( RCC->PLLCFGR & ulFieldsMask ) != ulFields )
    {
        return CLOCK_ERR_PLL_CONFIG;
    }

    return CLOCK_OK;
}

/* ==========================================================================
 * STEP d) START THE PLL AND WAIT FOR IT TO LOCK
 * ==========================================================================
 *
 * RCC->CR has two bits for the PLL:
 *   PLLON  bit 24  (read/write)  1 = switch the PLL on
 *   PLLRDY bit 25  (read-only)   set by the HARDWARE when the PLL is locked,
 *                                i.e. its output has settled at the target
 *                                frequency and can be used
 *
 * The PLL does not produce a usable clock the moment it is switched on: its
 * oscillator has to be steered to the target first. Switching the system clock
 * to it before PLLRDY is set would run the CPU from an unsettled clock. That is
 * why this step waits, and why the switch (step e) is a separate step after it.
 * The CPU keeps running from the HSI the whole time, so nothing changes for
 * the running program yet.
 *
 * Why `|=` and not `=` on RCC->CR: this register also holds the HSI switch
 * (HSION), the HSI trim value and the other oscillators' bits. A plain
 * assignment of just PLLON would write 0 to HSION, i.e. try to switch off the
 * very oscillator we are running on. (As I understand the manual the hardware
 * refuses that while the HSI is the system clock, but we do not rely on it.)
 * The read-modify-write sets one bit and keeps the rest. The read-only
 * *RDY bits come back as ones in the read; writing them back is ignored.
 *
 * The wait is BOUNDED, like the flash read-back: a counter of loop passes, not
 * a real time (no timer exists yet; SysTick belongs to FreeRTOS and its
 * interrupt must not be touched before the scheduler starts). The PLL's lock
 * time is on the order of 100 microseconds. One pass is about 10 CPU cycles at 16 MHz (about
 * 0.6 microseconds, a rough estimate, not measured), so 10000 passes are
 * roughly 6 ms: dozens of times the expected lock time. If it is still not
 * locked by then something is wrong, and waiting longer will not help; an
 * unbounded `while( !PLLRDY )` would instead freeze the board before the
 * scheduler, with nothing to show why.
 *
 * On a timeout the PLL is switched off again, so the chip is left in a clean
 * state (HSI, PLL off, still fully working), and an error is returned. This
 * failure path is NOT tested yet: it needs a deliberately broken PLL, which
 * the build-time checks above make hard to produce.
 *
 * Expected RCC->CR afterwards (0x40023800): bits 25 and 24 set, plus the HSI
 * bits that were already there. Reset value is 0x00000083 (HSION, HSIRDY and
 * the default trim) plus the factory calibration in bits 15:8, which differs
 * from chip to chip, so expect 0x03xxxx83 with xxxx chip-specific.
 */
#define CLOCK_PLL_LOCK_TRIES    ( 10000U )

static clock_status_t prvPllStart( void )
{
    uint32_t ulTries;

    RCC->CR |= RCC_CR_PLLON;

    for( ulTries = CLOCK_PLL_LOCK_TRIES; ulTries > 0U; ulTries-- )
    {
        if( ( RCC->CR & RCC_CR_PLLRDY ) != 0U )
        {
            break;
        }
    }

    if( ulTries == 0U )
    {
        RCC->CR &= ~RCC_CR_PLLON;
        return CLOCK_ERR_PLL_LOCK;
    }

    return CLOCK_OK;
}

/* ==========================================================================
 * STEP e) SWITCH THE CPU TO THE PLL
 * ==========================================================================
 *
 * This is the step that changes the speed. Everything it depends on is
 * already in place: the flash wait states (a), the APB1 prescaler (b), a
 * configured (c) and locked (d) PLL.
 *
 * RCC->CFGR has two fields for the clock source:
 *   SW   bits 1:0  (read/write)  REQUEST: which source should feed SYSCLK
 *                                00 = HSI, 01 = HSE, 10 = PLL
 *   SWS  bits 3:2  (read-only)   STATUS: which source is feeding it NOW,
 *                                same encoding, set by the hardware
 *
 * Writing SW is only a request. The hardware performs the switch itself (it
 * does so without glitches, when the new clock is ready) and then reports it
 * in SWS. Until SWS reads "PLL", the CPU is still on the HSI. So after
 * writing SW we wait for SWS, with a bounded loop like the others (10000
 * passes, about 6 ms at 16 MHz by the rough estimate used above; the switch
 * itself should take only a few clock cycles).
 *
 * After SWS confirms, instructions are being fetched at 84 MHz. That is the
 * moment the flash wait states (2) matter: had they not been set in step a,
 * this is where the CPU would read garbage and fault.
 *
 * The register is changed with a read-modify-write that touches only SW, so
 * the prescaler fields set in step b stay as they are.
 *
 * If SWS does not change within the limit, the request is withdrawn (SW back
 * to HSI) and an error is returned, so the system continues on the HSI. The
 * PLL is deliberately NOT switched off in this case: if the switch happened
 * just after the loop gave up, the PLL would already be the system clock, and
 * switching it off would stop the CPU. An idle PLL is harmless. The caller
 * (main) calls SystemCoreClockUpdate() afterwards, which reads SWS and so
 * reports whatever the hardware actually ended up with.
 *
 * Expected RCC->CFGR afterwards: 0x0000100A
 *   0x1000  APB1 /2 (step b)
 *   0x0008  SWS = 10, the PLL is in use
 *   0x0002  SW  = 10, the PLL was requested
 * and, once main() has called SystemCoreClockUpdate():
 *   SystemCoreClock = (16 MHz / 8) * 168 / 4 = 84000000 = 0x0501BD00.
 */
#define CLOCK_SWITCH_TRIES    ( 10000U )

static clock_status_t prvSwitchToPll( void )
{
    uint32_t ulTries;

    RCC->CFGR = ( RCC->CFGR & ~RCC_CFGR_SW ) | RCC_CFGR_SW_PLL;

    for( ulTries = CLOCK_SWITCH_TRIES; ulTries > 0U; ulTries-- )
    {
        if( ( RCC->CFGR & RCC_CFGR_SWS ) == RCC_CFGR_SWS_PLL )
        {
            break;
        }
    }

    if( ulTries == 0U )
    {
        RCC->CFGR = ( RCC->CFGR & ~RCC_CFGR_SW ) | RCC_CFGR_SW_HSI;
        return CLOCK_ERR_SWITCH;
    }

    return CLOCK_OK;
}

/* ==========================================================================
 * PUBLIC FUNCTION
 * ========================================================================== */
clock_status_t clock_init_84mhz( void )
{
    clock_status_t xStatus;

    /* The order matters, see the step comments above. Every step that can
     * fail reports it, and the first failure stops the sequence: the CPU is
     * then still on the 16 MHz HSI, which is a working configuration.
     *
     * Expected state on success:
     *   FLASH->ACR  = 0x00000702  (2 wait states, prefetch, caches)
     *   RCC->CFGR   = 0x0000100A  (APB1 /2, SW = SWS = PLL)
     *   RCC->PLLCFGR low 20 bits = 0x12A08  (M = 8, N = 168, P = /4, HSI)
     *   RCC->CR     = 0x03xxxx83  (PLLON and PLLRDY set; xxxx = HSI calibration)
     * and SystemCoreClock = 84000000 after the caller's SystemCoreClockUpdate(). */
    xStatus = prvFlashInit();
    if( xStatus != CLOCK_OK )
    {
        return xStatus;
    }

    prvBusPrescalersInit();

    xStatus = prvPllConfigure();
    if( xStatus != CLOCK_OK )
    {
        return xStatus;
    }

    xStatus = prvPllStart();
    if( xStatus != CLOCK_OK )
    {
        return xStatus;
    }

    return prvSwitchToPll();
}
