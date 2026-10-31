# ADR-0020: The MQTT broker runs on the Pi, and the publisher is a C program with libmosquitto

**Status:** Accepted (nothing is built yet; the smaller decisions of step 4.6 are open)
**Date:** 2026-10-31
**Phase:** 4

## Context

The gateway reads a checked 14-byte frame from `/dev/stm32_sensor_node` (ADR-0018, ADR-0019). Step 4.6
has to send the data on. MQTT (a message protocol over TCP: a program publishes to a topic on a broker,
and other programs subscribe to that topic) is the protocol the project plan names.

Two questions have to be settled before writing anything: where the broker runs, and what the publisher
program is written in.

Constraints and facts:
- No Mosquitto recipe exists in `openembedded-core`, `meta-yocto` or `meta-raspberrypi`; it is in
  `meta-openembedded`, a fifth repository. Found when the repo was cloned (2026-10-31): the recipe
  (`mosquitto_2.1.2.bb`) is in the layer `meta-networking`, not `meta-oe` as first assumed, and that
  layer depends on `meta-oe` and `meta-python`, so three layers of the repo are added. `meta-python`
  puts no Python in the image. The recipe's default `PACKAGECONFIG` is `ssl websockets`, so the broker
  would pull in OpenSSL and libwebsockets; the plan is to remove both for this recipe only.
- The image has no Python, and ADR-0017 reduced it from 142 MB to 37 MB on purpose.
- The Pi has no battery clock (the boot log showed the year 2018), so anything that checks certificate
  dates (TLS to a cloud broker) would fail.
- The Wi-Fi link has dropped before (the power-save suspicion, still untested), and the Pi's address
  changes with DHCP.
- No extra hardware is needed for either choice (budget).

## Options considered

### Broker: option A, on the Pi
- How it works: Mosquitto is added to the image from `meta-openembedded`. The publisher connects to
  `localhost:1883`.
- Pros: the gateway is self-contained; publishing does not depend on Wi-Fi, so a drop only affects
  subscribers; it is how a real gateway is built.
- Cons: a fifth repository to clone, pin and add to `bblayers.conf`; a bigger image; a broker to
  configure and secure (I believe Mosquitto 2 listens on localhost only without a configuration, so a
  listener and a password are needed for other machines; another secret in the image); a second open
  port on the Wi-Fi.

### Broker: option B, on another machine
- How it works: the Pi only runs the publisher; the broker runs on my laptop.
- Pros: nothing to run or secure on the Pi.
- Cons: nothing is collected unless the laptop is on and has a known address; the publisher fails when
  Wi-Fi drops; it still needs a client library from the same repository, or a client written by hand.

### Broker: option C, a public or cloud broker
- Cons: needs internet and, in practice, TLS, which fails without a clock; sends the data outside my
  network. Not considered further.

### Publisher: option 1, C with libmosquitto
- How it works: the client library is built by the same `mosquitto` recipe. The program is a recipe in
  `meta-gateway`, like `spi-frame-test`.
- Pros: same language as the rest of the gateway; small; the library handles keepalive and reconnect.
- Cons: more verbose than Python; the default build probably pulls in OpenSSL, to be checked.

### Publisher: option 2, C with a hand-written MQTT client
- Pros: no library; teaches the protocol.
- Cons: I would own keepalive, reconnect and the error cases; a lot of code that is not the point of the
  project.

### Publisher: option 3, Python with paho-mqtt
- Pros: the shortest code.
- Cons: Python and a second layer (`meta-python`) in an image that has none, which undoes part of
  ADR-0017.

## Decision

Broker option A and publisher option 1. The swing factors: with the broker on the Pi, the part that
collects the data does not depend on Wi-Fi or on my laptop; and a C program with the library keeps
the image free of Python while leaving the hard parts of the protocol (keepalive, reconnect) to tested
code.

## Consequences

- Easier: the publisher talks to `localhost`; the gateway works alone; one language on the Pi.
- Harder: a fifth repository to record (branch and commit, as for the other four) and to add to
  `bblayers.conf`; the broker needs a configuration and, for remote subscribers, a password that stays
  out of the repo like the Wi-Fi key and the root hash; port 1883 on the Wi-Fi has to be limited or
  protected.
- To check when building: what the `mosquitto` recipe pulls in (OpenSSL, other libraries), and how much
  it adds to the 37 MB image.
- Open for later steps (each its own decision): the message format, the topic name, the publish
  period, what the publisher does on `-EIO` and on `status` other than 0, and how broker access is
  restricted.

## Interview angle

"The data collector lives on the gateway, so the broker runs there too and the publisher only talks to
localhost; if Wi-Fi drops, publishing is not affected. I wrote the publisher in C with the Mosquitto
client library, to keep Python out of a minimal image."
