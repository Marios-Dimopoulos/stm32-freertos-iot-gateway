/*
 * spi-frame-test: reads frames from the STM32 sensor node through /dev/spidev0.0 and checks
 * them. It is the C version of the checks in tools/spi_frame_test.py (the Phase 2 test jig),
 * because the gateway image has no Python. The frame layout and the CRC are in
 * firmware/stm32-sensor-node/Src/frame.h and docs/decisions/0010-spi-link-design.md.
 *
 *     spi-frame-test [count]       count frames, one per second (default 1)
 *
 * This is a bring-up tool, not the Phase 4 driver: it uses the generic user-space `spidev`
 * interface, which the kernel driver of Phase 4 will replace.
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SPI_DEVICE      "/dev/spidev0.0"
#define SPI_MODE        SPI_MODE_0
/* 1 MHz: well under the 6 MHz limit of the project (the logic analyzer samples at 24 MHz). */
#define SPI_SPEED_HZ    1000000
#define PAUSE_SECONDS   1

#define FRAME_SIZE      14
#define FRAME_MARKER    0xA5
#define CRC_BYTES       12          /* the CRC covers bytes 0..11; it is stored in bytes 12..13 */
#define CRC_START       0xFFFF
#define CRC_POLYNOMIAL  0x1021

/* Offsets in the frame. Numbers are little-endian, as the STM32 wrote them. */
#define OFFSET_MARKER   0
#define OFFSET_STATUS   1
#define OFFSET_SEQUENCE 2           /* 2 bytes */
#define OFFSET_TEMP     4           /* 4 bytes, signed, hundredths of a degree Celsius */
#define OFFSET_PRESSURE 8           /* 4 bytes, unsigned, pascals */
#define OFFSET_CRC      12          /* 2 bytes */

#define CENTI_PER_DEGREE 100.0      /* the temperature is in hundredths of a degree */
#define PA_PER_HPA       100.0

/* CRC-16/CCITT-FALSE: polynomial 0x1021, start 0xFFFF, no reflection, no final XOR. Same as
 * the Python test and the firmware; a different variant would reject every frame. */
static uint16_t crc16(const uint8_t *data, int length)
{
    uint16_t crc = CRC_START;

    for (int i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ CRC_POLYNOMIAL) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Built byte by byte so that the result does not depend on the byte order of the CPU. */
static uint32_t read_le(const uint8_t *p, int bytes)
{
    uint32_t value = 0;

    for (int i = bytes - 1; i >= 0; i--) {
        value = (value << 8) | p[i];
    }
    return value;
}

int main(int argc, char **argv)
{
    int count = (argc > 1) ? atoi(argv[1]) : 1;
    uint8_t mode = SPI_MODE;
    int bad = 0;

    int fd = open(SPI_DEVICE, O_RDWR);
    if (fd < 0) {
        perror(SPI_DEVICE);
        return 1;
    }
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("SPI_IOC_WR_MODE");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        uint8_t tx[FRAME_SIZE] = { 0 };     /* the node ignores what the master sends */
        uint8_t rx[FRAME_SIZE] = { 0 };
        struct spi_ioc_transfer transfer;

        /* One transfer of FRAME_SIZE bytes with NSS low the whole time: the node's DMA sends
         * the whole frame in this one transaction. Two smaller transfers would be two
         * transactions and the second would not continue the frame. */
        memset(&transfer, 0, sizeof(transfer));
        transfer.tx_buf = (unsigned long)tx;
        transfer.rx_buf = (unsigned long)rx;
        transfer.len = FRAME_SIZE;
        transfer.speed_hz = SPI_SPEED_HZ;

        if (ioctl(fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
            perror("SPI_IOC_MESSAGE");
            return 1;
        }

        int good = (rx[OFFSET_MARKER] == FRAME_MARKER) &&
                   (crc16(rx, CRC_BYTES) == read_le(&rx[OFFSET_CRC], 2));

        printf("%3d %s ", i, good ? "good" : "BAD ");
        for (int k = 0; k < FRAME_SIZE; k++) {
            printf("%02X ", rx[k]);
        }
        if (good) {
            printf(" seq=%u status=%u T=%.2f C P=%.2f hPa",
                   (unsigned)read_le(&rx[OFFSET_SEQUENCE], 2),
                   (unsigned)rx[OFFSET_STATUS],
                   (int32_t)read_le(&rx[OFFSET_TEMP], 4) / CENTI_PER_DEGREE,
                   read_le(&rx[OFFSET_PRESSURE], 4) / PA_PER_HPA);
        } else {
            bad++;
        }
        printf("\n");

        if (i + 1 < count) {
            sleep(PAUSE_SECONDS);
        }
    }

    close(fd);
    return bad ? 1 : 0;
}
