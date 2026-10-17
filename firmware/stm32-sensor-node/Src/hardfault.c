/*
 * hardfault.c — what happens when the CPU takes a HardFault.
 *
 * WHY THIS FILE EXISTS. The start-up file provides a HardFault_Handler that is only
 * `B .` (a branch to itself) and is marked weak. With it, a bad pointer, a stack that
 * overflowed into unmapped memory, or an invalid instruction leaves the CPU spinning
 * with no clue about where. This handler replaces it (a strong definition wins over the
 * weak one) and does the one thing worth doing in a system without a debugger attached:
 * it works out which stack the faulting code was using, copies the registers that the
 * CPU pushed when it took the fault, plus the fault status registers, into a static
 * record, and stops. The record is read in the debugger afterwards (its address is in
 * the map file).
 *
 * WHAT THE CPU DOES ON A FAULT. On exception entry the CPU pushes eight words onto the
 * stack that was in use: R0, R1, R2, R3, R12, LR, PC and xPSR, in this order from the
 * lowest address. The pushed PC is the address of the instruction that faulted (for a
 * precise fault), which is the single most useful number. Tasks run on the process stack
 * (PSP) and interrupts and the start-up code on the main stack (MSP); bit 2 of the
 * EXC_RETURN value that the CPU puts in LR on entry says which one was in use.
 *
 * WHY THE FIRST PART IS ASSEMBLER. A C function would start by building its own stack
 * frame, so by the time it ran, the stack pointer would no longer point at the pushed
 * registers and LR would be overwritten. The naked function below does only what must be
 * done before any C code: pick MSP or PSP according to EXC_RETURN bit 2, pass that
 * pointer in R0 and EXC_RETURN in R1, and branch to a normal C function. (The same
 * technique as the context switch in FreeRTOS's port.c.)
 *
 * WHICH FAULT IT WAS. The BusFault, MemManage and UsageFault handlers are not enabled
 * (the enable bits in SCB->SHCSR are 0 after reset), so every one of them is escalated
 * to a HardFault. The kind is still in the status registers, which are recorded:
 *   CFSR  (0xE000ED28)  three groups of bits: MMFSR (bits 7:0, memory management),
 *                       BFSR (15:8, bus), UFSR (31:16, usage: for instance bit 16 =
 *                       undefined instruction, bit 25 = divide by zero if enabled)
 *   HFSR  (0xE000ED2C)  bit 30 FORCED = a configurable fault was escalated to HardFault
 *   MMFAR (0xE000ED34)  the address of a memory-management fault, valid if CFSR bit 7
 *   BFAR  (0xE000ED38)  the address of a bus fault, valid if CFSR bit 15
 *
 * LIMITS. It does not recover and does not restart: it stops, with the record filled in
 * and interrupts of lower priority unable to run (a HardFault is more urgent than any
 * of them). It cannot help if the stack pointer itself is bad (a stack overflow so deep
 * that the pushed frame is in unmapped memory makes the CPU take another fault inside the
 * fault, and it locks up). It calls no FreeRTOS function and uses no heap.
 *
 * HOW TO TEST IT ON THE BOARD: put a read of an unmapped address, for instance
 *     ( void ) *( volatile uint32_t * ) 0x30000000U;
 * in main() before the scheduler starts (the fault is then on the MSP), and again inside
 * a task (the fault is then on the PSP). Remove it afterwards. Then read xFaultRecord:
 * the PC is the address of that load, CFSR has the PRECISERR bit (bit 9) and BFARVALID
 * (bit 15), BFAR is 0x30000000, HFSR has FORCED (bit 30).
 */

#include "stm32f4xx.h"

typedef struct
{
    /* Pushed by the CPU on exception entry. */
    uint32_t ulR0;
    uint32_t ulR1;
    uint32_t ulR2;
    uint32_t ulR3;
    uint32_t ulR12;
    uint32_t ulLR;
    uint32_t ulPC;     /* the address of the faulting instruction */
    uint32_t ulPSR;

    uint32_t ulStackPointer;    /* where that frame was: the value of MSP or PSP */
    uint32_t ulExcReturn;       /* LR on entry; bit 2 = 1 means the frame is on the PSP (a task) */

    /* Fault status registers (see the top of the file). */
    uint32_t ulCFSR;
    uint32_t ulHFSR;
    uint32_t ulMMFAR;
    uint32_t ulBFAR;
} fault_record_t;

/* Kept where the debugger can read it. volatile so that the compiler does not remove
 * stores to a variable that nothing reads. */
static volatile fault_record_t xFaultRecord;

/* Called from the assembler below, so it cannot be static. */
void HardFault_HandlerC( uint32_t * pulFrame, uint32_t ulExcReturn );

void HardFault_HandlerC( uint32_t * pulFrame, uint32_t ulExcReturn )
{
    xFaultRecord.ulR0 = pulFrame[ 0 ];
    xFaultRecord.ulR1 = pulFrame[ 1 ];
    xFaultRecord.ulR2 = pulFrame[ 2 ];
    xFaultRecord.ulR3 = pulFrame[ 3 ];
    xFaultRecord.ulR12 = pulFrame[ 4 ];
    xFaultRecord.ulLR = pulFrame[ 5 ];
    xFaultRecord.ulPC = pulFrame[ 6 ];
    xFaultRecord.ulPSR = pulFrame[ 7 ];

    xFaultRecord.ulStackPointer = ( uint32_t ) pulFrame;
    xFaultRecord.ulExcReturn = ulExcReturn;

    xFaultRecord.ulCFSR = SCB->CFSR;
    xFaultRecord.ulHFSR = SCB->HFSR;
    xFaultRecord.ulMMFAR = SCB->MMFAR;
    xFaultRecord.ulBFAR = SCB->BFAR;

    /* Stop here, where the debugger can show the record. */
    for( ;; )
    {
    }
}

/* The name is fixed by the vector table in the start-up file. */
__attribute__( ( naked ) ) void HardFault_Handler( void )
{
    __asm volatile
    (
        " tst lr, #4                \n" /* bit 2 of EXC_RETURN: which stack was in use */
        " ite eq                    \n"
        " mrseq r0, msp             \n" /* 0: the main stack */
        " mrsne r0, psp             \n" /* 1: the process stack (a task) */
        " mov r1, lr                \n" /* EXC_RETURN, for the record */
        " b HardFault_HandlerC      \n"
    );
}
