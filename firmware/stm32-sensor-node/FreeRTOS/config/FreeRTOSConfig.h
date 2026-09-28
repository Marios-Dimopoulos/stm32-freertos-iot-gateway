/*
 * FreeRTOSConfig.h — kernel configuration for the STM32F411RE sensor node.
 *
 * Target:  STM32F411RE (Cortex-M4F), Keil MDK-ARM / Arm Compiler 6
 * Kernel:  FreeRTOS V11.3.1, port RVDS/ARM_CM4F
 *
 * Every FreeRTOS source file includes this header (via FreeRTOS.h) before
 * anything else, so these macros decide which kernel features even get
 * compiled. Anything NOT defined here falls back to the default in
 * FreeRTOS/include/FreeRTOS.h — and some of those defaults are the opposite
 * of what this project wants (e.g. dynamic allocation defaults to ON), so
 * important settings are always defined explicitly here, never left implicit.
 *
 * This file is being built section by section. Sections not yet present
 * (clock/tick, priorities, hooks, etc.) will be added in later steps.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ==========================================================================
 * MEMORY ALLOCATION MODEL
 * ==========================================================================
 *
 * Project rule: NO dynamic memory allocation in the firmware (see CLAUDE.md,
 * "Hard engineering constraints", and docs/plan.md).
 *
 * Why: this node is meant to run unattended for months. A heap that is
 * allocated from and freed at run time can fragment — enough total free
 * bytes, but no single contiguous block large enough — and an allocation
 * that fails deep inside the kernel means a task or queue silently never
 * exists, or a HardFault. Allocation time is also not deterministic, which
 * is a problem for real-time code. With static allocation, every byte of
 * RAM the RTOS uses is decided by the linker at build time and is visible
 * in the .map file; if it doesn't fit in 128KB SRAM, the BUILD fails,
 * not the device in the field.
 *
 * The three settings below enforce this rule at compile/link time rather
 * than relying on discipline.
 */

/*
 * configSUPPORT_STATIC_ALLOCATION = 1
 *
 * Compiles the "...Static()" creation APIs: xTaskCreateStatic(),
 * xQueueCreateStatic(), xSemaphoreCreateMutexStatic(), xTimerCreateStatic(),
 * etc. With these, the caller supplies the memory for the object's control
 * block (e.g. StaticTask_t for a task's TCB) and its storage (the task's
 * stack array, the queue's item buffer), normally as file-scope `static`
 * arrays that the linker places in .bss.
 *
 * Consequence: the kernel itself creates the Idle task (and the Timer
 * Service task if software timers are enabled) inside vTaskStartScheduler().
 * With dynamic allocation disabled, it has nowhere to get their memory from,
 * so it calls two application-provided callbacks:
 *     vApplicationGetIdleTaskMemory()
 *     vApplicationGetTimerTaskMemory()   (only if configUSE_TIMERS == 1)
 * We must implement these ourselves; until we do, the link step will fail
 * with an "undefined symbol" error. That is expected, not a bug.
 */
#define configSUPPORT_STATIC_ALLOCATION     1

/*
 * configKERNEL_PROVIDED_STATIC_MEMORY = 0
 *
 * Added in FreeRTOS V11.1. If set to 1, the kernel supplies its OWN default
 * implementations of vApplicationGetIdleTaskMemory() and
 * vApplicationGetTimerTaskMemory(), backed by internal static arrays whose
 * stack sizes are derived from configMINIMAL_STACK_SIZE /
 * configTIMER_TASK_STACK_DEPTH.
 *
 * We set it to 0 so that WE write those two callbacks (in application code)
 * and choose the Idle/Timer task stack sizes deliberately. Reasons:
 *   - Every static buffer in the system is then declared in our own code,
 *     where it can be sized, reviewed and found in the .map file — no
 *     hidden kernel-owned RAM.
 *   - It forces us to understand what the kernel needs in order to start
 *     the scheduler (a TCB + a stack for each kernel-created task).
 *
 * 0 is also the default in FreeRTOS.h; it is defined explicitly here so the
 * choice is visible rather than implicit.
 */
#define configKERNEL_PROVIDED_STATIC_MEMORY 0

/*
 * configSUPPORT_DYNAMIC_ALLOCATION = 0
 *
 * Removes the heap-based creation APIs from the kernel entirely:
 * xTaskCreate(), xQueueCreate(), xSemaphoreCreateMutex(), xTimerCreate(),
 * etc. are wrapped in #if (configSUPPORT_DYNAMIC_ALLOCATION == 1) in the
 * kernel sources, so with 0 they simply do not exist. Any code that calls
 * one fails to compile/link — the "no malloc" rule is enforced by the
 * toolchain instead of by code review.
 *
 * This MUST be set explicitly: FreeRTOS.h defaults it to 1 for backward
 * compatibility, so omitting it would silently re-enable dynamic allocation.
 *
 * Note: FreeRTOS.h contains an #error if BOTH static and dynamic support are
 * 0 — the kernel needs at least one way to get memory for its objects,
 * which is why STATIC must be 1 above.
 */
#define configSUPPORT_DYNAMIC_ALLOCATION    0

/*
 * configTOTAL_HEAP_SIZE = 0
 *
 * Size in bytes of the ucHeap[] array used by the FreeRTOS heap
 * implementations heap_1.c / heap_2.c / heap_4.c (the kernel core itself
 * never reads this macro). This project deliberately compiles NO heap_x.c
 * file, so this value currently has no effect on the build.
 *
 * It is still defined, as 0, to document intent: a reader should see
 * "zero heap, on purpose", not wonder whether it was forgotten.
 *
 * Second safety net: because no heap_x.c is in the project, the symbol
 * pvPortMalloc() does not exist at all — anything that tries to use the
 * FreeRTOS heap fails at link time.
 *
 * Limitation (be honest about it): these settings only govern the FreeRTOS
 * allocator. They do NOT prevent the C library's own malloc() from being
 * pulled in (e.g. by some printf/sprintf implementations). That has to be
 * verified separately by checking the linker .map file for malloc/free.
 */
#define configTOTAL_HEAP_SIZE               0

#endif /* FREERTOS_CONFIG_H */
