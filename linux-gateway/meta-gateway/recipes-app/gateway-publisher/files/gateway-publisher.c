/*
 * gateway-publisher: reads frames from the STM32 sensor node through /dev/stm32_sensor_node and
 * sends them to the MQTT broker on the gateway (ADR-0020, ADR-0021).
 *
 * Step 4.6c, piece 1: read ONE frame and decode it.
 * Piece 2: build the MQTT message of ADR-0021 as JSON text.
 * Piece 3: connect to the broker and publish that message.
 * Piece 4 (this file): do it in a loop. Every POLL_PERIOD_MS the program reads a frame and, if
 * its sequence number is new, publishes it. It runs until it gets SIGINT or SIGTERM.
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
#include <signal.h>
#include <stdbool.h>
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

/* How often to ask the driver for a frame (ADR-0021). The node makes a frame about every 0.988 s,
 * so five reads a second find every new frame within 0.2 s. A read costs two SPI transfers (about
 * 0.4 ms, ADR-0019), so the load is small. */
#define POLL_PERIOD_MS  200

/* The value of last_sequence before anything was published. A sequence number has 16 bits, so -1
 * is never a real one, and the first frame that is read is always new. */
#define NO_SEQUENCE     (-1)

/* Set by the signal handler, read by the loop in main(). sig_atomic_t is the one type that the C
 * standard allows a signal handler to write safely. */
static volatile sig_atomic_t stop_requested;

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

/* Runs when SIGINT (Ctrl+C) or SIGTERM (what an init script's `stop` sends) arrives. It only sets a
 * flag, because almost nothing else is safe to do inside a signal handler; the loop in main()
 * sees the flag and leaves by itself, so the connection is closed properly. */
static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
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
    struct sigaction action;
    int last_sequence = NO_SEQUENCE;
    bool read_failing = false;
    bool publish_failing = false;
    unsigned sequence;
    ssize_t n;
    int length;
    int rc;
    int fd;

    /* The device is opened once. Every read() on it does its own transfers and returns the
     * newest frame, so the file position means nothing. */
    fd = open(DEVICE, O_RDONLY);
    if (fd < 0) {
        perror(DEVICE);
        return 1;
    }

    mosq = broker_connect();
    if (mosq == NULL) {
        close(fd);
        return 1;
    }

    /* No SA_RESTART flag, so a signal also interrupts the sleep below and the program stops at
     * once and not up to 0.2 s later. */
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    while (!stop_requested) {
        /* One read() returns the whole 14-byte frame, or fails. EIO means the driver found no
         * good frame (the node is off, or the frame was damaged). */
        n = read(fd, frame, sizeof(frame));

        if (n < 0 && errno == EINTR) {
            continue;                   /* a signal: the loop condition decides what to do */
        }

        if (n != FRAME_SIZE) {
            /* Only logged, and only when it starts: a node that is off would otherwise print
             * five lines a second. Nothing is published, so a subscriber sees that the
             * sequence stops (ADR-0021). */
            if (!read_failing) {
                fprintf(stderr, "read failed: %s\n", (n < 0) ? strerror(errno) : "short frame");
                read_failing = true;
            }
        } else {
            if (read_failing) {
                fprintf(stderr, "read works again\n");
                read_failing = false;
            }

            sequence = (unsigned)read_le(&frame[OFFSET_SEQUENCE], 2);
            if ((int)sequence != last_sequence) {
                length = format_json(json, sizeof(json), sequence,
                                     (unsigned)frame[OFFSET_STATUS],
                                     (int32_t)read_le(&frame[OFFSET_TEMP], 4),
                                     (int32_t)read_le(&frame[OFFSET_PRESSURE], 4));
                if (length < 0) {
                    /* Cannot happen: JSON_MAX is larger than the longest message. */
                    fprintf(stderr, "the message does not fit in %d bytes\n", JSON_MAX);
                    break;
                }

                rc = mosquitto_publish(mosq, NULL, TOPIC, length, json, QOS, RETAIN);
                if (rc == MOSQ_ERR_SUCCESS) {
                    /* Only a published frame counts as done. After a failed publish the same
                     * sequence (or a newer one) is tried again at the next round. */
                    last_sequence = (int)sequence;
                    if (publish_failing) {
                        fprintf(stderr, "publish works again\n");
                        publish_failing = false;
                    }
                } else if (!publish_failing) {
                    /* The library's network thread reconnects by itself when the broker comes
                     * back; until then publish keeps failing, and this is printed once. */
                    fprintf(stderr, "mosquitto_publish failed (%d)\n", rc);
                    publish_failing = true;
                }
            }
        }

        usleep(POLL_PERIOD_MS * 1000);
    }

    broker_close(mosq);
    close(fd);
    return 0;
}
