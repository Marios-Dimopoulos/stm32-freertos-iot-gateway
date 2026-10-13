/*
 * spi.c — SPI1 as a slave, towards the Raspberry Pi (ADR-0010).
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) clock, pins, SPI1 configured as a slave, left disabled        <- DONE
 *   2) DMA2 stream 3 feeding SPI1->DR, armed once, SPI enabled       <- THIS STEP
 *   3) re-arming after every transaction (EXTI on NSS + the transmit task)
 *
 * WHY A SLAVE. The Pi's Linux SPI driver (Phase 4) sits behind a master controller, so
 * the Pi clocks and the STM32 answers (ADR-0010, decision 1). In slave mode the clock
 * comes from the master, and the baud-rate bits BR[2:0] of CR1 do not affect the
 * transfer rate (RM0383 p. 555). The 6 MHz limit of the project is therefore enforced
 * on the Pi's side, not here.
 */

#include "FreeRTOS.h"
#include "spi.h"
#include "frame.h"
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
 *     SPE  = 0             DISABLED for now (see spi.h)
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
 * NOT KNOWN: what level MISO has while the pin is connected to SPI1 but the SPI is
 * disabled; I did not find it in the manual. Nothing is connected in this step.
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
 * WHICH STREAM. Table 28 of RM0383 (DMA2 request mapping): SPI1_TX is channel 3 of
 * stream 3 (stream 5 also serves it). Stream 3 is used. (I read that table from a text
 * extraction of the PDF, where the columns are partly lost; the cell is to be confirmed
 * against the PDF page.)
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
 *   PAR  = address of SPI1->DR        clear the stream's flags (LIFCR)
 *   CR   = the settings below         M0AR = ucDmaFrame again (the hardware moved it)
 *                                     NDTR = FRAME_SIZE again (it counted down to 0)
 *                                     EN
 *
 * ASSUMPTION, NOT YET SEEN ON THE BOARD: that CR and PAR keep their values when the
 * stream stops. The manual's wording ("reprograms the stream") suggests only the
 * counter and the memory address change. Step 3 tests it: after a complete
 * transaction CR should still read 0x06020440 and PAR the same address. If not,
 * prvDmaReloadAndEnable() has to write them too.
 *
 * THE CR VALUE (0x06020440 before EN, 0x06020441 with EN):
 *   CHSEL = 3        channel 3 = the SPI1_TX request line
 *   PL    = 10       priority "high". Only this stream is active, so it makes no
 *                    difference today; it is stated because this is the time-critical
 *                    transfer of the system
 *   MSIZE = 00       memory item = 1 byte
 *   PSIZE = 00       peripheral item = 1 byte (SPI frames are 8 bits, DFF = 0)
 *   MINC  = 1        the memory address goes up after each byte: it walks the frame
 *   PINC  = 0        the peripheral address stays: it is always SPI1->DR
 *   CIRC  = 0        NORMAL mode (see above)
 *   DIR   = 01       memory to peripheral
 *   TCIE, HTIE, TEIE, DMEIE = 0   no interrupts. The transmit task will be woken by the
 *                    NSS edge (step 3), not by the DMA (ADR-0010, decision 6)
 * The stream is in DIRECT mode (the reset state of FCR, DMDIS = 0): each request moves
 * one item, no FIFO; nothing needs more.
 *
 * THE RULE FOR THE BUFFER. ucDmaFrame is written only while the stream is stopped
 * (a critical section cannot hold back the DMA, which is another bus master).
 *
 * ORDER IN spi_start(): configure, fill the buffer, load and enable the stream, and
 * only then the SPI (SPE). The manual asks for a slave to be enabled before the master
 * clocks and with its data register ready (p. 555). The rule kept here is "SPI enabled
 * means the data register is loaded": once SPE is set the SPI raises its first request
 * at once (TXE is 1), and the DMA is already enabled. (That a request raised before
 * the stream is enabled would be lost is not something I found in the manual; this is
 * the order that cannot go wrong.)
 *
 * ONE BOUNDED WAIT, FLAGGED: waiting for EN to read 0 is a busy loop, limited to a
 * number of passes. Today the stream has just come out of reset, so EN is already 0
 * and the loop does not run; it matters for step 3, where the stream may be in the
 * middle of a transfer. If it ever times out the function reports it and changes
 * nothing.
 *
 * Expected values after spi_start() (DMA2 base 0x40026400; stream 3 starts at +0x58):
 *   RCC->AHB1ENR        (0x40023830) bit 22 (DMA2EN) set; bit 0 (GPIOA) and bit 1
 *                       (GPIOB, the I2C driver) too -> 0x00400003
 *   DMA2_Stream3->CR    (0x40026458) -> 0x06020441
 *   DMA2_Stream3->NDTR  (0x4002645C) -> 13  PREDICTION, confirmed on the board: when SPE
 *                       goes on the SPI raises its first request and the DMA moves byte
 *                       0 into SPI1->DR; nothing clocks, so the second byte is not
 *                       requested
 *   DMA2_Stream3->PAR   (0x40026460) -> 0x4001300C (SPI1 base 0x40013000 + DR at 0x0C)
 *   DMA2_Stream3->M0AR  (0x40026464) -> the address of ucDmaFrame (see the map file).
 *                       Note: the DMA moves M0AR forward by one per byte, so after byte
 *                       0 it reads one more than ucDmaFrame
 *   SPI1->CR1           (0x40013000) -> 0x0040  SPE
 *   SPI1->SR            (0x40013008) -> TXE (bit 1) should be 0: the register holds byte 0
 *   ucDmaFrame (14 bytes) -> A5 01 00 00 00 00 00 00 00 00 00 00 CRC_LO CRC_HI, where the
 *                       status is 01 (FRAME_STATUS_NO_READING) and the sequence 00 00
 */
#define SPI_DMA_STOP_WAIT_PASSES    ( 10000U )

_Static_assert( ( FRAME_SIZE > 0U ) && ( FRAME_SIZE <= 0xFFFFU ),
                "DMA_SxNDTR is a 16-bit counter: the frame must fit in it" );

static uint8_t ucDmaFrame[ FRAME_SIZE ];

/* Everything about stream 3 that stays the same from one frame to the next. Called
 * once. It does not wait for EN to read 0: the stream has just come out of reset, so it
 * is disabled, and CR and PAR may be written. The stream is left DISABLED. */
static void prvDmaConfigure( void )
{
    /* The clock of DMA2, the controller that serves SPI1 (on the AHB1 bus). It is
     * enabled here, next to the only code that uses the DMA. The dummy read gives the
     * clock time to arrive, as for the other enables. */
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    ( void ) RCC->AHB1ENR;

    /* The peripheral side: always SPI1->DR (PINC = 0). */
    DMA2_Stream3->PAR = ( uint32_t ) &SPI1->DR;

    DMA2_Stream3->CR = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_1 |    /* channel 3 */
                       DMA_SxCR_PL_1 |                          /* priority high */
                       DMA_SxCR_MINC |                          /* memory address increments */
                       DMA_SxCR_DIR_0;                          /* memory to peripheral */
}

/* What has to be done again for every frame (RM0383 section 9.3.17): make sure the
 * stream is stopped, clear its old flags, point it at the start of ucDmaFrame again,
 * reload the count, and enable it. ucDmaFrame must already hold the frame to send, and
 * must not be written again until the stream has stopped. Returns SPI_OK, or
 * SPI_ERR_DMA_NOT_STOPPING if the stream did not stop in time (nothing was changed). */
static spi_status_t prvDmaReloadAndEnable( void )
{
    uint32_t ulPasses = SPI_DMA_STOP_WAIT_PASSES;

    DMA2_Stream3->CR &= ~DMA_SxCR_EN;

    while( ( ( DMA2_Stream3->CR & DMA_SxCR_EN ) != 0U ) && ( ulPasses > 0U ) )
    {
        ulPasses--;
    }

    if( ( DMA2_Stream3->CR & DMA_SxCR_EN ) != 0U )
    {
        return SPI_ERR_DMA_NOT_STOPPING;
    }

    DMA2->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CFEIF3;

    DMA2_Stream3->M0AR = ( uint32_t ) ucDmaFrame;
    DMA2_Stream3->NDTR = FRAME_SIZE;

    DMA2_Stream3->CR |= DMA_SxCR_EN;

    return SPI_OK;
}

spi_status_t spi_start( void )
{
    spi_status_t xStatus;

    prvDmaConfigure();

    /* The frame the Pi gets until a real one is handed over: "no reading yet",
     * sequence 0 (the value kept for exactly this, see sensor_task.c). */
    frame_build( ucDmaFrame, FRAME_STATUS_NO_READING, 0U, 0, 0U );

    xStatus = prvDmaReloadAndEnable();

    if( xStatus == SPI_OK )
    {
        /* The stream is listening; now the SPI may ask for its first byte. */
        SPI1->CR1 |= SPI_CR1_SPE;
    }

    return xStatus;
}