# ADR-0021: MQTT message, topic, publish rule, error handling and broker access

**Status:** Accepted (nothing is built yet)
**Date:** 2026-10-31
**Phase:** 4

## Context

ADR-0020 put the broker (Mosquitto) on the Pi and made the publisher a C program with libmosquitto.
Five smaller choices were left before writing it: the message format, the topic name, when to publish,
what to do when something goes wrong, and who may reach the broker.

Facts that decide them:
- A `read()` of `/dev/stm32_sensor_node` returns the newest checked 14-byte frame (ADR-0018, ADR-0019).
  The frame holds temperature in hundredths of a degree (`int32`) and pressure in pascals (`uint32`), so
  JSON text can be built from integers, without floating point.
- The node makes a frame about every 0.988 s (its clock is 1.1% fast, ADR-0006). `seq` goes up by one
  per frame, also while the sensor fails.
- A frame with `status` other than 0 has zeros for temperature and pressure (`status` 1 is the
  placeholder after a node start; 2 to 6 are sensor failures).
- The Pi has one open port (22, key-only login) and no clock, so TLS certificates cannot be checked.

## Options considered and decisions

### Message format
- JSON text, one message per frame (chosen).
- One topic per value: two messages per frame, so temperature and pressure of one moment are not tied
  together.
- The raw 14 bytes: every subscriber needs the frame layout and the CRC.

Chosen: `{"seq":97,"status":0,"temp_c":20.17,"press_hpa":1011.35}`. It keeps both values of one frame
together, carries `seq` and `status`, and any tool can read it.

### Topic
- `bmp280` (flat): breaks with a second source.
- `gateway/stm32/bmp280` (chosen): gateway publishes, the source is the STM32 board, the sensor is the
  BMP280. A second board or sensor gets its own name.
- One topic per value: conflicts with the one-message decision.

### When to publish
- Every 1.000 s: the Pi's 1.000 s and the node's 0.988 s drift, so about once every 83 s a frame is
  never read and `seq` jumps by 2, which looks like loss.
- Read every 0.2 s and publish only when `seq` changed (chosen): every frame is published once, a jump in
  `seq` is a real problem, and the cost is one variable and about five reads a second (0.4 ms each).
- Every 5 or 10 s: throws away data the node makes.

### Errors
- Publish only `status 0` and log the rest: subscribers cannot tell a broken sensor from a silent node.
- Publish every valid frame, with the numbers left out when `status` is not 0, and only log `-EIO`
  (chosen): `{"seq":98,"status":2}` uses the failure cause the firmware sends and shows no fake
  0.00 C. After `-EIO` nothing is published, and a subscriber sees that `seq` stops.
- The same plus an availability topic with MQTT Last Will: the usual production design, more machinery;
  it can be added later without changing the data topic.

### Broker access
- Localhost only, reached from the laptop through an SSH tunnel (chosen): no new open port and no new
  secret; the data does not cross the Wi-Fi in clear text.
- On the Wi-Fi address with a password: a second open port and another secret in every image, and without
  TLS the password and data are in clear text.
- On the Wi-Fi address without a password: anyone on the network can read and publish.

### Connection settings (decided before writing the publisher, 2026-10-31)
- **QoS 0** (at most once). QoS 1 adds duplicates and state; it protects only a message that is in
  flight, and I believe (not checked in the library) that nothing is queued while the connection is
  down. QoS 2 is a four-step handshake for a value that the next message replaces. On a loopback link,
  with one replaceable reading per second and a `seq` that shows gaps, QoS 0 is enough. Changing it
  is a one-number change.
- **No retain.** A retained message has no age: the frame has no timestamp and the Pi has no clock to
  add one, and without a Last Will (not in this design) a dead publisher would leave its last reading
  looking current. A new subscriber waits up to about one second for the first message. Retain with a
  timestamp becomes possible once the Pi has a clock (new ADR).
- **Client ID `gateway-publisher`, clean session.** A fixed name makes a second copy of the program
  kick the first (the broker disconnects the older client with the same ID), which is a useful alarm.
  A random ID would let two copies publish silently; an ID built from the hostname or the MAC is not
  needed with one gateway. Clean session: the broker keeps no state for this client (right for QoS 0
  and a client that subscribes to nothing).
- **Keepalive 60 s.** My publisher sends about one message per second, which already counts as
  activity; pings only happen when it is quiet (for example the node is off and every read gives
  `-EIO`). On loopback a connection cannot half-die the way a Wi-Fi link can, so a faster value gains
  nothing. A frozen publisher would be dropped by the broker after about 90 s.

## Decision

All the choices marked above. The swing factors: keep both values of a frame together and keep `seq`
meaningful; use the failure information the firmware already sends; and add no port and no secret.

## Consequences

- The publisher needs the decoding of `frame.h` (little-endian fields, `int32` temperature) and integer
  formatting of the two numbers.
- A subscriber cannot tell "node off" from "publisher dead": both are silence. The Last Will option is the
  way out, if needed.
- Only programs on the Pi, and my laptop through SSH, can subscribe. Other machines cannot until the Pi
  has a clock and the broker can use TLS (new ADR then).
- The image needs the Mosquitto client tools (`mosquitto_sub`) to check messages on the Pi itself.
- Not tested yet; the numbers above (0.2 s, five reads a second) are plans, not measurements.

## Interview angle

"I designed the message first: one JSON object per frame, so temperature and pressure of one moment stay
together, with the sequence number and status so a subscriber can see losses and sensor failures. The
broker is only reachable from the Pi, and I reach it through my SSH tunnel, so MQTT added no port and no
secret to the gateway."
