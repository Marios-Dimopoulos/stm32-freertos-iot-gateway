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

#endif /* SPI_H */
