/*
 * gateway-publisher: reads frames from the STM32 sensor node through /dev/stm32_sensor_node and
 * sends them to the MQTT broker on the gateway (ADR-0020, ADR-0021).
 *
 * Step 4.6c, piece 1 (this file): read ONE frame and print it decoded. There is no MQTT yet, so
 * the program can be tested on the board alone.
 *
 * The program does NOT check the marker or the CRC. The kernel driver does that and makes read()
 * fail with EIO for a bad frame (ADR-0018: the driver checks, the program decodes), so a
 * frame that read() returns is already a good one. Checking again would be code for no new
 * protection.
 *
 * The frame layout is in firmware/stm32-sensor-node/Src/frame.h and docs/decisions/0010-spi-link-
 * design.md. All numbers in it are little-endian (the low byte comes first).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define DEVICE          "/dev/stm32_sensor_node"

#define FRAME_SIZE      14
#define OFFSET_STATUS   1           /* 1 byte */
#define OFFSET_SEQUENCE 2           /* 2 bytes, unsigned */
#define OFFSET_TEMP     4           /* 4 bytes, signed, hundredths of a degree Celsius */
#define OFFSET_PRESSURE 8           /* 4 bytes, unsigned, pascals */

/* Built byte by byte, so that the result does not depend on the byte order of the CPU. */
static uint32_t read_le(const uint8_t *p, int bytes)
{
    uint32_t value = 0;
    int i;

    for (i = bytes - 1; i >= 0; i--) {
        value = (value << 8) | p[i];
    }
    return value;
}

/*
 * Writes a number given in hundredths as text with two decimals: 2017 gives "20.17", -5 gives
 * "-0.05". Only integer arithmetic: the frame already holds the temperature in hundredths of a
 * degree, so floating point is not needed, and it avoids the rounding and the formatting
 * differences of printing a float. The magnitude is taken as 64 bits so that the most negative
 * 32-bit number does not overflow when its sign is removed, and the sign is written by hand
 * because -5 / 100 is 0 in C, and "0" would lose the minus sign of "-0.05".
 *
 * Pressure uses it too: one pascal is 0.01 hPa, so the pressure in pascals IS the pressure in
 * hundredths of an hPa (101135 Pa gives "1011.35").
 */
static void format_hundredths(char *out, size_t size, int32_t value)
{
    int64_t magnitude = (value < 0) ? -(int64_t)value : value;

    snprintf(out, size, "%s%lld.%02lld", (value < 0) ? "-" : "",
             (long long)(magnitude / 100), (long long)(magnitude % 100));
}

int main(void)
{
    uint8_t frame[FRAME_SIZE];
    char temperature[16];
    char pressure[16];
    ssize_t n;
    int fd;

    fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror(DEVICE);
        return 1;
    }

    /* One read() returns the whole 14-byte frame, or fails. EIO means the driver found no good
     * frame (the node is off, or the frame was damaged). */
    n = read(fd, frame, sizeof(frame));
    close(fd);
    if (n != FRAME_SIZE) {
        if (n < 0) {
            perror("read");
        } else {
            fprintf(stderr, "read: got %zd bytes instead of %d\n", n, FRAME_SIZE);
        }
        return 1;
    }

    format_hundredths(temperature, sizeof(temperature), (int32_t)read_le(&frame[OFFSET_TEMP], 4));
    format_hundredths(pressure, sizeof(pressure), (int32_t)read_le(&frame[OFFSET_PRESSURE], 4));

    printf("seq=%u status=%u T=%s C P=%s hPa\n",
           (unsigned)read_le(&frame[OFFSET_SEQUENCE], 2),
           (unsigned)frame[OFFSET_STATUS], temperature, pressure);
    return 0;
}
