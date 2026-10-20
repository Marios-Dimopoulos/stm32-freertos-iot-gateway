# ADR-0005: Size task stacks by measurement with a margin; Idle stays at 128 words

**Status:** Accepted
**Date:** 2026-10-03
**Phase:** 1

## Context

With static allocation (ADR-0001) every task stack is a fixed array declared
in my code, so its size must be chosen up front. Too small and the task
overflows: a Cortex-M stack grows downward, so it silently overwrites whatever
the linker placed below the array (another task's TCB, a queue, a global).
Nothing crashes at that moment, and the failure shows up later somewhere
unrelated. `configCHECK_FOR_STACK_OVERFLOW = 2` detects it only after the fact.

Too large wastes RAM. Here RAM is not scarce (about 4 KB of 128 KB used), so the
size matters mostly as a discipline: every number should have a reason.

Facts that bound the choice:

- A saved context costs **17 words (68 B)** without the FPU (8 words pushed by
  the hardware + R4-R11 and EXC_RETURN pushed by PendSV), and up to **51
  words (204 B)** if the task used the FPU. A task stack can never be smaller
  than that, plus the task's own call frames.
- `uxTaskGetStackHighWaterMark()` and the `0xA5` fill the kernel writes at
  task creation measure the peak by counting the untouched bytes at the low
  end of the array.
- Keil's static call-graph report (`Objects/*.htm`) covers call chains but
  not interrupt frames, context saves or function pointers. It said 324 B for
  the start-up chain on the main stack, which says nothing about task stacks.

### Measurements (blink task and Idle, 20-30 s of running)

| Task | Array | Peak by the `0xA5` pattern | Peak actually written |
|---|---|---|---|
| Idle | 128 words (512 B) | 14 words | **at least 21 words (84 B)** |
| Blink | 128 words (512 B) | 27 words (first non-`0xA5` byte at `0x2000045C`) | not checked |

The Idle row is the important one. The pattern said 14 words, but a context
switch alone needs 17, so the number had to be wrong. Idle's saved stack pointer
(first word of `xIdleTaskTCB`, `0x20000264`) showed the cause: the 8 words of
R4-R11, `0x264`-`0x283`, are all `0xA5A5A5A5`. Those registers were loaded
from the fake initial frame at the first start and Idle's code never changed
them, so each context save writes the same value back. The memory was written
but looks untouched, and the first non-`0xA5` byte lands exactly on the saved
EXC_RETURN at `0x284`. **The `0xA5` pattern can be forged by real data, so a
measured peak can under-report.**

### Measurement of the sensor task (2026-10-08)

The blink task was replaced by the real sensor task (BMP280 over I2C, the
formatter, the UART), with a 256-word (1 KB) stack. For a while it printed
`uxTaskGetStackHighWaterMark()` every 10 s (temporary code, now removed).
Result: **167 words free at the minimum, so the peak is 89 words (356 B)**.

The value was the same with the sensor connected and with it disconnected (the
`NACK` errors, then the re-initialisation after it was reconnected). That is
expected: the deepest path is the successful init (the 24-byte trimming buffer,
the I2C receive sequence with its critical section), and it runs at start-up,
before the first report; a failing init returns at its first I2C call, so it is
shallower. The 89 words are still a lower bound. They very likely include
interrupt frames (when SysTick or USART2 fire, the hardware pushes an 8-word frame
onto the task's stack, and the I2C busy-waits last long enough compared with the
1 ms tick for that to have happened many times), but that was not forced.

### Measurement of the sensor and transmit tasks, Phase 2 (2026-10-18)

Both tasks with their final code (frame building, the CRC, the queue, the SPI re-arm).
Method: no code added. I ran `tools/spi_frame_test.py poll --count 60 --period 1.0` from
the Pi (60 reads, one a second, so one minute of the sensor task running and 60
wake-ups of the transmit task), pressed Stop and counted in the Memory window the bytes
still equal to `0xA5` from the lowest address of each stack array (addresses from the
map file).

| Task | Array | Untouched `0xA5` | Peak used | Array / peak |
|---|---|---|---|---|
| Sensor | 256 words (1024 B) at `0x20000540` | 640 B (160 words) | **96 words (384 B)** | 2.7x |
| Transmit | 128 words (512 B) at `0x20000998` | 356 B (89 words) | **39 words (156 B)** | 3.3x |

- Both peaks are above the 17-word minimum of a context switch, so the cross-check
  above does not flag a forged pattern.
- The sensor task went from 89 words (Phase 1) to 96 words with the frame code: 7 more.
- The rule (peak plus at least 50%) asks for 144 words (sensor) and 59 words (transmit).
  Both arrays are larger, so **neither size changes**.
- Limits, as before: a lower bound. It covers one minute with the sensor connected
  (not the NACK paths). For the transmit task, the placeholder path
  (`frame_build()` when no sensor frame exists yet) did not run: it needs a transaction
  to end within about 20 ms of power-up. `frame_build()` also runs in the sensor task,
  whose peak includes it. Interrupt frames (8 words each, pushed on the task's stack by
  the hardware) are in the numbers only if one happened at the deepest moment.

## Options considered

### Option A: estimate from the call graph and round up
- How it works: add up frame sizes along the deepest call chain, add the
  context, choose a round number.
- Pros: needs no running system.
- Cons: the call graph misses interrupt frames and function pointers, and the
  compiler's frame sizes change with every optimisation setting.

### Option B: measure the peak, then size with a margin
- How it works: run the task through all its code paths, read the high-water
  mark (or the `0xA5` run in the debugger), and set the size to at least 1.5x
  the measured peak. Never go below the context size (17 words, or 51 with
  the FPU). Cross-check the measurement against that minimum.
- Pros: based on what the code really does, and the cross-check catches the
  forged-pattern case above.
- Cons: only covers the paths that ran; must be repeated whenever the code
  changes.

### Option C: trim to the minimum measured
- How it works: shrink each stack to the smallest value that has not failed
  (e.g. Idle at 64 words).
- Pros: saves RAM (256 B on Idle).
- Cons: saves RAM I do not need to save, and removes the margin that covers
  paths not yet exercised and the under-reporting above.

## Decision

Option B. Stack sizes are chosen from measurement with a margin of at least
50% over the measured peak, checked against the theoretical context size.
`configMINIMAL_STACK_SIZE` (the Idle task) stays at **128 words (512 B)**:
Idle has been seen to write at least 21 words, so 128 is about 6x that, and
shrinking it would gain nothing while RAM is this plentiful (Option C).

Each task's stack size is a named macro whose comment records the measured
peak and how it was measured. The blink task's 128 words were a starting value
for a temporary task and did not carry over.

**The sensor task keeps its 256 words**, which is 2.9 times the Phase 1 peak of
89 words (2.7 times the 96 words measured in Phase 2). The rule alone (peak plus at
least 50%) would allow about 160 words, and that is the number Option B points at.
256 was kept on purpose: the whole program uses about 4 KB of 128 KB, so the 384 bytes
saved would buy nothing. This is a deliberate departure from the tightest value
the rule gives, not a missing measurement.

## Consequences

- A measured peak is a **lower bound**. Compare it against the context-size
  minimum before trusting it; if it is lower, find out why.
- The high-water mark covers **task stacks only**. The 1 KB main stack, shared
  by all interrupt handlers, has no check watching it (a main-stack overflow
  would run into whatever the linker placed below it, since the heap size is 0), so ISRs must stay
  small. Keil's static report says 324 B for the start-up chain, which does
  not include interrupts.
- The sensor task reported its high-water mark over USART2 during bring-up (done,
  see the measurement above; the temporary code is removed). Repeat the
  measurement whenever a task's code changes.
- Revisit the Idle size only if RAM becomes tight or a hook is added to Idle.

## Interview angle

"I size each task stack from a measured peak plus a margin and cross-check it
against the minimum a context switch needs. That check mattered: the
high-water mark said Idle used 14 words, but I knew a context save alone is 17,
and it turned out the saved registers still held the fill pattern, so the
pattern had been forged by real data. A measurement is a lower bound until it
has been checked against something you can calculate."
