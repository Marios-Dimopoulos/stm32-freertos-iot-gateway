/*
 * gateway-publisher: reads frames from the STM32 sensor node through /dev/stm32_sensor_node and
 * sends them to the MQTT broker on the gateway (ADR-0020, ADR-0021).
 *
 * Step 4.6c, piece 1: read ONE frame and decode it.
 * Piece 2: build the MQTT message of ADR-0021 as JSON text.
 * Piece 3 (this file): connect to the broker, publish that one message and exit. The loop that
 * repeats this comes in piece 4.
 *
 * The program does NOT check the marker or the CRC. The kernel driver does that and makes read()
 * fail with EIO for a bad frame (ADR-0018: the driver checks, the program decodes), so a
 * frame that read() returns is already a good one. Checking again would be code for no new
 * protection.
 *
 * The frame layout is in firmware/stm32-sensor-node/Src/frame.h and docs/decisions/0010-spi-link-
 * design.md. All numbers in it are little-endian (the low byte comes first).
 */
#include <errno.h>
#include <fcntl.h>
/* Only the client part of the library. The umbrella header <mosquitto.h> of Mosquitto 2.1 also
 * includes the broker and plugin headers, and through them the cJSON header, which this program
 * does not need (a syntax check on the PC failed on it). */
#include <mosquitto/libmosquitto.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

/* The connection settings are the ones decided in ADR-0020 and ADR-0021. The broker is on this
 * same Pi and listens on the loopback address only, so the host is "localhost". Clean session
 * (see broker_connect()): the broker keeps no state for this client. QoS 0 and no retain: the next
 * reading replaces this one within a second, and a retained reading would have no age. The
 * client ID is fixed on purpose: a second copy of this program would make the broker disconnect the
 * first, which is an alarm and not a silent duplicate. */
#define BROKER_HOST     "localhost"
#define BROKER_PORT     1883
#define KEEPALIVE_S     60
#define CLIENT_ID       "gateway-publisher"
#define TOPIC           "gateway/stm32/bmp280"
#define QOS             0
#define RETAIN          false

/* How long to wait for the library to say the message has left (see on_publish()), and how often
 * to look. */
#define PUBLISH_WAIT_MS 2000
#define POLL_MS         10

/* Set by on_publish(), which runs in the library's network thread, and read by main(). An atomic
 * variable makes that safe without a lock. */
static atomic_int published;

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

/*
 * Called by the library when a message has been sent. For QoS 0 that means the message was passed
 * to the operating system for delivery to the broker; there is no confirmation from the broker
 * (that is what QoS 0 is). It is enough to know that the library is done with it, so that
 * main() does not close the connection before the bytes have left.
 */
static void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
    (void)mosq;
    (void)obj;
    (void)mid;
    atomic_store(&published, 1);
}

/*
 * Connects to the broker and starts the library's network thread. Returns the connection, or NULL
 * after printing why it failed.
 *
 * mosquitto_connect() returns when the TCP connection exists; the broker's answer to the connect
 * request arrives later and is handled by the network thread. The library refuses to publish
 * only if the TCP connection does not exist, so a message can be published right after this.
 *
 * The error text of the library (mosquitto_strerror) is in a separate library that is not in the
 * image, so a failure to connect prints the system error, and the other calls print their code.
 */
static struct mosquitto *broker_connect(void)
{
    struct mosquitto *mosq;
    int rc;

    mosquitto_lib_init();

    /* true: clean session, the broker keeps nothing for this client between connections. */
    mosq = mosquitto_new(CLIENT_ID, true, NULL);
    if (mosq == NULL) {
        fprintf(stderr, "mosquitto_new failed\n");
        goto fail_lib;
    }
    mosquitto_publish_callback_set(mosq, on_publish);

    rc = mosquitto_connect(mosq, BROKER_HOST, BROKER_PORT, KEEPALIVE_S);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mosquitto_connect to %s:%d failed: %s\n", BROKER_HOST, BROKER_PORT,
                (rc == MOSQ_ERR_ERRNO) ? strerror(errno) : "invalid parameters");
        goto fail_client;
    }

    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mosquitto_loop_start failed (%d)\n", rc);
        goto fail_client;
    }
    return mosq;

fail_client:
    mosquitto_destroy(mosq);
fail_lib:
    mosquitto_lib_cleanup();
    return NULL;
}

/* Undoes broker_connect(). loop_stop waits for the network thread to end; it is called after
 * disconnect, which makes the thread leave its loop by itself, so it does not need to be forced
 * (false). */
static void broker_close(struct mosquitto *mosq)
{
    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, false);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
}

int main(void)
{
    uint8_t frame[FRAME_SIZE];
    char json[JSON_MAX];
    struct mosquitto *mosq;
    ssize_t n;
    int length;
    int waited_ms;
    int rc;
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

    length = format_json(json, sizeof(json),
                         (unsigned)read_le(&frame[OFFSET_SEQUENCE], 2),
                         (unsigned)frame[OFFSET_STATUS],
                         (int32_t)read_le(&frame[OFFSET_TEMP], 4),
                         (int32_t)read_le(&frame[OFFSET_PRESSURE], 4));
    if (length < 0) {
        fprintf(stderr, "the message does not fit in %d bytes\n", JSON_MAX);
        return 1;
    }
    printf("%s\n", json);

    mosq = broker_connect();
    if (mosq == NULL) {
        return 1;
    }

    rc = mosquitto_publish(mosq, NULL, TOPIC, length, json, QOS, RETAIN);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mosquitto_publish failed (%d)\n", rc);
    } else {
        /* publish only queues the message; the network thread sends it. Wait until on_publish()
         * says it has gone, because closing the connection first would drop it. */
        for (waited_ms = 0; waited_ms < PUBLISH_WAIT_MS && !atomic_load(&published);
             waited_ms += POLL_MS) {
            usleep(POLL_MS * 1000);
        }
        if (!atomic_load(&published)) {
            fprintf(stderr, "the message was not sent within %d ms\n", PUBLISH_WAIT_MS);
            rc = -1;
        }
    }

    broker_close(mosq);
    return (rc == MOSQ_ERR_SUCCESS) ? 0 : 1;
}
