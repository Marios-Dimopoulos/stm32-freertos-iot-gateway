# ADR-0004: FreeRTOS interrupt priority threshold at level 5

**Status:** Accepted
**Date:** 2026-09-30
**Phase:** 1

## Context

FreeRTOS protects its own data (ready/delayed lists, queues) with critical
sections. On Cortex-M the port does this by writing a threshold into the
BASEPRI register: interrupts at or below that urgency are held pending for
the length of the critical section, and more urgent ones keep running.

That splits the interrupt levels into two groups, and every ISR in the
project has to belong to exactly one:

- **More urgent than the threshold:** never delayed by the kernel, but must
  not call any FreeRTOS function.
- **At or below the threshold:** may be delayed briefly, and may call the
  `...FromISR()` functions.

The STM32F411 implements 4 priority bits, so there are 16 levels (0 = most
urgent, 15 = least urgent). Two hardware facts make the choice easy to get
wrong:

- After reset every interrupt is at level 0, the *most* urgent, which is on
  the "never touch the kernel" side of any threshold.
- BASEPRI takes the level in the top 4 bits of a byte (level 5 = `0x50`),
  while `NVIC_SetPriority()` takes the plain level (5). Writing a plain 5 to
  BASEPRI gives 0, which means "block nothing", so the kernel would
  silently protect nothing.

Every ISR planned for this project (SPI/DMA transfer complete, possibly I2C
events) wakes a task, so all of them need the `...FromISR()` functions.

## Options considered

### Option A: threshold at level 5
- How it works: `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`, shifted
  to `0x50` for BASEPRI. Levels 0-4 are kernel-unaware, levels 5-15 are
  kernel-aware.
- Pros: 11 kernel-aware levels, far more than the 2-4 needed, and 5
  kernel-unaware levels left over for an ISR that must never be delayed
  (e.g. precise timer capture). It is also the value used in ST's and
  FreeRTOS's own STM32 examples.
- Cons: nothing today needs a kernel-unaware ISR, so levels 0-4 are unused
  for now.

### Option B: threshold at level 1
- How it works: only level 0 is kernel-unaware, levels 1-15 are kernel-aware.
- Pros: the most levels for ISRs that use the kernel.
- Cons: only one level for a time-critical ISR, and we only need a handful of
  kernel-aware levels, so this gains nothing.

### Option C: threshold at level 15
- How it works: only level 15 is kernel-aware.
- Cons: every ISR that uses the kernel has to share one level, so none of
  them can preempt each other. That is far more restrictive than needed.

## Decision

Option A, level 5. Nothing in the hardware singles out 5 over 4 or 6; what
decided it is that the design needs only a few kernel-aware levels, and 5
leaves plenty on both sides of the line. Being the conventional value means
anyone who knows FreeRTOS on STM32 reads it without a second look.

Implemented in `FreeRTOSConfig.h` as a readable level plus a shift, so the
raw BASEPRI value can't be typed wrongly:

```
configPRIO_BITS                              4
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
configMAX_SYSCALL_INTERRUPT_PRIORITY         5 << (8 - 4)   /* 0x50 */
```

## Consequences

- **Every ISR that calls a `...FromISR()` function must be set to level 5 or
  a larger number** with `NVIC_SetPriority()` before its interrupt is
  enabled. Leaving the reset default (0) is a bug.
- The NVIC priority grouping must stay at the reset default (all 4 bits are
  preemption bits). The project uses no ST HAL, so nothing changes it, and
  `NVIC_SetPriorityGrouping()` must not be called with another value.
- `configASSERT` is required for the safety checks in the port
  (`xPortStartScheduler()` and `vPortValidateInterruptPriority()`). With it
  defined, a wrongly-prioritised ISR stops the system on its first call
  instead of corrupting the kernel once in a while.
- PendSV and SysTick are set by this port to the least urgent level, so
  context switches wait until every other ISR has finished.
- `configKERNEL_INTERRUPT_PRIORITY` is not defined: the `GCC/ARM_CM4F` port
  does not read it.
- Task priorities are a separate system (higher number = more urgent) and
  are not related to these interrupt levels. Any interrupt preempts any task.

## Interview angle

"FreeRTOS protects its lists by raising BASEPRI, not by disabling all
interrupts, so I get to choose where the line goes. ISRs above it are never
delayed but can't touch the kernel, and ISRs at or below it can use the
`FromISR` API. I set it at level 5 and the config asserts catch any ISR that
ends up on the wrong side, which by default is every interrupt after reset,
because they all start at level 0."
