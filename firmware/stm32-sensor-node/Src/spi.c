/*
 * spi.c — SPI1 as a slave, towards the Raspberry Pi (ADR-0010).
 *
 * Steps, in the order they were built and checked on the board:
 *
 *   1) clock, pins, SPI1 configured as a slave, left disabled
 *   2) DMA2 stream 3 feeding SPI1->DR, armed once, SPI enabled
 *   3) re-arming after every transaction (EXTI on NSS + the transmit task)
 *        3a) the interrupt on NSS rising, counting
 *        3b) the handler wakes the transmit task
 *        3c) spi_load_frame(), and the task's real work
 *
 * WHY A SLAVE. The Pi's Linux SPI driver (Phase 4) sits behind a master controller, so
 * the Pi clocks and the STM32 answers (ADR-0010, decision 1). In slave mode the clock
 * comes from the master, and the baud-rate bits BR[2:0] of CR1 do not affect the
 * transfer rate (RM0383 p. 555). The 6 MHz limit of the project is therefore enforced
 * on the Pi's side, not here.
 *
 * On the wire (logic analyzer, 2026-10-20, ADR-0010): NSS is low during exactly 112 SCK
 * clocks, MISO is sampled on the rising edge (SPI mode 0), and the frame decodes with a
 * valid CRC. The Pi's clock measured 1.000, 2.000, 3.997 and 5.88 MHz for 1, 2, 4 and
 * 6 MHz requested, so the limit holds. Between back-to-back reads from Python NSS stays
 * high for 154 us at least; 30000 such reads were all good.
 */

#include "FreeRTOS.h"
#include "spi.h"
#include "frame.h"
#include "transmit_task.h"
#include "stm32f4xx.h"

/* ==========================================================================
 * STEP 1) CLOCK, PINS AND THE SPI CONFIGURATION
 * ==========================================================================
 *
 * Clocks. SPI1 sits on the APB2 bus: RCC->APB2ENR bit SPI1EN (bit 12, RM0383). The
 * GPIOA clock is already on (uart_init() enabled it), but this module does not rely on
 * that: it enables it itself, as the I2C driver does for GPIOB. Each enable is followed
 * by a dummy read of the same register (an STM32F4 erratum: an access straight after
 * the enable can be ignored), as in the other drivers.
 *
 * No peripheral reset (RCC_APB2RSTR) is done: SPI1 has just been clocked for the first
 * time since reset, so it is in its reset state.
 *
 * THE FOUR PINS (all on port A, alternate function 5 = SPI1:
 *
 *   PA4 NSS   input  PULL-UP. NSS is active low, so "high" means "not selected". With
 *                    the Pi disconnected a floating NSS could read as selected at
 *                    random; the pull-up makes it a defined "not selected".
 *   PA5 SCK   input  PULL-DOWN. The manual (p. 554) says the idle state of SCK must
 *                    match the polarity selected in CR1: pulling it down if CPOL = 0.
 *                    (PA5 is also wired to the Nucleo's user LED LD2, 
 *                    once SCK is driven by the Pi the LED will flicker,
 *                    which is harmless.)
 *   PA6 MISO  OUTPUT push-pull, no pull, FAST speed. The only pin we drive. The speed
 *                    setting limits how fast the output edges can be. The low setting
 *                    is meant for slow signals (such as the 115200 baud UART), so for
 *                    a clock of several MHz a faster setting is needed; "fast" is
 *                    enough for 4 to 6 MHz, and radiates less than the "high" setting.
 *   PA7 MOSI  input  no pull. The master always drives it while we are selected.
 *
 * Output type and speed only matter for an OUTPUT, so they are set for PA6 only. The
 * pulls are set for all four so each pin's state is stated, not inherited.
 *
 * ORDER (same as the UART and I2C drivers): everything that describes the pin (output
 * type, pull, speed, and the function number in AFR) is written BEFORE MODER, and
 * MODER is written LAST, because MODER is what connects the pin to the peripheral.
 * Here the SPI's own registers (CR1, CR2) are written before MODER too, so that by the
 * time the pins are connected to SPI1 it is already configured (and disabled).
 *
 * All the GPIO registers are changed with read-modify-write, never assigned: they
 * also hold the settings of the other pins of port A (PA2 is the UART's TX, PA13 to
 * PA15 the debug interface).
 *
 * CR1 and CR2 of SPI1 are ASSIGNED, as the UART's control registers are: nothing else
 * uses SPI1, and writing every field states the whole configuration. Their reset
 * value is 0 for CR1, which is already what we want, but the intent is written out:
 *
 *   CR1 = 0x0000
 *     CPHA = 0, CPOL = 0   mode 0: SCK idles low, data sampled on the RISING edge and
 *                          changed on the falling edge (the Pi's spidev default too)
 *     MSTR = 0             SLAVE
 *     BR   = 000           irrelevant in slave mode (see above)
 *     SPE  = 0             DISABLED here; spi_start() enables it once the DMA is armed (see spi.h)
 *     LSBFIRST = 0         most significant bit first
 *     SSM = 0, SSI = 0     hardware NSS: the pin PA4 is the chip select
 *     RXONLY = 0, BIDIMODE = 0, BIDIOE = 0   ordinary two-wire full duplex
 *     DFF = 0              8-bit data frames
 *     CRCEN = 0            the CRC is done in software, in the frame (frame.c)
 *   CR2 = 0x0002
 *     TXDMAEN = 1          the SPI asks the DMA for the next byte every time its
 *                          transmit register empties (RM0383 p. 571)
 *     everything else 0    no interrupts (TXEIE, RXNEIE, ERRIE), SSOE = 0 (that one is
 *                          for masters), FRF = 0 (Motorola format, not TI)
 *
 * Expected values afterwards (reset values of port A quoted from RM0383):
 *   RCC->APB2ENR   (0x40023844) bit 12 set
 *   GPIOA->MODER   (0x40020000) -> 0xA800AA20
 *     = 0xA8000020 (the UART's PA2 and the debug pins) + PA4..PA7 as AF (10 each = 0xAA00)
 *   GPIOA->OTYPER  (0x40020004) -> 0x00000000  PA6 push-pull
 *   GPIOA->OSPEEDR (0x40020008) -> 0x0C002000  reset 0x0C000000 + PA6 fast (10 at 13:12)
 *   GPIOA->PUPDR   (0x4002000C) -> 0x64000900  reset 0x64000000 + PA4 pull-up (0x100)
 *                                              + PA5 pull-down (0x800)
 *   GPIOA->AFR[0]  (0x40020020) -> 0x55550700  PA2 = 7 (UART), PA4..PA7 = 5
 *   SPI1->CR1      (0x40013000) -> 0x0000
 *   SPI1->CR2      (0x40013004) -> 0x0002
 *   SPI1->SR       (0x40013008) -> 0x0002      TXE set (not meaningful while disabled)
 *
 * The level of MISO while SPI1 is disabled is not relied on: the Pi reads MISO only
 * while NSS is low, and the SPI is enabled (spi_start) before the first transaction.
 * On the wire MISO idles low between transactions.
 */
#define SPI_PIN_ALTERNATE_FUNCTION    ( 5U )    /* AF5 = SPI1 on PA4..PA7 */

void spi_init( void )
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    ( void ) RCC->AHB1ENR;

    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    ( void ) RCC->APB2ENR;

    /* MISO is the only output. */
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT6;

    GPIOA->OSPEEDR = ( GPIOA->OSPEEDR & ~GPIO_OSPEEDR_OSPEED6 ) | GPIO_OSPEEDR_OSPEED6_1;

    /* NSS pull-up, SCK pull-down, MISO and MOSI no pull. */
    GPIOA->PUPDR = ( GPIOA->PUPDR & ~( GPIO_PUPDR_PUPD4 | GPIO_PUPDR_PUPD5 |
                                       GPIO_PUPDR_PUPD6 | GPIO_PUPDR_PUPD7 ) ) |
                   GPIO_PUPDR_PUPD4_0 | GPIO_PUPDR_PUPD5_1;

    GPIOA->AFR[ 0 ] = ( GPIOA->AFR[ 0 ] & ~( GPIO_AFRL_AFSEL4 | GPIO_AFRL_AFSEL5 |
                                             GPIO_AFRL_AFSEL6 | GPIO_AFRL_AFSEL7 ) ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL4_Pos ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL5_Pos ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL6_Pos ) |
                      ( SPI_PIN_ALTERNATE_FUNCTION << GPIO_AFRL_AFSEL7_Pos );

    SPI1->CR1 = 0U;
    SPI1->CR2 = SPI_CR2_TXDMAEN;

    /* MODER last: this connects the four pins to SPI1. */
    GPIOA->MODER = ( GPIOA->MODER & ~( GPIO_MODER_MODER4 | GPIO_MODER_MODER5 |
                                       GPIO_MODER_MODER6 | GPIO_MODER_MODER7 ) ) |
                   GPIO_MODER_MODER4_1 | GPIO_MODER_MODER5_1 |
                   GPIO_MODER_MODER6_1 | GPIO_MODER_MODER7_1;
}

/* ==========================================================================
 * STEP 2) THE DMA FEEDS SPI1->DR: CONFIGURED ONCE, RELOADED FOR EVERY FRAME
 * ==========================================================================
 *
 * THE PATH OF A BYTE. The frame to send sits in ucDmaFrame (below), a 14-byte array
 * private to this file. SPI1 is a slave: it can only shift a byte out when the master
 * clocks, and the byte must already be in its data register. With TXDMAEN set (step
 * 1), every time the SPI's transmit register is empty (flag TXE) it sends a request to
 * the DMA, and the DMA answers by taking the next byte of ucDmaFrame and writing it to
 * SPI1->DR (RM0383 p. 571). No CPU instruction takes part, so the byte is there in
 * time whatever the CPU is doing (a critical section, an interrupt of lower urgency):
 * the master's clock does not wait for the slave.
 *
 * TWO FUNCTIONS, BECAUSE THE STREAM STOPS BY ITSELF. The stream runs in NORMAL mode:
 * after FRAME_SIZE items the count is 0 and the HARDWARE clears EN (RM0383 p. 178), so
 * for the next frame the stream has to be started again. Circular mode would avoid
 * that, and was rejected in ADR-0010: after one transaction of a different length every
 * later frame would start at the wrong byte. What is set once and what is set again:
 *
 *   ONCE, prvDmaConfigure()           EVERY FRAME, prvDmaReloadAndEnable()
 *   -----------------------           ------------------------------------
 *   the DMA2 clock                    wait until EN reads 0 (bounded)
 *   PAR  = address of SPI1->DR        copy the new frame into ucDmaFrame
 *   M0AR = &ucDmaFrame[ 1 ]           write BYTE 0 into SPI1->DR (see below)
 *   CR   = the settings below         clear the stream's flags (LIFCR)
 *                                     NDTR = FRAME_SIZE - 1 again (it counted down to 0)
 *                                     EN
 *
 * CONFIRMED ON THE BOARD (2026-10-13, after one complete transaction with the Pi): CR
 * reads 0x06020440, i.e. only EN was cleared (by the hardware), and PAR still reads
 * 0x4001300C. So the settings written once stay, and only the counter has to be
 * reloaded. NDTR read 0.
 *
 * NOTE ON M0AR. The manual calls it the BASE address of the memory area, and that is
 * how it behaves: after a complete transaction it still read the address it had been
 * given, not that address + 14. The DMA walks through the buffer with its own running
 * pointer; NDTR is the visible progress. So M0AR is a setting like PAR: it is written
 * once, in prvDmaConfigure(), and not for every frame. (Checked on the board in two
 * places, before byte 0 was written by the CPU: with NDTR = 13 right after spi_start(),
 * and after a complete transaction.) It now holds the address of ucDmaFrame[ 1 ]
 * because the CPU sends byte 0 itself, see the next section.
 *
 * WHY THE CPU WRITES BYTE 0 INTO DR, AND THE DMA ONLY SENDS BYTES 1 TO 13
 * (found on the board on 2026-10-14; ADR-0010, decision 7).
 *
 * THE RULE. DR is always ONE BYTE AHEAD. TXE goes to 1 when a byte moves from DR into
 * the shifter, which is at the START of that byte, and then the DMA loads the next
 * one. So while byte k shifts out, DR already holds byte k + 1. With n bytes clocked
 * the DMA has loaded bytes 0 to n, and DR ends up empty ONLY IF n = 14 (there is no
 * byte 14 to load). After a transaction of fewer than 14 bytes, or one that was
 * restarted in the middle, DR still holds an unsent byte and TXE = 0.
 *
 * THE FAILURE. The first version only enabled the stream at the re-arm and relied on
 * the SPI raising a request (TXE = 1) so that the DMA loaded byte 0. With a leftover
 * byte in DR there is no request: the leftover goes out FIRST, the frame follows one
 * byte late, the last byte (the CRC high byte) is loaded but the transaction ends
 * before it can be shifted, and it becomes the next leftover: the node stays shifted
 * for ever. Seen in a 50-read test: reads 1 to 19 correct; read 20 hit by a re-arm
 * three bytes into the transaction (an NSS rising edge in the middle of it); reads 21
 * to 49 all shifted by one byte, each junk first byte equal to the CRC high byte of
 * the frame before it (checked for four frames).
 *
 * THE FIX. At every re-arm the CPU writes byte 0 into DR ITSELF, which replaces
 * whatever was there, and the stream is configured for bytes 1 to 13 only (M0AR =
 * &ucDmaFrame[ 1 ], NDTR = FRAME_SIZE - 1). After a re-arm the state is always "byte 0
 * in DR, TXE = 0, the stream waiting for byte 1", whatever the previous transaction
 * was. A transaction that is hit by a disturbance is still corrupted (its bytes have
 * already gone out: the Pi finds it with the CRC), but the NEXT one is correct.
 *
 * ORDER, WHICH MATTERS: stop the stream, copy the frame, write DR, and only then enable
 * the stream. If the stream were enabled while TXE = 1 it would serve a request at
 * once and load byte 1 into DR, and the CPU's byte 0 would then be written over it.
 *
 * CHECKED ON THE BOARD (2026-10-14): the first read after a reset is the placeholder, so
 * DR may be written while SPE is still 0 (the first arming does that); and after a
 * deliberate short read (7 bytes) the next full read was a correct frame, so a write
 * to DR does replace a byte that is still there. In a burst of 50 reads, 47 were good,
 * none was shifted, and the 3 bad ones were taken while the NSS wire was being moved
 * by hand; each was followed by a good one.
 *
 * THE CR VALUE (0x06020440 before EN, 0x06020441 with EN):
 *   CHSEL = 3        channel 3 = the SPI1_TX request line
 *   PL    = 10       priority "high". Only this stream is active, so it makes no
 *                    difference; it is stated because this is the time-critical
 *                    transfer of the system
 *   MSIZE = 00       memory item = 1 byte
 *   PSIZE = 00       peripheral item = 1 byte (SPI frames are 8 bits, DFF = 0)
 *   MINC  = 1        the memory address goes up after each byte: it walks the frame
 *   PINC  = 0        the peripheral address stays: it is always SPI1->DR
 *   CIRC  = 0        NORMAL mode (see above)
 *   DIR   = 01       memory to peripheral
 *   TCIE, HTIE, TEIE, DMEIE = 0   no interrupts. The transmit task is woken by the
 *                    NSS edge (step 3), not by the DMA (ADR-0010, decision 6)
 * The stream is in DIRECT mode (the reset state of FCR, DMDIS = 0): each request moves
 * one item, no FIFO; nothing needs more.
 *
 * THE RULE FOR THE BUFFER. ucDmaFrame is written only while the stream is stopped
 * (a critical section cannot hold back the DMA, which is another bus master).
 *
 * ORDER IN spi_start(): configure, build the placeholder, load DR and enable the
 * stream, and only then the SPI (SPE). The manual asks for a slave to be enabled before
 * the master clocks and with its data register ready (p. 555). The rule kept here is
 * "SPI enabled means the data register is loaded": byte 0 is already in DR when SPE is
 * set, and the stream is already enabled for bytes 1 to 13.
 *
 * ONE BOUNDED WAIT, FLAGGED: waiting for EN to read 0 is a busy loop, limited to a
 * number of passes. At start-up the stream has just come out of reset, so EN is already 0
 * and the loop does not run; it matters for the re-arm (step 3), where the stream may
 * be in the middle of a transfer. If it ever times out the function reports it and changes
 * nothing.
 *
 * Expected values after spi_start() (DMA2 base 0x40026400; stream 3 starts at +0x58):
 *   RCC->AHB1ENR        (0x40023830) bit 22 (DMA2EN) set; bit 0 (GPIOA) and bit 1
 *                       (GPIOB, the I2C driver) too -> 0x00400003
 *   DMA2_Stream3->CR    (0x40026458) -> 0x06020441
 *   DMA2_Stream3->NDTR  (0x4002645C) -> 13  = FRAME_SIZE - 1, the count that was loaded.
 *                       Since the CPU wrote byte 0 and TXE = 0, no request has been
 *                       served yet. (It read 13 before the CPU wrote byte 0 as well, for
 *                       another reason: then the DMA had loaded byte 0 itself.)
 *   DMA2_Stream3->PAR   (0x40026460) -> 0x4001300C (SPI1 base 0x40013000 + DR at 0x0C)
 *   DMA2_Stream3->M0AR  (0x40026464) -> the address of ucDmaFrame PLUS ONE (see the map
 *                       file). It keeps that value during and after the transfer (a base
 *                       address, see the note)
 *   SPI1->CR1           (0x40013000) -> 0x0040  SPE
 *   SPI1->SR            (0x40013008) -> TXE (bit 1) should be 0: the register holds byte 0
 *   ucDmaFrame (14 bytes) -> A5 01 00 00 00 00 00 00 00 00 00 00 10 96, where the status
 *                       is 01 (FRAME_STATUS_NO_READING), the sequence 00 00 and the CRC
 *                       0x9610 (read by the Pi over the wire and matched)
 */
#define SPI_DMA_STOP_WAIT_PASSES    ( 10000U )

_Static_assert( ( FRAME_SIZE > 1U ) && ( FRAME_SIZE <= 0xFFFFU ),
                "DMA_SxNDTR is a 16-bit counter: the frame must fit in it, "
                "and the CPU sends byte 0, so at least one byte is left for the DMA" );

static uint8_t ucDmaFrame[ FRAME_SIZE ];

/* Everything about stream 3 that stays the same from one frame to the next. Called
 * once. It does not wait for EN to read 0: the stream has just come out of reset, so it
 * is disabled, and CR, PAR and M0AR may be written. The stream is left DISABLED. */
static void prvDmaConfigure( void )
{
    /* The clock of DMA2, the controller that serves SPI1 (on the AHB1 bus). It is
     * enabled here, next to the only code that uses the DMA. The dummy read gives the
     * clock time to arrive, as for the other enables. */
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    ( void ) RCC->AHB1ENR;

    /* The peripheral side: always SPI1->DR (PINC = 0). */
    DMA2_Stream3->PAR = ( uint32_t ) &SPI1->DR;

    /* The memory side: ucDmaFrame[ 1 ], because byte 0 is written into DR by the CPU
     * (see "WHY THE CPU WRITES BYTE 0" above). It is a BASE address, so it does not move
     * while the stream runs and does not have to be written again. */
    DMA2_Stream3->M0AR = ( uint32_t ) &ucDmaFrame[ 1 ];

    DMA2_Stream3->CR = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_1 |    /* channel 3 */
                       DMA_SxCR_PL_1 |                          /* priority high */
                       DMA_SxCR_MINC |                          /* memory address increments */
                       DMA_SxCR_DIR_0;                          /* memory to peripheral */
}

/* What has to be done again for every frame (RM0383 section 9.3.17): make sure the
 * stream is stopped, put the new frame into ucDmaFrame, write its byte 0 into DR, clear
 * the stream's old flags, reload the count, and enable it. The addresses and the
 * settings were written once, by prvDmaConfigure().
 *
 * THIS IS THE ONLY PLACE THAT WRITES ucDmaFrame, and it does so after the stop and
 * before the enable (stop, copy, reload): the DMA is another bus master that reads that
 * array on its own, and a critical section could not hold it back. Both the first
 * arming (spi_start) and every re-arm (spi_load_frame) come through here.
 *
 * pucFrame points to FRAME_SIZE bytes. Returns SPI_OK, or SPI_ERR_DMA_NOT_STOPPING if
 * the stream did not stop in time (nothing was changed then). */
static spi_status_t prvDmaReloadAndEnable( const uint8_t * pucFrame )
{
    uint32_t ulPasses = SPI_DMA_STOP_WAIT_PASSES;
    uint32_t ulIndex;

    DMA2_Stream3->CR &= ~DMA_SxCR_EN;

    while( ( ( DMA2_Stream3->CR & DMA_SxCR_EN ) != 0U ) && ( ulPasses > 0U ) )
    {
        ulPasses--;
    }

    if( ( DMA2_Stream3->CR & DMA_SxCR_EN ) != 0U )
    {
        return SPI_ERR_DMA_NOT_STOPPING;
    }

    /* The stream is stopped: the DMA is not reading ucDmaFrame now. */
    for( ulIndex = 0U; ulIndex < FRAME_SIZE; ulIndex++ )
    {
        ucDmaFrame[ ulIndex ] = pucFrame[ ulIndex ];
    }

    /* Byte 0 goes into DR from the CPU, replacing whatever is there (a byte left over
     * from a short or interrupted transaction, for instance). From here on DR holds
     * byte 0 and TXE = 0, whatever happened before. It is done BEFORE the stream is
     * enabled: with TXE = 1 the stream would serve a request at once and load byte 1
     * into DR, and this write would then replace it. */
    SPI1->DR = ucDmaFrame[ 0 ];

    DMA2->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CFEIF3;

    /* The DMA sends bytes 1 to FRAME_SIZE - 1. */
    DMA2_Stream3->NDTR = FRAME_SIZE - 1U;

    DMA2_Stream3->CR |= DMA_SxCR_EN;

    return SPI_OK;
}

/* ==========================================================================
 * STEP 3a) THE END OF A TRANSACTION: AN INTERRUPT ON NSS RISING (PA4, EXTI LINE 4)
 * ==========================================================================
 *
 * WHY THIS EDGE. A transaction ends when the Pi raises NSS. That is the one signal that
 * is there whether the Pi clocked 14 bytes, fewer, or more, and it is the moment when
 * the frame buffer may be refilled for the next transaction (ADR-0010, decision 6).
 * The DMA's own "transfer complete" cannot be used for this: it does not fire at all if
 * the Pi clocks fewer bytes than the count.
 *
 * EXTI is the chip's external interrupt controller. Its lines 0 to 15 are wired to pins
 * BY NUMBER: line 4 listens to pin 4 of one port, chosen in SYSCFG. NSS is PA4, so line
 * 4 with port A. The pin stays connected to SPI1 (alternate function): the manual says
 * that in alternate-function mode the pin's input stage is active (section 8.3.11), so
 * EXTI should see it. 
 * 
 * WHAT IS WRITTEN, IN ORDER:
 *   RCC->APB2ENR  SYSCFGEN   the clock of the SYSCFG block (it holds the port selector)
 *   SYSCFG->EXTICR[1]  field EXTI4 = 0000   (EXTICR2 holds lines 4 to 7) line 4 takes
 *                            port A. Cleared explicitly, although it is the reset value
 *   EXTI->RTSR    TR4        rising edge triggers. Falling is NOT enabled: the start of
 *                            a transaction is not an event we want
 *   EXTI->PR      PR4        clear a pending bit left from before: its reset value is
 *                            undefined, and it is cleared by WRITING 1 to it
 *   EXTI->IMR     MR4        unmask: let the line raise an interrupt (reset: masked)
 *   NVIC          priority 6 THEN enable, never the other way (after reset every
 *                            interrupt is at level 0, above the FreeRTOS line, ADR-0004)
 *
 * PRIORITY 6. The handler calls a ...FromISR() function (the transmit task's), so it
 * must be at level 5 or numerically higher (ADR-0004). 6 is just below the line: more
 * urgent than the UART (10), because the re-arm has to finish before the Pi's next
 * chip select. Checked at build time, as in uart.c.
 *
 * THE HANDLER. The name EXTI4_IRQHandler is fixed by the vector table in the startup
 * file, where it is a weak placeholder; defining it here replaces it. It must clear the
 * pending bit (write 1), otherwise the interrupt fires again at once, forever. It
 * counts the transactions seen (ulNssRisingCount, to be watched in the debugger: it
 * goes up by one for each read made from the Pi) and notifies the transmit task, which
 * does the real work (transmit_task.c).
 *
 * WHY THE HANDLER LOOKS AT THE PIN BEFORE IT BELIEVES THE EDGE (ADR-0010, decision 8).
 * EXTI has no filter: a very short spike on NSS in the middle of a transaction is a
 * rising edge like any other, and the wake-up it caused re-armed the DMA in the middle
 * of the frame: the transaction restarted from byte 0 (seen on the board, for example
 * a frame that was its first 7 bytes twice). A real end of transaction and a spike
 * differ in one thing that can be read: the LEVEL OF THE PIN a few microseconds later.
 * After a real end NSS is high, and stays high until the Pi starts the next
 * transaction (tens of microseconds at least). After a spike the Pi is still holding
 * NSS low, so it is low again by the time the handler runs. GPIOA->IDR gives the
 * level of PA4 at that moment (the pin's input stage is active in alternate-function
 * mode, the same fact that lets EXTI see it).
 *
 * So: NSS high = a transaction ended (counted in ulNssRisingCount, task woken);
 * NSS low = a glitch (counted in ulNssGlitchCount, nothing else is done). The interrupt
 * itself still happens: only its consequence is avoided. The pending bit is cleared
 * BEFORE the pin is read, so that an edge arriving in between is not lost.
 *
 * What it does not do: it does not remove the noise, and it does not help against a
 * spike that is still there when the handler looks, nor against a real loss of the
 * contact (then the SPI peripheral itself loses the frame, and the CRC finds it).
 * ulNssGlitchCount counts how often it happens (7 in 3000 and 15 in 1800 transactions
 * in the long runs). The logic analyzer saw spikes of one sample (50 ns) on NSS exactly
 * at SCK's falling edge with a probe wiring that disturbed the link, and none in about
 * 40 frames with a clean wiring; whether the counted spikes have the same cause is not
 * settled (ADR-0010).
 *
 * BEFORE THE SCHEDULER RUNS: the interrupt is enabled while the kernel's BASEPRI is
 * still raised (it is only lowered by the SVC handler that starts the first task, see
 * main.c), so an edge before that is held pending, not lost.
 *
 * Expected values afterwards:
 *   RCC->APB2ENR      (0x40023844) bit 14 (SYSCFGEN) and bit 12 (SPI1EN) -> 0x00005000
 *   SYSCFG->EXTICR[1] (0x4001380C) -> 0x00000000  (port A)
 *   EXTI->IMR         (0x40013C00) -> 0x00000010  (line 4 unmasked)
 *   EXTI->RTSR        (0x40013C08) -> 0x00000010  (rising edge)
 *   NVIC priority of EXTI4 (IRQ 10: 0xE000E40A, one byte) -> 0x60  (level 6 in the top 4 bits)
 *   NVIC_ISER0        (0xE000E100) bit 10 set
 */
#define SPI_NSS_IRQ_PRIORITY    ( 6U )

_Static_assert( SPI_NSS_IRQ_PRIORITY >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
                "the NSS interrupt must be at or below the FreeRTOS line (ADR-0004): "
                "its handler will call a FromISR function" );
_Static_assert( SPI_NSS_IRQ_PRIORITY < ( 1U << __NVIC_PRIO_BITS ),
                "interrupt level out of range for this chip" );

/* Number of NSS rising edges that were REAL ends of a transaction (NSS was high when the
 * handler looked). Written only by the handler. */
static volatile uint32_t ulNssRisingCount;

/* Number of rising edges that were NOT: NSS was low again when the handler looked, so
 * they were spikes in the middle of a transaction. Written only by the handler. */
static volatile uint32_t ulNssGlitchCount;

/* Bit 4 of GPIOA->IDR is PA4, the NSS pin (GPIO_IDR_ID4 in the device header). */
#define SPI_NSS_IDR_BIT    ( 1UL << 4 )

static void prvNssInterruptEnable( void )
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    ( void ) RCC->APB2ENR;

    /* Line 4 takes port A (field value 0000). */
    SYSCFG->EXTICR[ 1 ] &= ~SYSCFG_EXTICR2_EXTI4;

    EXTI->RTSR |= EXTI_RTSR_TR4;

    /* A stale pending bit would fire the moment the line is unmasked. */
    EXTI->PR = EXTI_PR_PR4;
    EXTI->IMR |= EXTI_IMR_MR4;

    /* Priority first, then enable: it can never run at the reset priority 0. */
    NVIC_SetPriority( EXTI4_IRQn, SPI_NSS_IRQ_PRIORITY );
    NVIC_EnableIRQ( EXTI4_IRQn );
}

void EXTI4_IRQHandler( void )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if( ( EXTI->PR & EXTI_PR_PR4 ) != 0U )
    {
        /* Cleared by writing 1, not by read-modify-write. Before the pin is read, so
         * that an edge arriving in between sets the bit again and is not lost. */
        EXTI->PR = EXTI_PR_PR4;

        if( ( GPIOA->IDR & SPI_NSS_IDR_BIT ) != 0UL )
        {
            /* NSS is high: the Pi ended the transaction. Tell the transmit task, which
             * does the work. The handler itself stays this short. */
            ulNssRisingCount++;
            transmit_task_end_of_transaction_from_isr( &xHigherPriorityTaskWoken );
        }
        else
        {
            /* NSS is low again: a spike in the middle of a transaction. Do not re-arm,
             * the frame in progress must not be restarted. */
            ulNssGlitchCount++;
        }
    }

    /* If the notification woke a task more urgent than the one that was running (the
     * transmit task is at priority 2, the sensor task at 1), switch to it as soon as
     * this handler returns, not at the next tick. */
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

spi_status_t spi_start( void )
{
    spi_status_t xStatus;
    uint8_t aucPlaceholder[ FRAME_SIZE ];

    prvDmaConfigure();

    /* The frame the Pi gets until a real one is handed over: "no reading yet",
     * sequence 0 (the value kept for exactly this, see sensor_task.c). It is built in a
     * local array and given to prvDmaReloadAndEnable(), like every later frame. */
    frame_build( aucPlaceholder, FRAME_STATUS_NO_READING, 0U, 0, 0U );

    xStatus = prvDmaReloadAndEnable( aucPlaceholder );

    if( xStatus == SPI_OK )
    {
        /* The end-of-transaction interrupt (step 3a), before the SPI goes live. */
        prvNssInterruptEnable();

        /* The stream is listening; now the SPI may ask for its first byte. */
        SPI1->CR1 |= SPI_CR1_SPE;
    }

    return xStatus;
}

/* ==========================================================================
 * STEP 3c) THE FRAME FOR THE NEXT TRANSACTION
 * ==========================================================================
 *
 * Called by the transmit task after every transaction, with the newest frame. It stops
 * the stream (waiting, bounded, until EN reads 0), copies the frame into ucDmaFrame and
 * starts the stream again, all in prvDmaReloadAndEnable(). It never assumes that the
 * master clocked exactly 14 bytes: after a short transaction the stream is still
 * enabled, and prvDmaReloadAndEnable() stops it first (ADR-0010, "stopped and checked
 * before every reload").
 *
 * IF THE STREAM DOES NOT STOP IN TIME the frame is NOT loaded, the failure is counted in
 * ulReloadFailures, and the status is returned. Nothing else is done: the next
 * transaction will try again. (An assertion was rejected: the node would stop for
 * something the Pi can cause.)
 *
 * It is for a TASK, not for an interrupt (the loop is bounded but is a busy wait), and
 * it must not be called at the same time from two tasks: only the transmit task calls
 * it.
 *
 * It takes the frame from the CALLER, so this module knows nothing about queues or tasks
 * (the transmit task fetches the frame; the application layer stays above the driver).
 */
static volatile uint32_t ulReloadFailures;

spi_status_t spi_load_frame( const uint8_t * pucFrame )
{
    spi_status_t xStatus = prvDmaReloadAndEnable( pucFrame );

    if( xStatus != SPI_OK )
    {
        ulReloadFailures++;
    }

    return xStatus;
}

uint32_t spi_reload_failures( void )
{
    return ulReloadFailures;
}