# ADR-0001: Static-only memory allocation for FreeRTOS objects

**Status:** Accepted
**Date:** 2026-09-28
**Phase:** 1

## Context

The STM32F411RE sensor node runs FreeRTOS on 128KB of SRAM and is meant to
run unattended for long periods. It needs a small, fixed set of RTOS objects:
a sensor task, later a transmit task, a queue to connect them, and the
kernel's own Idle (and possibly Timer Service) task.

FreeRTOS can get memory for these objects in two ways: from a heap at run
time (`xTaskCreate` → `pvPortMalloc`, backed by one of `heap_1.c`–`heap_5.c`),
or from buffers the application declares at compile time (`xTaskCreateStatic`
and friends). The kernel's default is dynamic allocation enabled.

If I did nothing, I would get the default: a heap sized by guesswork,
objects that can fail to be created at run time if the heap is too small or
fragmented, and RAM usage that only shows up as one opaque `ucHeap[]` array
in the map file. CLAUDE.md sets "no dynamic memory allocation in the FreeRTOS
firmware" as a hard constraint.

## Options considered

### Option A: Dynamic allocation with `heap_4.c`
- How it works: `configSUPPORT_DYNAMIC_ALLOCATION = 1`, a `configTOTAL_HEAP_SIZE`
  byte array, objects created with `xTaskCreate()` etc. `heap_4` merges
  adjacent free blocks to reduce fragmentation.
- Pros: Least code; no buffers to declare. Objects can be created and deleted
  at run time. It's the default and what most tutorials use.
- Cons: Creation can fail at run time (every call site needs a NULL check,
  and there's no good recovery on a headless node). Allocation time isn't
  deterministic. Fragmentation is still possible if objects are deleted. The
  heap size is a guess, and the per-object RAM breakdown isn't visible in the
  map file.

### Option B: Dynamic allocation with `heap_1.c` (allocate once, never free)
- How it works: Same APIs as Option A, but `vPortFree()` does nothing. All
  objects are created once at startup.
- Pros: No fragmentation, since nothing is ever freed. Deterministic. Still
  uses the convenient `xTaskCreate()` API.
- Cons: Still one opaque heap array whose size has to be tuned by hand.
  Creation can still fail at run time if the heap is too small. The "never
  delete" rule is a convention, not something the build enforces.

### Option C: Static allocation only
- How it works: `configSUPPORT_STATIC_ALLOCATION = 1`,
  `configSUPPORT_DYNAMIC_ALLOCATION = 0`, and no `heap_x.c` in the build.
  Every object's control block and storage is a file-scope `static` array
  passed to the `...Static()` API. With `configKERNEL_PROVIDED_STATIC_MEMORY = 0`,
  the application also supplies the Idle and Timer task memory via
  `vApplicationGetIdleTaskMemory()` / `vApplicationGetTimerTaskMemory()`.
- Pros: All RAM use is decided at link time. If it doesn't fit, the build
  fails instead of the device. Object creation can't fail from lack of memory.
  Every buffer appears by name in the `.map` file. The toolchain enforces the
  rule: `xTaskCreate()` doesn't exist in this configuration, and
  `pvPortMalloc()` can't be linked.
- Cons: More boilerplate (a TCB and a stack array per task, a storage buffer
  per queue, two callbacks for the kernel tasks). Objects can't be sized
  or created on demand at run time. Some FreeRTOS features that need the heap
  (e.g. `configUSE_STATS_FORMATTING_FUNCTIONS`) become unavailable.

## Decision

Option C. Its deciding advantage is that the compiler and linker enforce the
rule. Options A and B depend on discipline and on sizing a heap correctly,
while Option C turns any mistake into a build error that shows up on the
developer's machine, not in the field.

## Consequences

- Easier: RAM budgeting. Per-object usage can be read from the map file, and
  running out of memory at run time is impossible for RTOS objects.
- Easier: reviews. Any `xTaskCreate()` / `pvPortMalloc()` use fails to build,
  so reviewers don't have to look for it.
- Harder: every new task or queue needs its buffers declared explicitly, and
  stack sizes must be chosen on purpose (later checked with
  `uxTaskGetStackHighWaterMark()` / stack overflow checking).
- Forecloses: creating or deleting objects at run time, and the heap-dependent
  FreeRTOS stats-formatting helpers.
- Done as a consequence:
  - `vApplicationGetIdleTaskMemory()` is in `freertos_callbacks.c`; the project does
    not link without it. `vApplicationGetTimerTaskMemory()` is not needed, because
    `configUSE_TIMERS = 0`.
  - These settings only cover the FreeRTOS allocator, so the linker `.map` file is
    checked for the C library's `malloc`/`free`: none is linked (2026-10-03, and again
    in the last map, with a start-up heap of 0 bytes; see `architecture.md`).

## Interview angle

"On a headless node with 128KB of RAM, I'd rather the linker tell me I'm out
of memory than have the device find out six months into deployment. So I
disabled dynamic allocation in FreeRTOS entirely, which means `xTaskCreate`
doesn't even compile, and every task and queue gets a static buffer that I
can see in the map file."
