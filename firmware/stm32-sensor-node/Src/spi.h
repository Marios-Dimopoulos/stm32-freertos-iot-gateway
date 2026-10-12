/*
 * spi.h — SPI1 as a SLAVE towards the gateway (the Raspberry Pi is the master): the
 * data link of Phase 2 (ADR-0010).
 *
 * PA4 = NSS (chip select, from the Pi), PA5 = SCK (from the Pi), PA6 = MISO (our
 * output: the frame goes out here), PA7 = MOSI (from the Pi; nothing uses it yet).
 */

#ifndef SPI_H
#define SPI_H

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

#endif /* SPI_H */
