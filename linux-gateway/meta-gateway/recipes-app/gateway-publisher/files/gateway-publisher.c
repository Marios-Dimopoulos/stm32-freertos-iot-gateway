/*
 * gateway-publisher: reads frames from the STM32 sensor node through /dev/stm32_sensor_node and
 * sends them to the MQTT broker on the gateway (ADR-0020, ADR-0021).
 *
 * Step 4.6c, piece 1: read ONE frame and decode it.
 * Piece 2 (this file): build the MQTT message of ADR-0021 as JSON text and print it. There is
 * no MQTT yet, so the program can be tested on the board alone.
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

/* The status that means "this frame has a real reading" (frame_status_t OK in frame.h). Any other
 * status is the placeholder after a node start (1) or a sensor failure (2 to 6), and then the
 * temperature and pressure of the frame are zeros that must not be shown as readings. */
#define STATUS_OK       0

/* The longest message has 71 characters: status 0 with the largest sequence and the most extreme
 * numbers, {"seq":65535,"status":0,"temp_c":-21474836.48,"press_hpa":-21474836.48} (measured
 * with a test on the PC). A status other than 0 hides the numbers, so it is always shorter. This
 * leaves room, and format_json() checks the fit anyway. */
#define JSON_MAX        96

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

/*
 * Builds the message of ADR-0021 into `out` and returns its length, or -1 if it does not fit in
 * `size` bytes (a message that is cut must never be sent).
 *
 *   status 0:       {"seq":97,"status":0,"temp_c":20.17,"press_hpa":1011.35}
 *   other status:   {"seq":98,"status":2}
 *
 * With a status other than 0 the numbers are left out on purpose, so that a subscriber never
 * sees the zeros of a failed reading as a temperature of 0.00 C. The numbers are written without
 * quotes, so that a subscriber reads them as numbers and not as text. The text is built by hand
 * with snprintf: it has two fixed shapes and only numbers in it, so no string needs escaping and
 * a JSON library would add code for nothing.
 */
static int format_json(char *out, size_t size, unsigned sequence, unsigned status,
                       int32_t temperature, int32_t pressure)
{
    char temperature_text[16];
    char pressure_text[16];
    int length;

    if (status == STATUS_OK) {
        format_hundredths(temperature_text, sizeof(temperature_text), temperature);
        format_hundredths(pressure_text, sizeof(pressure_text), pressure);
        length = snprintf(out, size,
                          "{\"seq\":%u,\"status\":%u,\"temp_c\":%s,\"press_hpa\":%s}",
                          sequence, status, temperature_text, pressure_text);
    } else {
        length = snprintf(out, size, "{\"seq\":%u,\"status\":%u}", sequence, status);
    }

    /* snprintf returns the length the text WOULD have had; a value of `size` or more means it
     * was cut. */
    if (length < 0 || (size_t)length >= size) {
        return -1;
    }
    return length;
}

int main(void)
{
    uint8_t frame[FRAME_SIZE];
    char json[JSON_MAX];
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

    if (format_json(json, sizeof(json),
                    (unsigned)read_le(&frame[OFFSET_SEQUENCE], 2),
                    (unsigned)frame[OFFSET_STATUS],
                    (int32_t)read_le(&frame[OFFSET_TEMP], 4),
                    (int32_t)read_le(&frame[OFFSET_PRESSURE], 4)) < 0) {
        fprintf(stderr, "the message does not fit in %d bytes\n", JSON_MAX);
        return 1;
    }

    printf("%s\n", json);
    return 0;
}
