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
 * i must implement these ourselves; until i do, the link step will fail
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
 * i set it to 0 so that i write those two callbacks (in application code)
 * and choose the Idle/Timer task stack sizes deliberately. Reasons:
 *   - Every static buffer in the system is then declared in my own code,
 *     where it can be sized, reviewed and found in the .map file — no
 *     hidden kernel-owned RAM.
 *   - It forces me to understand what the kernel needs in order to start
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
 *   - Later, my own clock setup will switch the CPU to 84 MHz via the PLL
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
 * old value and i are back to the silent mismatch described above.
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
 * Deliberately NOT defined (defaults are what i want):
 *
 *   configSYSTICK_CLOCK_HZ — defaults to configCPU_CLOCK_HZ (port.c), and
 *     the port then sets SysTick to count the CPU clock itself (CLKSOURCE
 *     bit = 1). Only needed to use the slower reference clock (HCLK/8 on
 *     STM32), which i have no reason to do.
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
 * to the kernel by me.
 */

/*
 * configMAX_PRIORITIES = 3   ->  task priorities 0, 1 and 2
 *
 * Number of TASK priority levels (higher number = more urgent; 0 is the
 * lowest and is used by the Idle task). It equals the number of distinct
 * URGENCY classes in the design, not the number of tasks: tasks of equal
 * urgency share a level and time-slice. The kernel keeps one ready list per
 * level (pxReadyTasksLists[], about 20 bytes each), so unused levels cost
 * RAM and scheduler bookkeeping for nothing.
 *
 * The task set this is derived from (see docs/architecture.md):
 *
 *   priority 2   Transmit task (Phase 2) — prepares each SPI/DMA buffer for
 *                the Raspberry Pi. The Pi is the SPI MASTER: it clocks data
 *                out whenever IT decides, so this task's deadline is imposed
 *                from outside and it must never wait behind other work.
 *   priority 1   Sensor task (Phase 1) — reads the BMP280 over I2C1 at a
 *                period i choose. Running a few ms late changes nothing
 *                (temperature/pressure do not move that fast). During
 *                bring-up it also prints readings over USART2; that blocking
 *                debug output can always be preempted by priority 2.
 *   priority 0   Idle task (created by the kernel).
 *
 * Why not more levels: no other task is planned. The MPU-6050 IMU was
 * dropped from the design, and debug logging is done directly from the
 * sensor task (no dedicated logging task), so neither adds an urgency class.
 * Level 2 has no task until Phase 2; it is defined now because the transmit
 * task is a committed part of the plan and its priority relative to the
 * sensor task is already a justified design decision.
 *
 * Changing this later is a one-line edit; nothing else depends on the exact
 * number. Upper limit: 32, because of
 * configUSE_PORT_OPTIMISED_TASK_SELECTION below.
 *
 * Required: FreeRTOS.h raises #error if this is not defined.
 */
#define configMAX_PRIORITIES                        ( 3 )

/*
 * configMINIMAL_STACK_SIZE = 128  —  UNIT IS WORDS (StackType_t = uint32_t),
 *                                    so 128 words = 512 bytes.
 *
 * The kernel uses this as the Idle task's stack size. Because
 * configKERNEL_PROVIDED_STATIC_MEMORY = 0, the Idle stack array is actually
 * declared in my vApplicationGetIdleTaskMemory(); that function will size
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
 * for background housekeeping. i need neither (USB-powered), and a hook
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
 * there adds latency to every tick. i have no use for it.
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
 * callback for me to write (vApplicationGetTimerTaskMemory()).
 *
 * my periodic work (reading sensors at a fixed interval) fits naturally
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
 * explicitly because it puts an upper limit on configMAX_PRIORITIES.
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
 * Obligation this creates: i must implement
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

/* ==========================================================================
 * INTERRUPT PRIORITIES
 * ==========================================================================
 *
 * This section is about INTERRUPT priorities only. Do not confuse the two
 * separate priority systems:
 *
 *                   TASK priority                INTERRUPT priority
 *   managed by      FreeRTOS scheduler (SW)      NVIC, inside the CPU (HW)
 *   set with        xTaskCreateStatic() arg      NVIC_SetPriority()
 *   direction       HIGHER number = more urgent  LOWER number = more urgent
 *   range           0..configMAX_PRIORITIES-1    levels 0..15 on the STM32F411
 *
 * The two are never compared with each other: ANY interrupt, even the least
 * urgent one (level 15), interrupts ANY task, even the highest-priority one.
 *
 * Why the kernel cares about interrupt levels at all:
 *   While the kernel edits its own data (e.g. moving a TCB between the
 *   Ready and Delayed lists), an ISR that also calls a FreeRTOS function
 *   (xQueueSendFromISR(), ...) would edit the same lists halfway through and
 *   corrupt them. So for those few instructions (a "critical section") the
 *   kernel writes a threshold into the CPU's BASEPRI register:
 *
 *       BASEPRI = 0         block nothing (the normal state, almost always)
 *       BASEPRI = level N   interrupts at level N..15 are held PENDING (not
 *                           lost — they run as soon as BASEPRI returns to
 *                           0); levels 0..N-1 still run immediately
 *
 *   BASEPRI is a hardware register in the Cortex-M4 core (accessed with the
 *   MSR/MRS instructions, see vPortRaiseBASEPRI() in portmacro.h), not a
 *   variable. It is raised by the kernel for the whole system — never "per
 *   task". Application code can request the same protection with
 *   taskENTER_CRITICAL() / taskEXIT_CRITICAL(), and must keep it just as
 *   short.
 *
 * The threshold therefore draws ONE line through the 16 levels:
 *
 *     level 0 .. T-1   more urgent than the line. NEVER delayed by the
 *                      kernel — but their ISRs must NEVER call any FreeRTOS
 *                      function.
 *     ---------------- T = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY ----
 *     level T .. 15    at or below the line. May be delayed for the length
 *                      of a critical section (a few instructions) — and
 *                      their ISRs MAY call the ...FromISR() functions.
 *                      (PendSV and SysTick are hard-coded by this port to
 *                      the least urgent level; see port.c, portNVIC_PENDSV_PRI
 *                      / portNVIC_SYSTICK_PRI.)
 *
 * An ISR gets either "never delayed" or "may talk to the kernel", never
 * both.
 */

/*
 * configPRIO_BITS = 4
 *
 * NOT read by the kernel — a helper for the shift below. It is the number of
 * interrupt-priority bits this chip implements: the STM32F411 implements 4
 * (16 levels; __NVIC_PRIO_BITS in the device header stm32f411xe.h).
 *
 * The encoding it exists for: each interrupt's priority is an 8-bit field,
 * but only the TOP 4 bits are implemented; the low 4 always read as 0:
 *
 *     bit   7 6 5 4 | 3 2 1 0
 *          [ level ] [always 0]      level 5  -> 0101 0000 = 0x50
 *                                    level 15 -> 1111 0000 = 0xF0
 *
 * (ARM puts the implemented bits at the top so the same code works on chips
 * with a different number of priority bits.)
 */
#define configPRIO_BITS                                 4

/*
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5   (the line, as a plain LEVEL)
 *
 * NOT read by the kernel either — this is the human-readable level, in the
 * same form NVIC_SetPriority() takes. Application code uses it when giving
 * priorities to ISRs that call FreeRTOS functions, e.g.
 *     NVIC_SetPriority( SomeIRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1 );
 *
 * Why 5:
 *   - Every ISR currently planned (SPI/DMA transfer complete, possibly I2C
 *     events) has to wake a task, i.e. call a ...FromISR() function, so all
 *     of them must sit at level >= T. With T = 5 there are 11 such levels
 *     (5..15) — far more than the 2-4 i need.
 *   - Levels 0..4 stay free for possible future ISRs that must never be
 *     delayed by the kernel (e.g. precise timer capture).
 *   - It cannot be 0: BASEPRI = 0 means "block nothing".
 *   - 5 rather than 4 or 6 has no deep technical reason; it is the value
 *     used by ST's and FreeRTOS's own STM32 examples, so it is what anyone
 *     familiar with FreeRTOS on STM32 expects to see.
 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

/*
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << (8 - 4) = 0x50
 *
 * THE setting the kernel actually reads: the value it writes into BASEPRI
 * when it enters a critical section (portmacro.h, vPortRaiseBASEPRI(); also
 * inside xPortPendSVHandler() in port.c). The name means "the most urgent
 * interrupt level that may still make system calls (call FreeRTOS
 * functions)".
 *
 * BASEPRI needs the RAW, already-shifted byte (0x50), unlike
 * NVIC_SetPriority(), which takes the plain level (5) and shifts it itself.
 *
 * The classic bug this shift prevents: writing a plain 5 here. 0x05 has only
 * unimplemented bits set, so the hardware stores 0x00 = "block nothing", and
 * every kernel critical section silently protects nothing. (The port checks
 * for this in xPortStartScheduler(), but only when configASSERT is defined.)
 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY            ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/*
 * Rules that follow from this section, for when ISRs are written:
 *
 *   1. Every ISR that calls a ...FromISR() function MUST be given a level
 *      >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY with
 *      NVIC_SetPriority(). After reset every interrupt is at level 0 — the
 *      MOST urgent, i.e. above the line — so leaving the default is a bug.
 *      (Checked by vPortValidateInterruptPriority() in port.c when
 *      configASSERT is defined.)
 *
 *   2. Never change the NVIC priority grouping. The 4 priority bits can be
 *      split into "preemption" and "sub-priority" parts; FreeRTOS requires
 *      all 4 to be preemption bits. The reset default already is that, and
 *      nothing in this project changes it (no ST HAL). Do not call
 *      NVIC_SetPriorityGrouping() with any other value.
 *
 * Deliberately NOT defined:
 *
 *   configKERNEL_INTERRUPT_PRIORITY — a setting from older ports. This port
 *     (V11.3.1, GCC/ARM_CM4F) does not read it: PendSV and SysTick are
 *     hard-coded to the least urgent level. Defining it would be dead code.
 */

/* ==========================================================================
 * ASSERTIONS
 * ==========================================================================
 *
 * An assert states "this condition must be true here; if it is not,
 * something is badly wrong — stop". The kernel source contains about 230 of
 * them (121 in tasks.c, 56 in queue.c, 14 in port.c, ...), e.g.
 *
 *     configASSERT( puxStackBuffer != NULL );      (tasks.c, xTaskCreateStatic)
 *
 * FreeRTOS decides WHERE the checks are; this macro decides WHAT HAPPENS
 * when one fails.
 *
 * If configASSERT is left undefined, FreeRTOS.h defines it as nothing and
 * the preprocessor removes every one of those checks — including the ones
 * that guard the INTERRUPT PRIORITIES section above:
 *   - configMAX_SYSCALL_INTERRUPT_PRIORITY has bits in unimplemented
 *     positions, i.e. the "plain 5 instead of 0x50" bug
 *     (port.c, xPortStartScheduler);
 *   - an ISR more urgent than the line calls a ...FromISR() function, e.g.
 *     because NVIC_SetPriority() was forgotten and it is still at the reset
 *     level 0 (port.c, vPortValidateInterruptPriority). Without the assert
 *     this only fails when the interrupt lands inside a critical section —
 *     once in a million, at random. With it, it fails on the FIRST call;
 *   - the NVIC priority grouping was changed (same function).
 * Also enabled by it: configCHECK_HANDLER_INSTALLATION (defaults to 1),
 * which verifies at scheduler start that the vector table really contains
 * the port's SVC and PendSV handlers (see the handler-mapping section).
 *
 * What this definition does when a check fails:
 *   1. portDISABLE_INTERRUPTS() raises BASEPRI to the line, so SysTick and
 *      PendSV can no longer run: no more ticks, no more task switches. The
 *      system is frozen in the state it had at the moment of the failure.
 *      (portDISABLE_INTERRUPTS comes from portmacro.h, which every kernel
 *      file gets through FreeRTOS.h; taskDISABLE_INTERRUPTS() in task.h is
 *      the same thing, but task.h is not included everywhere.)
 *   2. for( ;; ); spins forever.
 *
 * How to use it: if the board appears to hang, press Stop in the Keil
 * debugger. If execution is inside this loop, the Call Stack window shows
 * the function and the exact configASSERT() line that failed.
 *
 * Alternatives considered:
 *   - Calling a function with __FILE__ / __LINE__ so the location can be
 *     printed: useful without a debugger, but there is no UART output yet.
 *     Upgrading later means changing only this macro.
 *   - A BKPT instruction: halts an attached debugger automatically, but
 *     with no debugger attached it escalates to a HardFault, disguising the
 *     assert as a different kind of crash.
 *
 * Production note: a shipped product must not hang forever. There, the
 * handler would record the failure and reset the device (typically via a
 * watchdog). For a development board with a debugger attached, freezing is
 * exactly what i want.
 */
#define configASSERT( x )    if( ( x ) == 0 ) { portDISABLE_INTERRUPTS(); for( ;; ); }

/* ==========================================================================
 * HANDLER MAPPING (port handlers -> vector table names)
 * ==========================================================================
 *
 * Background — how the CPU finds exception code:
 *
 *   Normal function calls do not need any table: the compiler emits a BL
 *   instruction that contains the target's address (filled in by the
 *   linker). Exceptions and interrupts are different: nobody calls them,
 *   hardware triggers them at any moment. So the CPU uses the VECTOR TABLE,
 *   a list of addresses at the very start of flash (0x08000000), one entry
 *   (4 bytes) per exception source of the chip — 102 entries on the
 *   STM32F411: 16 system exceptions defined by ARM (entries 0..15) and 86
 *   peripheral interrupts defined by ST.
 *
 *   Each source has a fixed number N, wired in hardware. When source N
 *   fires, the CPU reads the address stored at (table start + 4 * N) and
 *   jumps to it. No names exist at run time — only addresses.
 *
 *   The three entries FreeRTOS depends on:
 *     entry 11  SVC      triggered on purpose by the "svc 0" instruction in
 *                        prvPortStartFirstTask() (port.c) to start the very
 *                        first task — a task can only be started by
 *                        RETURNING from an exception into its prepared
 *                        stack frame, so the port has to enter one first;
 *     entry 14  PendSV   the context switch;
 *     entry 15  SysTick  the tick.
 *
 * The problem these defines solve:
 *
 *   The table is written in startup_stm32f411xe.s using NAMES, e.g.
 *       DCD  SVC_Handler
 *   which tells the linker "write the address of the function called
 *   SVC_Handler here". The port's functions have different names
 *   (vPortSVCHandler, xPortPendSVHandler, xPortSysTickHandler), and to the
 *   linker those are unrelated.
 *
 *   The startup file also provides a placeholder for every name in the
 *   table — a function that only loops forever ("B ."), marked [WEAK]. Weak
 *   means "fallback": if the project contains a normal function with the
 *   same name, the linker keeps that one and discards the placeholder
 *   (decided once, at link time — not at run time).
 *
 *   Without this section the build would still SUCCEED — the linker would
 *   silently use the placeholders — and then "svc 0" would jump into an
 *   endless loop and no task would ever run. (With configASSERT defined, the
 *   port's handler check in xPortStartScheduler() stops first, on a line
 *   that names the cause — configCHECK_HANDLER_INSTALLATION, default 1.)
 *
 * How the defines work ("direct routing", the method recommended in port.c):
 *
 *   #define A B replaces the text A with B before compiling. port.c includes
 *   this header, so its function
 *       void xPortSysTickHandler( void ) { ... }
 *   is compiled as
 *       void SysTick_Handler( void ) { ... }
 *   The project then contains a normal function with exactly the name the
 *   table asks for; the linker discards the weak placeholder and writes the
 *   address of the port's code into the table. No extra code, no run-time
 *   cost, and no vendor file is modified.
 *
 * Alternatives considered:
 *   - Writing my own SVC_Handler / PendSV_Handler / SysTick_Handler that
 *     jump to the port's functions ("indirect routing"): extra instructions
 *     on every tick and context switch; the SVC and PendSV wrappers would
 *     have to be hand-written assembly (the port's code expects the stack
 *     exactly as the hardware left it, which a normal C function would
 *     disturb); and the handler check would have to be turned off. Only
 *     worth it if my own code had to run inside SysTick_Handler.
 *   - Editing the vector table in the startup file: modifies a file managed
 *     by the Keil device pack (under RTE/), which a pack update can replace.
 *
 * Consequence: the port now OWNS these three names. Never define a function
 * called SVC_Handler, PendSV_Handler or SysTick_Handler anywhere in the
 * project — the linker would report a duplicate definition. (For code that
 * must run on every tick, configUSE_TICK_HOOK exists.)
 */
#define vPortSVCHandler         SVC_Handler
#define xPortPendSVHandler      PendSV_Handler
#define xPortSysTickHandler     SysTick_Handler

/* ==========================================================================
 * OPTIONAL API FUNCTIONS (INCLUDE_ settings)
 * ==========================================================================
 *
 * Some kernel API functions are compiled only if the matching INCLUDE_xxx
 * macro is 1: in tasks.c each of them is wrapped in
 *     #if ( INCLUDE_xxx == 1 ) ... #endif
 * FreeRTOS.h defaults most of these to 0, so by default the function's code
 * is not compiled at all (this keeps unused functions out of flash on
 * toolchains that cannot remove them automatically).
 *
 * What happens if a function is used while its INCLUDE_ is 0: task.h still
 * DECLARES the function (the prototype is unconditional), so the call
 * compiles — but the function body does not exist, and the build fails at
 * the LINK step with an "undefined symbol" error. It is a build-time
 * failure, never a run-time one.
 *
 * Only the functions the design needs are enabled. Everything else
 * (vTaskDelete, vTaskSuspend, vTaskPrioritySet, ...) stays at its default
 * of 0 until something actually uses it.
 *
 * (INCLUDE_uxTaskGetStackHighWaterMark is set in the TASKS AND STACKS
 * section above, next to the stack-sizing settings it serves.)
 */

/*
 * INCLUDE_vTaskDelay = 1
 *
 * Compiles vTaskDelay( ticks ): "block me for this many ticks, counted FROM
 * NOW". The task is moved to the Delayed list and the CPU is given away
 * immediately; the tick handler moves it back to the Ready list when
 * xTickCount reaches (now + ticks).
 *
 * Used for simple relative delays and for the first scheduler test (a task
 * toggling a GPIO every pdMS_TO_TICKS( 500 )).
 */
#define INCLUDE_vTaskDelay                          1

/*
 * INCLUDE_xTaskDelayUntil = 1
 *
 * Compiles xTaskDelayUntil( &xLastWakeTime, period ) — and with it
 * vTaskDelayUntil(), which task.h defines as a macro around it that ignores
 * the return value.
 *
 * Difference from vTaskDelay(), and why periodic tasks need this one:
 *   - vTaskDelay( 1000 ) waits 1000 ticks from the moment it is CALLED. The
 *     time the task spent working (e.g. reading the sensor over I2C) is
 *     added on top, so each cycle is "work time + 1000 ticks" and the
 *     schedule drifts.
 *   - xTaskDelayUntil( &xLastWakeTime, 1000 ) wakes the task at
 *     xLastWakeTime + 1000, i.e. at fixed points in time, regardless of how
 *     long the work took. The period is exact and does not drift.
 *
 * It returns pdFALSE if the wake-up time had already passed when it was
 * called (the task overran its period and was not delayed at all) — a
 * direct way to detect a missed deadline.
 *
 * This is the function the periodic sensor task will use, and the reason
 * software timers could be left off (configUSE_TIMERS = 0).
 */
#define INCLUDE_xTaskDelayUntil                     1

/*
 * INCLUDE_xTaskGetSchedulerState = 1
 *
 * Compiles xTaskGetSchedulerState(), which tells whether the scheduler has not
 * been started yet, is running, or is suspended. The I2C driver uses it in an
 * assertion: its waits are bounded by the RTOS tick, which does not advance before
 * vTaskStartScheduler(), so calling the driver from main() would not time out but
 * could hang. The assertion turns that mistake into an immediate, visible stop.
 */
#define INCLUDE_xTaskGetSchedulerState              1

#endif /* FREERTOS_CONFIG_H */
