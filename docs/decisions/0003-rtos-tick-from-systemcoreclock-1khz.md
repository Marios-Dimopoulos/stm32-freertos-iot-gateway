# ADR-0003: Derive the RTOS tick from `SystemCoreClock`, at 1 kHz

**Status:** Accepted
**Date:** 2026-09-29
**Phase:** 1

## Context

FreeRTOS on Cortex-M generates its tick with SysTick, a 24-bit down-counter
inside the core that counts CPU clock cycles. The port programs it once, when
the scheduler starts:

```
LOAD = configCPU_CLOCK_HZ / configTICK_RATE_HZ - 1        (port.c:827)
```

One tick lasts `LOAD + 1` *real* CPU cycles. So two separate things must be
decided:

1. **`configCPU_CLOCK_HZ`**: how the kernel learns the CPU frequency. This is
   not a free choice, because it must equal the real clock set by the RCC.
   If it doesn't, every delay and timeout is silently wrong by the ratio of
   the two.
2. **`configTICK_RATE_HZ`**: the kernel's time resolution. This one is a free
   choice.

The forces at play:
- The target clock is 84 MHz from the PLL, but **no clock-tree code exists
  yet**. After reset, `SystemInit()` leaves the chip on the 16 MHz HSI
  oscillator.
- We want to build and test the RTOS *before* writing the PLL code, and not
  have to revisit the RTOS config when the PLL code is added.
- The node is USB-powered, so power is not a concern.
- The logic analyzer and the Keil debugger make the result directly
  measurable.

## Options considered

### `configCPU_CLOCK_HZ`

#### Option A: Hard-coded constant `84000000UL`
- How it works: The division is done at compile time, and `LOAD` is always
  83,999.
- Pros: The value is visible in the header, and there's no run-time ordering
  to get right.
- Cons: It states the clock we *intend*, not the one we *have*. Today (16 MHz)
  every tick would be 84,000 × 62.5 ns = 5.25 ms, so all delays would be
  5.25× too long with no error or warning. Any future clock change must also
  remember to edit this header.

#### Option B: CMSIS `SystemCoreClock` variable
- How it works: `SystemCoreClockUpdate()` reads the RCC registers (clock
  source, PLL M/N/P, AHB prescaler) and stores the resulting HCLK frequency
  in `SystemCoreClock`. The port reads it once, in `vTaskStartScheduler()`.
- Pros: The tick follows the real clock. It's correct at 16 MHz today
  (LOAD = 15,999) and at 84 MHz after the PLL step (LOAD = 83,999), with no
  config change. This is what ST's own FreeRTOS templates use.
- Cons: It adds an ordering rule to `main()`. If `SystemCoreClockUpdate()` is
  skipped after changing the clock, the variable is stale and the mismatch
  comes back silently. The header needs an `extern` declaration.

### `configTICK_RATE_HZ`

#### Option C: 1000 Hz (1 ms tick)
- Pros: `pdMS_TO_TICKS()` is exact for whole milliseconds. The worst-case
  delay error is 1 ms. The integer division is exact at both 16 and 84 MHz.
- Cons: 1000 SysTick interrupts per second. The expected overhead is well
  under 1% at 84 MHz, but this should be measured.

#### Option D: 100 Hz (10 ms tick)
- Pros: 10× fewer interrupts, which matters for battery devices.
- Cons: The worst-case delay error is 10 ms. Short delays round down to 0,
  e.g. `pdMS_TO_TICKS(5) == 0`, so `vTaskDelay()` doesn't sleep at all.

## Decision

**B + C: `configCPU_CLOCK_HZ = SystemCoreClock`, `configTICK_RATE_HZ = 1000`.**

What decided it for `configCPU_CLOCK_HZ`: the clock setup *will* change after
this config is written. Option B makes the tick follow the hardware instead
of a number written in a header, and it turns a silent timing bug into one
ordering rule that's easy to state and check. For the tick rate, 1 kHz costs
little on a USB-powered 84 MHz part and removes a class of silent
rounding-to-zero bugs.

## Consequences

- **`main()` must follow this order:**
  clock setup → `SystemCoreClockUpdate()` → `vTaskStartScheduler()`.
  `SystemCoreClockUpdate()` only *reads* the clock configuration; it
  configures nothing.
- The RTOS can be built and tested now, on 16 MHz HSI. Adding the PLL later
  needs no change to `FreeRTOSConfig.h`.
- `FreeRTOSConfig.h` declares `extern uint32_t SystemCoreClock;`. Application
  code must include `FreeRTOS.h`, which provides `<stdint.h>` first, never
  `FreeRTOSConfig.h` directly.
- **Failure mode to recognise:** PLL at 84 MHz but `SystemCoreClockUpdate()`
  forgotten. Then `LOAD` = 15,999 is counted at 84 MHz, so a tick lasts
  16,000 × 11.9 ns ≈ 0.19 ms and every delay runs **5.25× too fast**. A GPIO
  toggled every `pdMS_TO_TICKS(500)` would show a ~190 ms period instead of
  1.000 s.
- **Verification to do** once the first task exists:
  - Keil *Peripherals → Core Peripherals → System Tick Timer* shows
    `LOAD = 15999` (HSI), and `83999` after the PLL step.
  - The PulseView period of the 500 ms toggle is 1.000 s.
  - Measure the actual tick overhead.
- Revisit the tick rate only if a low-power mode (tickless idle) is ever
  needed.

## Interview angle

"`configCPU_CLOCK_HZ` isn't a setting, it's a fact the kernel has to be told
correctly. So I derive it from the CMSIS `SystemCoreClock`, which is computed
from the actual RCC registers, rather than hard-coding what I intended the
clock to be. The only thing I actually choose is the tick rate, and at 1 kHz
the cost is negligible compared with 10 ms rounding errors in every delay."
