/*
 * uart.c — USART2 driver (TX only for now).
 *
 * The USART2 peripheral shares the ST-LINK's virtual COM port: PA2 (TX) goes
 * to the ST-LINK chip, which forwards it over the USB cable to the PC, where
 * Tera Term shows it. (On the Nucleo-64 the connection PA2/PA3 <-> ST-LINK is
 * made by solder bridges, closed by default as far as I know. If no text ever
 * appears, check that first.)
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) USART2 clock + PA2 as alternate function 7                   <- DONE
 *   2) baud rate (USART2->BRR) and enable (CR1: UE, TE)             <- DONE
 *   3) send one byte (wait for TXE, write DR)                       <- DONE
 *   4) interrupt-driven send with a ring buffer                      <- DONE
 *        4a) the buffer, the TX interrupt
 *        4b) whole-message write with a drop policy and a drop counter
 *   5) a small decimal formatter, no printf and no malloc           <- DONE
 * 
 */

#include "FreeRTOS.h"
#include "task.h"
#include "uart.h"
#include "stm32f4xx.h"

/* ==========================================================================
 * STEP 1) CLOCKS AND THE TX PIN
 * ==========================================================================
 *
 * Two clocks must be on before any register of these peripherals can be
 * written (writes while a peripheral has no clock are silently ignored):
 *
 *   - GPIOA, on the AHB1 bus: RCC->AHB1ENR bit GPIOAEN. 
 *   - USART2, on the APB1 bus: RCC->APB1ENR bit USART2EN (bit 17).
 *
 * Each enable is followed by a dummy read of the same register. The clock
 * takes a moment to reach the peripheral, and an access straight after the
 * enable can be ignored (an STM32F4 erratum; ST's own code does the same
 * read-back). The (void) keeps the compiler quiet; the read happens because
 * the register is volatile.
 *
 * PA2 as USART2_TX. A pin can be wired to different internal functions through
 * a multiplexer, selected in two steps:
 *
 *   MODER (2 bits per pin): 00 input, 01 output, 10 ALTERNATE FUNCTION,
 *                           11 analog. PA2 uses bits 5:4 and must become 10:
 *                           the pin is then driven by a peripheral instead of
 *                           by the ODR/BSRR registers.
 *   AFR   (4 bits per pin): WHICH peripheral function. AFR[0] covers pins 0-7,
 *                           and PA2 uses bits 11:8. The table of alternate
 *                           functions in the datasheet lists PA2 -> AF7 =
 *                           USART2_TX.
 *
 * AFR is written BEFORE MODER. The reset value of AFR is 0 (AF0, "system
 * function"), so switching MODER first would connect the pin to the wrong
 * function for a moment, with a possible glitch on the line.
 *
 * Both registers are changed with read-modify-write, not assigned. As with any configuration
 * register, pin set-up belongs in one-time initialisation, not in running
 * tasks.
 *
 * What is left at reset values: output type push-pull, speed low, no pull-up/down. That is fine for
 * 115200 baud: the edges are slow. No pull-up is needed on PA2: it is a
 * push-pull OUTPUT, so once the USART is enabled the pin itself drives the idle
 * level (high) as well as the low level. Pull resistors matter for inputs and
 * for open-drain outputs (such as I2C), not here. (Between this step and the
 * USART being enabled in step 2 the level of the pin is not guaranteed by
 * anything I have checked, so a stray character may reach the terminal at
 * start-up; it would be harmless.)
 *
 * Expected values afterwards (with the LED setup from main.c already done,
 * which is the case when the scheduler is running):
 *   RCC->APB1ENR  (0x40023840) bit 17 set        -> 0x00020000
 *   GPIOA->AFR[0] (0x40020020) bits 11:8 = 7     -> 0x00000700
 *   GPIOA->MODER  (0x40020000)                   -> 0xA8000420
 *     reset 0xA8000000 (PA13-PA15 as alternate function for SWD),
 *     + PA5 as output (01 at bits 11:10 = 0x400), + PA2 as AF (10 at bits 5:4 = 0x20).
 */
#define UART_TX_ALTERNATE_FUNCTION    ( 7U )    /* AF7 = USART1..3 on PA2 (datasheet table) */

static void prvUartPinsAndClocks( void )
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    ( void ) RCC->AHB1ENR;

    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    ( void ) RCC->APB1ENR;

    GPIOA->AFR[ 0 ] = ( GPIOA->AFR[ 0 ] & ~GPIO_AFRL_AFSEL2 ) |
                      ( UART_TX_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL2_Pos );

    GPIOA->MODER = ( GPIOA->MODER & ~GPIO_MODER_MODER2 ) | GPIO_MODER_MODER2_1;
}

/* ==========================================================================
 * STEP 2) FRAME FORMAT, BAUD RATE AND ENABLE
 * ==========================================================================
 *
 * What goes on the wire for each byte (8N1): the line idles high; a START bit
 * (low) announces the byte; 8 data bits follow, least significant first; a
 * STOP bit (high) ends it. That is 10 bits per byte. The receiver has no clock
 * wire: it synchronises on the start bit's falling edge and samples each bit in
 * the middle of its time, so both sides must agree on the bit duration (the
 * baud rate) closely enough that the error does not add up to half a bit over
 * the 10 bits of a frame.
 *
 * Baud rate = bits per second ON THE LINE, start and stop bits included. At
 * 115200 one bit lasts 8.68 microseconds, a byte (10 bits) 86.8 microseconds,
 * so at most about 11,520 bytes per second.
 *
 * USART2->BRR holds the divisor of the peripheral's input clock (PCLK1, the
 * APB1 clock). With the default oversampling by 16, the manual defines
 *     USARTDIV = PCLK1 / (16 * baud)
 * and BRR stores USARTDIV as a fixed-point number: mantissa in bits 15:4,
 * fraction (in sixteenths) in bits 3:0. That means the whole register is just
 * USARTDIV * 16 = PCLK1 / baud, so no mantissa/fraction juggling is needed:
 *     BRR = round( PCLK1 / baud )
 * For PCLK1 = 42 MHz: 42,000,000 / 115,200 = 364.58, rounded to 365 = 0x16D
 * (mantissa 22, fraction 13). The real rate is then 42,000,000 / 365 = 115,068
 * baud: -0.11 % error, far inside what the receiver tolerates.
 *
 * PCLK1 is NOT SystemCoreClock. SystemCoreClock is HCLK (the CPU clock), and
 * USART2 sits on APB1, whose prescaler (set in clock.c) divides it again. It is
 * computed here from the real registers: HCLK >> (the prescaler's shift from
 * the standard CMSIS table, indexed by the PPRE1 field of RCC->CFGR). A useful
 * consequence: if the PLL had failed and the CPU were still on the 16 MHz HSI,
 * PCLK1 would be 8 MHz and the divisor 69 (a 0.64 % error), so the output would
 * stay readable at 115200 and could even report the failure. This means
 * uart_init() must run AFTER SystemCoreClockUpdate().
 *
 * The three control registers are ASSIGNED here, not read-modify-written. That
 * is deliberate and differs from the shared GPIO/RCC registers above: nothing
 * else uses USART2, it has only just been clocked, and writing every field
 * states the configuration explicitly instead of trusting reset values:
 *   CR1 = 0  8 data bits (M = 0), no parity (PCE = 0), oversampling by 16
 *            (OVER8 = 0), everything disabled
 *   CR2 = 0  1 stop bit
 *   CR3 = 0  no flow control, no DMA, no error interrupts
 * Then, following the order in the reference manual's transmitter procedure:
 * the baud rate is written while the USART is still disabled, the transmitter
 * enable (TE) is set, and finally the USART enable (UE). Setting TE makes the
 * peripheral send an idle frame as its first transmission
 * the receiver side is not enabled (no RE), since this driver
 * only sends.
 *
 * Nothing is sent yet: that is step 3.
 *
 * Expected values afterwards (USART2 base 0x40004400):
 *   USART2->BRR  (0x40004408) = 0x0000016D
 *   USART2->CR1  (0x4000440C) = 0x00002008   UE (bit 13) + TE (bit 3)
 *   USART2->SR   (0x40004400) = 0x000000C0   TXE (bit 7) and TC (bit 6) set:
 *                                            the data register is empty and
 *                                            nothing is being transmitted
 */
#define UART_BAUD_RATE    ( 115200U )

static void prvUartConfigure( void )
{
    uint32_t ulPclk1Hz;

    ulPclk1Hz = SystemCoreClock >> APBPrescTable[ ( RCC->CFGR & RCC_CFGR_PPRE1 ) >> RCC_CFGR_PPRE1_Pos ];

    USART2->CR1 = 0U;
    USART2->CR2 = 0U;
    USART2->CR3 = 0U;

    USART2->BRR = ( ulPclk1Hz + ( UART_BAUD_RATE / 2U ) ) / UART_BAUD_RATE;

    USART2->CR1 = USART_CR1_TE;
    USART2->CR1 |= USART_CR1_UE;
}

/* ==========================================================================
 * STEP 4a) INTERRUPT-DRIVEN SEND THROUGH A RING BUFFER
 * ==========================================================================
 *
 * How a byte leaves the chip:
 *
 *   your code -> USART2->DR (data register) -> shift register -> PA2, bit by bit
 *
 * Writing a byte to DR hands it to the hardware, which moves it into an
 * internal shift register and clocks it out with the start and stop bits added.
 * As soon as the byte has moved into the shift register, DR is free again and
 * the status flag TXE ("transmit data register empty", USART2->SR bit 7) goes
 * high: the NEXT byte may be written while the previous one is still being
 * sent. Writing DR clears TXE. (The other flag, TC, "transmission complete",
 * bit 6, rises only after the very last bit has left the wire; it matters when
 * something must wait until everything has really been sent.) A byte takes 86.8
 * microseconds on the wire at 115200 baud.
 *
 * Polling TXE in a loop would keep the CPU busy for those 86.8 microseconds per
 * byte. Instead, tasks drop their bytes into a BUFFER and return at once, and
 * the USART tells the CPU, with an interrupt, each time it can take the next
 * byte:
 *
 *   task:   uart_write() -> copy the message into the buffer, enable the TXE interrupt
 *   ISR:    TXE interrupt -> take the next byte from the buffer, write it to DR;
 *           when the buffer is empty, disable the TXE interrupt
 *
 * The RING BUFFER is an array with two counters:
 *   ulTxHead  how many bytes have been PUT in, in total (changed by tasks only)
 *   ulTxTail  how many bytes have been TAKEN out, in total (changed by the ISR only)
 * The counters are not wrapped: they simply keep counting (an unsigned 32-bit
 * counter wraps by itself, and 2^32 is a multiple of the buffer size, so the
 * arithmetic stays correct). The position inside the array is the counter AND
 * (size - 1), which is why the size must be a power of two (checked below). Then:
 *   bytes waiting  = head - tail          (right even across the wrap)
 *   empty          = head == tail
 *   full           = head - tail == size  (every slot is usable)
 *
 * SHARED STATE AND HOW IT IS PROTECTED (ADR-0004). The task side of the buffer
 * runs inside a critical section (taskENTER_CRITICAL: BASEPRI is raised to the
 * FreeRTOS line, level 5). Interrupts at level 5 or numerically higher are held
 * back for the few instructions of the section. There are three reasons, in
 * order of importance:
 *
 *   1) TASK AGAINST TASK (the strongest reason). Several tasks may call
 *      uart_write(), and a higher-priority task can preempt a lower one at any
 *      instruction. Without the section: task A reads ulTxHead, task B preempts it,
 *      reads the SAME ulTxHead, copies its message to the same slots and moves the
 *      head; when A resumes it overwrites B's bytes and moves the head from its old
 *      value, so one message is lost and the other is corrupted. Today only one
 *      task prints, but the planned SPI transmit task (priority 2) will be able to
 *      preempt the sensor task, so the protection is needed by design.
 *   2) CHECK AND COPY AS ONE UNIT. The free-space check and the copy must not be
 *      separated: if another task could take the space between them, two tasks
 *      would both pass the check and together overflow the buffer. This is also
 *      what makes the "whole message or nothing" rule hold.
 *   3) THE SHARED CR1 REGISTER. The task sets TXEIE in USART2->CR1 and the ISR
 *      clears it; each is a read-modify-write, and CR1 is a register that both
 *      sides change, so an interrupt could in principle split the task's update.
 *      My own analysis (not proven formally) is that this race is BENIGN here: the
 *      ISR only ever changes TXEIE, and a stale write can at worst leave TXEIE set
 *      once more than needed, which causes one extra interrupt that finds the
 *      buffer empty and turns it off again. The protection costs a few
 *      microseconds, so it is kept anyway: a shared register is easier to defend
 *      when its updates are never split.
 *
 * For the section to hold the ISR back, the USART2 interrupt MUST sit at level
 * >= 5: it is set to level 10 (checked at build time), deliberately less urgent
 * than the future SPI/DMA interrupt. The reverse case cannot happen: a task never
 * runs while an ISR is in progress. The ISR itself calls no FreeRTOS function.
 * The section also contains a compiler memory barrier, so the byte is really in
 * the array before the head counter moves.
 *
 * Two rules that come from using the FreeRTOS critical section:
 *   - taskENTER_CRITICAL() asserts if called from an interrupt (port.c), so
 *     uart_write() is for tasks only;
 *   - before the scheduler starts, the port's nesting counter holds a dummy
 *     value (0xAAAAAAAA), so a section entered then would never lower BASEPRI
 *     again until the scheduler starts. Call it from tasks, after the start.
 *
 * Why the ISR must turn TXEIE off when the buffer is empty: TXE stays 1 as long
 * as nothing is being sent, so with TXEIE still set the interrupt would fire
 * again immediately, forever, and the CPU would never leave the handler.
 *
 * The handler's name is fixed by the vector table in the startup file
 * (USART2_IRQHandler, a [WEAK] placeholder there): defining a function with that
 * exact name makes the linker use ours. Its priority is set with
 * NVIC_SetPriority(), which takes the plain level (the CMSIS function does the
 * shift into the top bits that BASEPRI's encoding needs).
 */
#define UART_TX_BUFFER_SIZE    ( 256U )
#define UART_IRQ_PRIORITY      ( 10U )

_Static_assert( ( UART_TX_BUFFER_SIZE & ( UART_TX_BUFFER_SIZE - 1U ) ) == 0U,
                "the TX buffer size must be a power of two" );
_Static_assert( UART_IRQ_PRIORITY >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
                "the USART2 interrupt must be at or below the FreeRTOS line (ADR-0004), "
                "or the critical section will not protect the shared state" );
_Static_assert( UART_IRQ_PRIORITY < ( 1U << __NVIC_PRIO_BITS ),
                "interrupt level out of range for this chip" );

static uint8_t ucTxBuffer[ UART_TX_BUFFER_SIZE ];
static volatile uint32_t ulTxHead;    /* total bytes put in:   changed by tasks, inside the critical section */
static volatile uint32_t ulTxTail;    /* total bytes taken out: changed by the ISR only */

/*
 * STEP 4b) WHOLE MESSAGES AND THE DROP POLICY
 * -------------------------------------------
 *
 * What if the buffer is full? Under the planned load it practically never is, but a burst of
 * messages written back to back (a start-up banner, an error printed in a loop)
 * can write much faster than the line sends (11.5 KB/s). The options were: wait
 * for room (a busy-wait again, in the task that should be doing the real work),
 * block on a semaphore (more RTOS machinery), or DROP. Dropping never delays the
 * caller, which is the whole point of the buffer.
 *
 * It drops the WHOLE message, never part of it: a line cut in the middle would
 * be unreadable and look like a bug. So the free space is checked first, for the
 * full length, and only then is anything copied. The check and the copy sit in
 * ONE critical section, so no other task can take the space in between, and the
 * ISR (which only frees space) cannot make the check wrong in a dangerous
 * direction. Every drop is counted (ulTxDroppedMessages), so the loss is visible
 * instead of silent, and uart_dropped_messages() makes the number available to be
 * printed later.
 *
 * The section lasts as long as the copy loop. The compiled loop is about 6 to 7
 * instructions per byte, so roughly 10 cycles per byte once loads and stores are
 * counted. During it the interrupts at level 5 and above numerically, SysTick included, are held back;
 * a tick would be delayed by that much. That is acceptable for debug messages of
 * this size. The longest possible message is the buffer size (256 bytes, about
 * 30 microseconds).
 *
 * Free space = size - (head - tail). The unwrapped counters make this exact even
 * after they overflow: unsigned subtraction wraps correctly. Inside the critical
 * section the ISR cannot run, so tail cannot move during the calculation.
 */
static volatile uint32_t ulTxDroppedMessages;

uart_status_t uart_write( const uint8_t * pucData, uint32_t ulLength )
{
    uart_status_t xStatus = UART_OK;
    uint32_t ulIndex;
    uint32_t ulHead;

    taskENTER_CRITICAL();
    {
        ulHead = ulTxHead;    /* read once: ulTxHead is volatile, and inside the loop it would be re-read for every byte */

        if( ulLength > ( UART_TX_BUFFER_SIZE - ( ulHead - ulTxTail ) ) )
        {
            ulTxDroppedMessages++;
            xStatus = UART_ERR_FULL;
        }
        else if( ulLength > 0U )
        {
            for( ulIndex = 0U; ulIndex < ulLength; ulIndex++ )
            {
                ucTxBuffer[ ( ulHead + ulIndex ) & ( UART_TX_BUFFER_SIZE - 1U ) ] = pucData[ ulIndex ];
            }

            ulTxHead = ulHead + ulLength;
            USART2->CR1 |= USART_CR1_TXEIE;
        }
        else
        {
            /* Zero length: nothing to do. */
        }
    }
    taskEXIT_CRITICAL();

    return xStatus;
}

uint32_t uart_dropped_messages( void )
{
    return ulTxDroppedMessages;
}

void USART2_IRQHandler( void )
{
    /* Only the TX interrupt is enabled, so TXE is expected to be set. It is
     * checked anyway: writing DR when the register is not empty would overwrite
     * a byte that is still waiting to be moved to the shift register. */
    if( ( USART2->SR & USART_SR_TXE ) != 0U )
    {
        if( ulTxTail != ulTxHead )
        {
            USART2->DR = ucTxBuffer[ ulTxTail & ( UART_TX_BUFFER_SIZE - 1U ) ];
            ulTxTail++;
        }
        else
        {
            USART2->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}

/* ==========================================================================
 * PUBLIC FUNCTION
 * ========================================================================== */
void uart_init( void )
{
    /* Steps 1) and 2): clocks, pin, baud rate, transmitter on. The TX interrupt
     * itself stays off (TXEIE = 0) until the first byte is queued. */
    prvUartPinsAndClocks();
    prvUartConfigure();

    /* Step 4a): the interrupt. Priority first, then enable, so it can never run
     * at the reset priority 0 (above the FreeRTOS line) even for an instant. */
    NVIC_SetPriority( USART2_IRQn, UART_IRQ_PRIORITY );
    NVIC_EnableIRQ( USART2_IRQn );
}
