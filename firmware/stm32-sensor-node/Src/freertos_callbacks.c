/*
 * freertos_callbacks.c — functions that the FreeRTOS kernel CALLS, but that
 * the application must provide.
 *
 * Normally application code calls the kernel. These two are the opposite:
 * the kernel needs something only the application can supply, so it calls a
 * function with a fixed name and signature, and the linker connects the two.
 * They are required by choices recorded in FreeRTOSConfig.h:
 *
 *   vApplicationGetIdleTaskMemory()  <- configSUPPORT_STATIC_ALLOCATION 1 and
 *                                       configKERNEL_PROVIDED_STATIC_MEMORY 0
 *                                       (ADR-0001)
 *   vApplicationStackOverflowHook()  <- configCHECK_FOR_STACK_OVERFLOW 2
 *
 * Without them the project builds but FAILS AT THE LINK STEP with
 * "undefined symbol" errors. That is expected until this file is part of the
 * Keil project (group "Application").
 *
 * Kept in its own file so main.c only contains application logic.
 */

#include "FreeRTOS.h"
#include "task.h"

/* ==========================================================================
 * IDLE TASK MEMORY
 * ==========================================================================
 *
 * Why the kernel asks for this:
 *   For tasks WE create, we pass memory to xTaskCreateStatic() ourselves.
 *   The Idle task is different: the kernel creates it by itself, inside
 *   vTaskStartScheduler(), so that there is always at least one task able to
 *   run. With static allocation the kernel has nowhere to get that task's
 *   memory from, so it calls this function and asks us for it:
 *       - memory for the TCB   (Task Control Block: the kernel's record of
 *                               the task — saved stack pointer, priority,
 *                               list links, name);
 *       - memory for the stack (the task's private working area, where its
 *                               registers are saved when it is switched out).
 *
 * This function does NOT allocate anything. It only hands the kernel the
 * ADDRESSES of two variables that already exist for the whole life of the
 * program. They are file-scope `static` for two reasons:
 *   - lifetime: a `static` variable lives in .bss for the whole program, at
 *     an address fixed by the linker. A local (non-static) array would live
 *     on the main stack, which the scheduler resets when it starts
 *     (prvPortStartFirstTask in port.c), so it would be overwritten by
 *     interrupt handlers while Idle is using it as its stack;
 *   - the RAM is accounted for at link time: both arrays show up by name in
 *     the .map file (look for xIdleTaskTCB and uxIdleTaskStack).
 *
 * Stack size: configMINIMAL_STACK_SIZE is in WORDS (StackType_t is
 * uint32_t), so 128 words = 512 bytes. The same macro is used for the array
 * size and for the size reported to the kernel, so the two can never
 * disagree. The value was checked by measurement, see ADR-0005.
 *
 * Alignment: the ARM calling convention wants an 8-byte aligned stack, but a
 * uint32_t array is only guaranteed 4-byte aligned. No attribute is needed:
 * the kernel rounds the TOP of the stack down to a multiple of 8 itself
 * (tasks.c, prvInitialiseNewTask), losing at most 4 bytes of the array.
 */
static StaticTask_t xIdleTaskTCB;
static StackType_t  uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t  ** ppxIdleTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * puxIdleTaskStackSize )
{
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *puxIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

/* ==========================================================================
 * STACK OVERFLOW HOOK
 * ==========================================================================
 *
 * When it is called:
 *   With configCHECK_FOR_STACK_OVERFLOW = 2, the kernel checks the running
 *   task's stack at EVERY CONTEXT SWITCH (inside vTaskSwitchContext(), which
 *   the PendSV handler calls): is the saved stack pointer outside the stack
 *   array, or have the last 16 bytes of the array lost the 0xA5 fill pattern
 *   the kernel wrote at task creation? If so, it calls this function. So this
 *   code always runs inside the PendSV exception handler, on the main stack.
 *
 * What it does: freezes the system. Halting is deliberate:
 *   An overflow writes downward past the start of the task's stack array,
 *   into whatever the linker placed below it — possibly another task's TCB,
 *   a queue or a global. By the time this hook runs, that memory may
 *   already be corrupted, so trying to carry on (or "recover") would run on
 *   damaged kernel data. Stopping at the moment of detection keeps the
 *   evidence for the debugger. This is the same policy as configASSERT.
 *
 * How the freeze works:
 *   portDISABLE_INTERRUPTS() raises BASEPRI to the line
 *   (configMAX_SYSCALL_INTERRUPT_PRIORITY), and the loop never exits.
 *   Honest note: we are already inside PendSV, whose level (15) is the least
 *   urgent, and PendSV had already raised BASEPRI before calling the kernel,
 *   so SysTick and every ISR at or below the line are already held back
 *   here. The call is kept so the hook stays correct if it is ever reached
 *   another way. Interrupts MORE urgent than the line (levels 0-4) are not
 *   masked by BASEPRI and could still run; none are used in this project.
 *
 * Finding the culprit in Keil:
 *   Press Stop. If the CPU is in this loop, the task that overflowed is the
 *   one given to this function: R0 = its handle (the address of its TCB) and
 *   R1 = a pointer to its name string, as they were on entry. Look at R1 in
 *   the Memory window. (The Watch window may show the parameters as
 *   unavailable, because they are unused after the casts below and the
 *   compiler is free to drop them.)
 *
 * Production note: a shipped device would log the failure and reset (usually
 * through a watchdog) instead of hanging. For a development board with a
 * debugger attached, freezing is exactly what we want.
 */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;
    ( void ) pcTaskName;

    portDISABLE_INTERRUPTS();

    for( ;; )
    {
    }
}
