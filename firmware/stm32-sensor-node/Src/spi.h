/*
 * spi.h — SPI1 as a SLAVE towards the gateway (the Raspberry Pi is the master): the
 * data link of Phase 2 (ADR-0010).
 *
 * PA4 = NSS (chip select, from the Pi), PA5 = SCK (from the Pi), PA6 = MISO (our
 * output: the frame goes out here), PA7 = MOSI (from the Pi; nothing uses it yet).
 */

#ifndef SPI_H
#define SPI_H

typedef enum
{
    SPI_OK = 0,
    SPI_ERR_DMA_NOT_STOPPING    /* the DMA stream did not stop in time: nothing was changed */
} spi_status_t;

/*
 * Configures the clock, the four pins and SPI1 as a slave in mode 0 (clock idles low,
 * data sampled on the rising edge), 8-bit frames, most significant bit first, hardware
 * NSS, transmit DMA requests enabled.
 *
 * The peripheral is left DISABLED (SPE = 0): a slave must have its data register ready
 * before the master's first clock edge (RM0383 p. 555), so it is enabled later, by the
 * code that arms the DMA. Call once from main(), before the scheduler starts.
 */
void spi_init( void );

/*
 * Arms DMA2 stream 3 with the placeholder frame ("no reading yet", sequence 0) and then
 * ENABLES the SPI: from here on the Pi can clock a frame out. Call once, after
 * spi_init() and before the scheduler starts. It does not need the scheduler.
 *
 * Returns SPI_OK, or SPI_ERR_DMA_NOT_STOPPING (the SPI is then not enabled).
 */
spi_status_t spi_start( void );

/*
 * Makes pucFrame (FRAME_SIZE bytes, see frame.h) the frame the Pi will read in its NEXT
 * transaction: it stops the DMA stream, waiting until it is really stopped, copies the
 * frame into the DMA buffer, writes the frame's byte 0 into the SPI data register
 * itself (replacing any byte left there by a short or interrupted transaction), and
 * starts the stream for the remaining bytes. The state after the call is therefore the
 * same whatever the previous transaction was (see spi.c).
 *
 * It is meant to be called by the transmit task, once after every transaction (the NSS
 * rising edge wakes it). Call it from a task only, and from one task only. It must be
 * done before the Pi's next chip select, so the Pi has to leave a gap between
 * transactions.
 *
 * Returns SPI_OK, or SPI_ERR_DMA_NOT_STOPPING if the stream did not stop in time: the
 * frame was NOT loaded, and the failure was counted.
 */
spi_status_t spi_load_frame( const uint8_t * pucFrame );

/* How many times spi_load_frame() failed because the stream did not stop in time.
 * Normally 0. For diagnostics. */
uint32_t spi_reload_failures( void );

#endif /* SPI_H */
