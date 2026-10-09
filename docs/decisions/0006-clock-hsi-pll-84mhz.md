# ADR-0006: Run the CPU at 84 MHz from the HSI through the PLL

**Status:** Accepted
**Date:** 2026-10-03
**Phase:** 1

## Context

The project targets an 84 MHz CPU clock (CLAUDE.md). After reset the
STM32F411 runs on its internal 16 MHz RC oscillator (the HSI), and the
CMSIS start-up code (`SystemInit()`) does not change that. Nothing in the
RTOS needs 84 MHz to work, as the first blink test showed at 16 MHz. The
reasons for it are the project specification, headroom for the BMP280
maths and later phases, and an SPI clock derived from a known bus frequency.

Two things make the change more than a few register writes:

- The flash needs a fixed time per read (roughly 30-35 ns, inferred from the
  reference manual's wait-state table), independent of the CPU clock. At
  16 MHz one cycle is 62.5 ns, which is enough. At 84 MHz one cycle is 11.9 ns
  and the flash needs 2 extra wait states. If the clock is raised first, the
  CPU reads garbage at the switch and faults.
- The FreeRTOS tick is derived from `SystemCoreClock` when the scheduler
  starts (ADR-0003), so the clock must be set up before that.

A hang is the worst failure mode here: it happens before the scheduler and
before any output exists, with nothing to show why.

## Options considered

### Option A: HSI + PLL
- How it works: keep the internal oscillator as the PLL input
  (16 MHz / M = 8 -> 2 MHz, x N = 168 -> 336 MHz, / P = 4 -> 84 MHz).
- Pros: no dependence on the board and nothing extra to configure; there is
  no external oscillator that might fail to start.
- Cons: the HSI is accurate to only about 1% at room temperature (datasheet
  figure from memory) and drifts with temperature. `HSI_VALUE` in
  `system_stm32f4xx.c` is a nominal constant, not a measurement.

### Option B: HSE from the ST-LINK's 8 MHz output (bypass mode) + PLL
- How it works: use the 8 MHz clock the ST-LINK supplies to the HSE pin
  (M = 4, N = 168, P = 4 gives the same 84 MHz).
- Pros: much better accuracy.
- Cons:
  - depends on the board's revision and solder bridges (to be checked in the
    Nucleo user manual; I could not confirm it from here);
  - needs `HSEBYP` and a bounded wait for `HSERDY`;
  - `system_stm32f4xx.c` defines `HSE_VALUE` as 25 MHz by default, so with an
    8 MHz source `SystemCoreClockUpdate()` would compute 25 MHz / 4 * 168 / 4 =
    262.5 MHz instead of 84 MHz (3.1x too much) and
    the SysTick reload would be wrong, silently, unless `HSE_VALUE=8000000` is
    set project-wide.

### Option C: leave the clock at 16 MHz
- Pros: nothing to write or to get wrong.
- Cons: ignores the specification; no headroom; SPI would have to be derived
  from 16 MHz.

### Where the code lives (independent of A/B/C)
- In `main.c`: fewer files, but mixes hardware bring-up with application code.
- In ST's HAL or generated code: not used in this project (own register-level
  drivers, see ADR-0008).
- In its own module, `Src/clock.c` + `clock.h`: one function,
  `clock_init_84mhz()`, called from `main()` before `SystemCoreClockUpdate()`.

## Decision

Option A in its own module. The deciding factor is that the needs here are
modest: a 115200 baud UART tolerates a few percent, and I2C and SPI as masters
do not depend on the exact master frequency. HSI has no hardware dependency, no
oscillator that can fail to start, and no `HSE_VALUE` trap. HSE stays the
upgrade path if accuracy ever matters (precise timestamps, USB).

The sequence in `clock_init_84mhz()`, in this order:

| Step | Register | What and why this position |
|---|---|---|
| a | `FLASH->ACR` | 2 wait states, prefetch and caches on, read back. Before the clock rises. |
| b | `RCC->CFGR` | AHB /1, APB1 /2, APB2 /1. APB1 may not exceed 50 MHz, so it is divided before the switch. |
| c | `RCC->PLLCFGR` | M = 8, N = 168, P = /4, source HSI, written while the PLL is off and read back. |
| d | `RCC->CR` | `PLLON`, bounded wait for `PLLRDY`. |
| e | `RCC->CFGR` | `SW` = PLL, bounded wait for `SWS` = PLL. |

Design rules of the module:

- **Bounded waits, never a bare `while`.** Each wait is a counter of loop
  passes (a rough 6 ms at 16 MHz, not a real time, because no timer exists yet
  and SysTick belongs to FreeRTOS). On a timeout the function stops and returns
  a status code. The CPU is then still on the HSI, which works.
- **Register fields derived from divider values**, because the P field is
  encoded (00 = /2, 01 = /4, 10 = /6, 11 = /8).
- **The PLL limits are checked at build time** with `_Static_assert`: M 2-63,
  N 50-432, P in {2, 4, 6, 8}, VCO input 1-2 MHz, VCO 100-432 MHz, output
  84 MHz. An illegal number fails the build (demonstrated with N = 300 and 336).
- **Read-modify-write** on every shared register, never a plain assignment
  (for instance `RCC->CR` also holds the HSI switch).

## Consequences

- The CPU runs at 84 MHz: `SystemCoreClock` = 84,000,000 and the SysTick reload
  is 83,999. `main()` keeps the ADR-0003 order: `clock_init_84mhz()`, then
  `SystemCoreClockUpdate()`, then `vTaskStartScheduler()`.
- **APB1 runs at 42 MHz, and `SystemCoreClock` reports HCLK only.** USART2's
  baud divisor and I2C1's timing must use 42 MHz, not `SystemCoreClock`.
- **SPI1 sits on APB2 (84 MHz).** The 6 MHz limit from CLAUDE.md needs a
  prescaler of /16 (5.25 MHz); /8 would give 10.5 MHz. To be rechecked in
  Phase 2.
- The absolute frequency is only as good as the HSI (about 1%). Baud rates and
  any time measurement inherit that error.
- **Verified on the board** by reading registers in the debugger (all matched
  the expected values): `FLASH->ACR` = `0x702`, `RCC->PLLCFGR` = `0x24012A08`,
  `RCC->CR` with `PLLON` and `PLLRDY` set, `RCC->CFGR` = `0x0000100A`
  (`SWS` = PLL), `SystemCoreClock` = 84,000,000, SysTick `LOAD` = 83,999, and
  the blink still about 1 s by eye.
- **Not verified:** the true frequency measured outside the chip (no logic
  analyzer yet; the MCO pin could output the clock for that), and the timeout
  and error paths, which were never triggered (a broken PLL is hard to produce
  on purpose, and the build-time checks make it harder).
- A request withdrawn at the exact moment the hardware completes the switch is
  a race that has not been analysed.
- If the project ever moves to HSE: define `HSE_VALUE=8000000`, enable
  `HSEBYP`, bound the `HSERDY` wait, and re-run the whole verification.

## Interview angle

"The chip starts at 16 MHz. To get 84 MHz I program the PLL, but the order
matters: the flash gets its wait states first, because it has a fixed access
time and would return garbage at the higher clock, and the APB1 prescaler is
set before the switch so it never exceeds its limit. I wait for the PLL's lock
flag and for the status field to confirm the switch, with bounded loops so a
failed lock leaves the board running on the internal oscillator instead of
hanging before the scheduler. I used the HSI because the system doesn't need
crystal accuracy and it avoids a hardware dependency, and I know what that
costs: about 1% on every baud rate and timestamp."
