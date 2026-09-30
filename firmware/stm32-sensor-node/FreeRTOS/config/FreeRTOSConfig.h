/*
 * FreeRTOSConfig.h — kernel configuration for the STM32F411RE sensor node.
 *
 * Target:  STM32F411RE (Cortex-M4F), Keil MDK-ARM / Arm Compiler 6
 * Kernel:  FreeRTOS V11.3.1, port GCC/ARM_CM4F
 *
 * Every FreeRTOS source file includes this header (via FreeRTOS.h) before
 * anything else, so these macros decide which kernel features even get
 * compiled. Anything NOT defined here falls back to the default in
 * FreeRTOS/include/FreeRTOS.h — and some of those defaults are the opposite
 * of what this project wants (e.g. dynamic allocation defaults to ON), so
 * important settings are always defined explicitly here, never left implicit.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ==========================================================================
 * MEMORY ALLOCATION MODEL
 * ==========================================================================
 *
 * Project rule: NO dynamic memory allocation in the firmware (see docs/plan.md).
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

/* ==========================================================================
 * CLOCK AND TICK
 * ==========================================================================
 *
 * How the tick is produced (all of this happens once, inside
 * vTaskStartScheduler() -> xPortStartScheduler() -> vPortSetupTimerInterrupt(),
 * see portable/GCC/ARM_CM4F/port.c):
 *
 *   SysTick is a 24-bit down-counter inside the Cortex-M4 core. Its VAL
 *   register decreases by 1 on every CPU clock cycle. When VAL reaches 0 it
 *   raises the SysTick exception (-> xPortSysTickHandler -> xTaskIncrementTick,
 *   which does xTickCount++ and wakes any delayed task whose time has come),
 *   then reloads VAL from the LOAD register and starts again.
 *
 *   One tick therefore lasts (LOAD + 1) CPU cycles, and the port computes:
 *
 *       LOAD = configCPU_CLOCK_HZ / configTICK_RATE_HZ - 1      (port.c:827)
 *
 *   - configCPU_CLOCK_HZ is NOT a choice: it must equal the real CPU clock
 *     as configured by the RCC. If it doesn't, every tick (and therefore
 *     every vTaskDelay / timeout) is silently wrong by the ratio of the two.
 *   - configTICK_RATE_HZ IS the choice: it sets the kernel's time resolution.
 *
 * LOAD is written once and never changes. Delays are NOT programmed into
 * SysTick — they are counted in software, in ticks, by the kernel.
 *
 * Reference: PM0214 (STM32 Cortex-M4 programming manual), section 4.5,
 * "SysTick timer".
 */

/*
 * SystemCoreClock is the CMSIS global variable holding the current CPU
 * (HCLK) frequency in Hz. It is DEFINED (storage allocated) in
 * RTE/Device/STM32F411RETx/system_stm32f4xx.c; this line only DECLARES it,
 * so that port.c — which expands configCPU_CLOCK_HZ below — knows the name
 * and its type when it compiles.
 *
 * uint32_t is already available here because FreeRTOS.h includes <stdint.h>
 * before it includes this file. Consequence: application code must include
 * "FreeRTOS.h", never "FreeRTOSConfig.h" directly (standard FreeRTOS rule).
 *
 * Many FreeRTOSConfig.h files wrap this declaration in
 * #if defined(__ICCARM__) || defined(__GNUC__) ... because some ports
 * include the config from ASSEMBLY files, where a C declaration is a syntax
 * error. The GCC/ARM_CM4F port has no assembly files, and neither does
 * startup_stm32f411xe.s include this header, so no guard is needed here.
 * If an assembly file ever includes this header, add the guard then.
 */
extern uint32_t SystemCoreClock;

/*
 * configCPU_CLOCK_HZ = SystemCoreClock
 *
 * Tells the port how fast the CPU clock (and therefore SysTick, which counts
 * CPU cycles — see configSYSTICK_CLOCK_HZ note below) really runs.
 *
 * Chosen over a hard-coded constant such as 84000000UL because this value
 * must match the REAL clock, and the real clock is going to change:
 *   - Today no clock-tree code exists: after reset SystemInit() leaves the
 *     chip on the internal 16 MHz HSI oscillator -> SystemCoreClock = 16 MHz
 *     -> LOAD = 15,999 -> 1 ms tick. Correct.
 *   - Later, our own clock setup will switch the CPU to 84 MHz via the PLL
 *     -> SystemCoreClock = 84 MHz -> LOAD = 83,999 -> 1 ms tick. Correct,
 *     with no change to this file.
 *   A constant of 84000000UL would be WRONG today: LOAD = 83,999 counted at
 *   16 MHz = 5.25 ms per tick, so every delay would be 5.25x too long,
 *   with no error or warning.
 *
 * The price of this choice is ONE ordering rule in main(), which must be:
 *
 *     1. clock setup (PLL etc.)            -- sets the real clock
 *     2. SystemCoreClockUpdate();          -- READS the RCC registers and
 *                                             stores the result in
 *                                             SystemCoreClock (it configures
 *                                             nothing)
 *     3. vTaskStartScheduler();            -- port reads this macro ONCE
 *                                             and programs SysTick LOAD
 *
 * If step 2 is skipped after changing the clock, SystemCoreClock keeps its
 * old value and we are back to the silent mismatch described above.
 *
 * Verification (hypothesis-driven, once the first task exists): in the Keil
 * debugger, Peripherals -> Core Peripherals -> System Tick Timer should show
 * LOAD = 15999 on HSI (83999 after the PLL step); a GPIO toggled every
 * pdMS_TO_TICKS(500) should show a 1.000 s period in PulseView.
 */
#define configCPU_CLOCK_HZ                  ( SystemCoreClock )

/*
 * configTICK_RATE_HZ = 1000  ->  1 tick = 1 ms
 *
 * The tick is the smallest unit of time the kernel can measure: every
 * vTaskDelay(), queue/semaphore timeout and software timer is a whole number
 * of ticks. Tick rate (Hz) = 1 / tick period, so a 1 ms resolution means
 * 1000 Hz.
 *
 * Why 1000 and not 100 (10 ms):
 *   - pdMS_TO_TICKS(ms) = ms * configTICK_RATE_HZ / 1000 is exact for every
 *     whole number of milliseconds. At 100 Hz, pdMS_TO_TICKS(5) = 0, and
 *     vTaskDelay(0) does not sleep at all (tasks.c, vTaskDelay: the
 *     "xTicksToDelay > 0" check) — a silent 5 ms -> 0 ms error.
 *   - A delay can end up to one tick early (the call can happen anywhere
 *     between two ticks), so the tick period is also the worst-case error
 *     of every delay: 1 ms instead of 10 ms.
 *   - The cost is one SysTick interrupt per ms. When no task needs waking,
 *     the handler is short; the expected overhead is well under 1% of the
 *     CPU at 84 MHz (to be measured, not assumed, once the system runs).
 *   - Lower tick rates mainly help battery-powered designs (fewer wakeups).
 *     This node is USB-powered.
 *
 * Arithmetic checks (SysTick LOAD is 24 bits, max 16,777,215):
 *   16 MHz / 1000 = 16,000 -> LOAD = 15,999   (exact, fits)
 *   84 MHz / 1000 = 84,000 -> LOAD = 83,999   (exact, fits)
 */
#define configTICK_RATE_HZ                  ( 1000 )

/*
 * configUSE_PREEMPTION = 1  (preemptive scheduling)
 *
 * When a task with a HIGHER priority than the running task becomes ready
 * (e.g. its delay expires on a tick, or an ISR gives it data through a
 * queue), the kernel switches to it immediately: it sets PendSV pending, and
 * the context switch happens as soon as no other interrupt is running.
 *
 * With 0 (cooperative scheduling) the higher-priority task would have to
 * wait until the running task gives up the CPU voluntarily (blocks or calls
 * taskYIELD()), so response time would depend on the slowest task's code.
 * Preemption is what makes the scheduler real-time.
 *
 * Required: FreeRTOS.h raises #error if this is not defined.
 */
#define configUSE_PREEMPTION                1

/*
 * configUSE_TIME_SLICING = 1
 *
 * If several ready tasks share the SAME (highest) priority, the kernel
 * switches between them on every tick, so each gets a turn. Tasks of
 * different priorities are unaffected: the highest-priority ready task
 * always runs.
 *
 * This is already the default (FreeRTOS.h); defined explicitly so the
 * choice is visible.
 */
#define configUSE_TIME_SLICING              1

/*
 * configTICK_TYPE_WIDTH_IN_BITS = 32 bits
 *
 * Size of TickType_t, i.e. of the tick counter xTickCount and of every delay
 * or timeout value.
 *   - 32 bits is the Cortex-M4's native word size: a single load/store
 *     reads or writes it, so it is atomic (can't be read half-updated) and
 *     cheap.
 *   - At 1000 Hz it wraps around after 2^32 ms ~= 49.7 days. The kernel
 *     handles the wrap correctly (it keeps a separate overflow delayed-task
 *     list), so the device can run longer than that.
 *   - 16 bits would limit the longest single delay to 65,535 ticks ~= 65 s
 *     at 1000 Hz; 64 bits would be non-atomic on a 32-bit CPU.
 *
 * Required: FreeRTOS.h raises #error if neither this nor the older
 * configUSE_16_BIT_TICKS is defined.
 */
#define configTICK_TYPE_WIDTH_IN_BITS       TICK_TYPE_WIDTH_32_BITS

/*
 * Deliberately NOT defined (defaults are what we want):
 *
 *   configSYSTICK_CLOCK_HZ — defaults to configCPU_CLOCK_HZ (port.c), and
 *     the port then sets SysTick to count the CPU clock itself (CLKSOURCE
 *     bit = 1). Only needed to use the slower reference clock (HCLK/8 on
 *     STM32), which we have no reason to do.
 *
 *   configUSE_TICKLESS_IDLE — defaults to 0. Low-power feature that stops
 *     the tick while only the Idle task runs. Not needed on USB power, and
 *     it complicates tick accounting.
 */

/* ==========================================================================
 * TASKS AND STACKS
 * ==========================================================================
 *
 * Reminder of what a task is made of (all statically allocated, ADR-0001):
 *   - its CODE: the task function, in flash;
 *   - its STACK: a StackType_t array in RAM — local variables, return
 *     addresses, and the task's saved registers while it is not running;
 *   - its TCB (Task Control Block): a StaticTask_t in RAM — the kernel's
 *     record of the task (saved stack pointer as the FIRST member, which
 *     PendSV reads/writes directly; state-list and event-list items;
 *     priority; stack start for overflow checks; name; ...).
 * The TCB is NOT on the stack: they are two separate objects, both handed
 * to the kernel by us.
 */

/*
 * configMAX_PRIORITIES — DELIBERATELY NOT DEFINED YET.
 *
 * Number of task priority levels (0 = lowest, used by the Idle task). It
 * must equal the number of distinct URGENCY classes in the design, not the
 * number of tasks: tasks of equal urgency share a level and time-slice.
 * The task set has not been decided yet (see "Open questions" in
 * docs/architecture.md), so no value can be justified yet.
 *
 * It is REQUIRED (FreeRTOS.h raises #error without it): it must be decided,
 * or set provisionally and clearly marked as such, before the first build.
 * Upper limit: 32, because of configUSE_PORT_OPTIMISED_TASK_SELECTION below.
 */

/*
 * configMINIMAL_STACK_SIZE = 128  —  UNIT IS WORDS (StackType_t = uint32_t),
 *                                    so 128 words = 512 bytes.
 *
 * The kernel uses this as the Idle task's stack size. Because
 * configKERNEL_PROVIDED_STATIC_MEMORY = 0, the Idle stack array is actually
 * declared in OUR vApplicationGetIdleTaskMemory(); that function will size
 * the array with this same macro, so there is a single source of truth.
 *
 * Why 128 words (a starting value, to be MEASURED and trimmed later):
 *   - Worst-case saved context when Idle is switched out:
 *       hardware exception frame incl. FPU state   26 words
 *       R4-R11 + EXC_RETURN (saved by PendSV)       9 words
 *       S16-S31 (saved by PendSV if FPU was used)  16 words
 *                                                 --------
 *                                                  51 words = 204 bytes
 *     (without FPU use it is only 17 words; the FPU part is stacked only if
 *     the task executed a floating-point instruction.)
 *   - Idle's own call depth is small: prvIdleTask() and the kernel functions
 *     it calls. There is no idle hook (see configUSE_IDLE_HOOK) adding user
 *     code on top.
 *   - ISRs do NOT use this stack (they run on the MSP), apart from the one
 *     hardware exception frame already counted above.
 *   - 128 words is ~2.5x the worst-case frame, matching the FreeRTOS
 *     Cortex-M demo configurations.
 *
 * How it will be measured instead of guessed:
 *   - uxTaskGetStackHighWaterMark() at run time (enabled below);
 *   - Keil's static call-graph report (Objects/stm32-sensor-node.htm,
 *     "Maximum Stack Usage"), generated by the linker on every build.
 *
 * Trap to remember: writing 512 here "meaning bytes" would reserve 2 KB.
 */
#define configMINIMAL_STACK_SIZE                    ( 128 )

/*
 * configUSE_IDLE_HOOK = 0
 *
 * If 1, the Idle task calls a user function vApplicationIdleHook() on every
 * loop — typically used to put the CPU to sleep (__WFI) to save power, or
 * for background housekeeping. We need neither (USB-powered), and a hook
 * would add its own call depth to the Idle task's stack.
 *
 * Required: FreeRTOS.h raises #error if this is not defined.
 */
#define configUSE_IDLE_HOOK                         0

/*
 * configUSE_TICK_HOOK = 0
 *
 * If 1, a user function vApplicationTickHook() is called from INSIDE the
 * SysTick interrupt, on every tick (1000 times per second). Anything placed
 * there adds latency to every tick. We have no use for it.
 *
 * Required: FreeRTOS.h raises #error if this is not defined.
 */
#define configUSE_TICK_HOOK                         0

/*
 * configUSE_TIMERS = 0  (no software timers, no Timer Service task)
 *
 * Software timers are a kernel service ("call this function in / every N
 * ticks"), built on top of the tick. All timer callbacks run inside a
 * hidden Timer Service task, which would cost a stack, a TCB, a command
 * queue and a priority level — and, with static allocation, one more
 * callback for us to write (vApplicationGetTimerTaskMemory()).
 *
 * Our periodic work (reading sensors at a fixed interval) fits naturally
 * inside tasks using vTaskDelayUntil(), which wakes at a fixed RATE
 * without drift. So the Timer task would be pure overhead.
 *
 * This is already the default; defined explicitly because it is a design
 * decision and it is what removes the Timer-task memory callback.
 *
 * Known catch: some APIs defer their work to the Timer task — notably
 * xEventGroupSetBitsFromISR() and xTimerPendFunctionCall(). Using either
 * would require turning timers on.
 */
#define configUSE_TIMERS                            0

/*
 * configUSE_PORT_OPTIMISED_TASK_SELECTION = 1
 *
 * How the scheduler finds the highest-priority ready task:
 *   - 0 (generic C): walk the ready lists from the top priority down until a
 *     non-empty one is found — time grows with configMAX_PRIORITIES.
 *   - 1 (port-optimised): the kernel keeps a 32-bit bitmap, one bit per
 *     priority that has ready tasks, and the Cortex-M4 CLZ (Count Leading
 *     Zeros) instruction finds the highest set bit in ONE instruction —
 *     constant time, regardless of the number of priorities.
 *
 * Cost: configMAX_PRIORITIES must be <= 32 (one bit per priority in a
 * 32-bit word) — far more than this design will need.
 *
 * The CM4F port already defaults this to 1 (portmacro.h); defined
 * explicitly because it constrains the still-open configMAX_PRIORITIES.
 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION     1

/*
 * configCHECK_FOR_STACK_OVERFLOW = 2
 *
 * Why it matters here: with static allocation, task stacks are arrays
 * placed next to each other (and next to TCBs, queues, globals) in .bss.
 * A Cortex-M stack grows DOWNWARD, so an overflowing task silently writes
 * over whatever the linker placed just below its array. Nothing crashes at
 * that moment; something unrelated misbehaves later. The Cortex-M4 has no
 * hardware stack-limit register (the M33's PSPLIM does not exist here), so
 * without this check nothing would notice.
 *
 * Both methods run at every context switch (a few cycles each):
 *   - Method 1: is the task's saved stack pointer outside its stack array?
 *   - Method 2: method 1, PLUS are the last 16 bytes at the far end of the
 *     stack still the 0xA5 fill pattern the kernel wrote at creation?
 *     Catches an overflow that went too deep and then came back up before
 *     the switch happened.
 *
 * Limits (be honest about them):
 *   - It detects AFTER the fact: the neighbouring memory may already be
 *     corrupted. The hook must therefore stop the system, not try to
 *     recover.
 *   - It does not check ISRs (they run on the MSP, not on task stacks).
 *
 * Obligation this creates: we must implement
 *     void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName );
 * otherwise the link fails. It will disable interrupts and halt where the
 * debugger can show which task overflowed.
 */
#define configCHECK_FOR_STACK_OVERFLOW              2

/*
 * INCLUDE_uxTaskGetStackHighWaterMark = 1
 *
 * Compiles uxTaskGetStackHighWaterMark(), which returns the MINIMUM amount
 * of free stack (in words) a task has ever had since it started — computed
 * by counting how many 0xA5 fill words at the far end of the stack were
 * never overwritten. This is how the starting values above (e.g. the 128
 * words of configMINIMAL_STACK_SIZE) get replaced by measured numbers.
 *
 * Defaults to 0 in FreeRTOS.h, so it must be switched on explicitly.
 */
#define INCLUDE_uxTaskGetStackHighWaterMark         1

/*
 * Deliberately NOT defined (default is fine):
 *
 *   configMAX_TASK_NAME_LEN — defaults to 16: each TCB stores a copy of the
 *     task's name (16 bytes) for debugging. No reason to change it.
 */

#endif /* FREERTOS_CONFIG_H */
