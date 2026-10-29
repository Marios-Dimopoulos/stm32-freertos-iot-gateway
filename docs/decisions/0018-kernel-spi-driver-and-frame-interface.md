# ADR-0018: A kernel SPI driver for the STM32 node, with a raw-frame character device

**Status:** Accepted (the driver attaches and reads a frame since step 4.4; the character device is step 4.5 and is not written yet)
**Date:** 2026-10-29
**Phase:** 4

## Context

The gateway has to read a 14-byte frame from the STM32 over SPI and pass it on (MQTT, step 4.6).
Phase 3 already reads valid frames with `spi-frame-test`, a C program that uses `spidev` (the
generic kernel driver that gives user space raw access to one SPI chip select). So reading
works without any driver of my own. The project, however, asks for a kernel driver (`CLAUDE.md`,
Phase 4), and there are real reasons to want one:

- With `spidev` every program must know the frame layout and the CRC, and nothing in the kernel
  can enforce the minimum pause between transfers. Phase 2 only showed that the node was ready
  within the 154 us gap that back-to-back reads from Python leave (an upper bound, the re-arm time
  itself was not measured); a C program or a kernel driver may read much faster, so the driver has
  to enforce a delay (ADR-0010).
- Any program that may open `/dev/spidev0.0` can send anything to the chip.
- A damaged frame (wrong marker or CRC) reaches the program, which has to check it.

Questions to settle before writing more code: kernel driver or `spidev`; what a program gets
from the driver; where the frame is decoded; how the driver is attached to the device; what it
does when the node does not answer.

## Options considered

### Option A: keep `spidev` and the user-space program
- How it works: `/dev/spidev0.0` stays, and the program that publishes to MQTT does the
  transfer, the checks and the pauses itself.
- Pros: nothing to write in the kernel; already works on the image.
- Cons: the protocol, the pause and the checks live in every program that uses the chip; no
  single place enforces the pause; it does not meet the Phase 4 goal.

### Option B: a kernel SPI driver with a character device that returns the raw 14-byte frame
- How it works: `stm32_node` is an `spi_driver`. A device-tree node tells the kernel that an
  STM32 node is on chip select 0, and the kernel calls the driver's `probe`. A `read()` on
  `/dev/stm32_sensor_node` does one SPI transfer and returns the 14 bytes, only if the marker and
  the CRC are right.
- Pros: one place for the transfer, the checks and the pause; programs get a checked frame;
  the kernel keeps the chip away from other programs; it is the usual Linux structure.
- Cons: kernel code is harder to write and debug, and a bug can crash the whole system.

### Option C: a kernel driver that decodes and exposes values (text in a file, or IIO)
- How it works: the driver decodes temperature and pressure and offers them as text, or through
  IIO (the kernel's framework for sensors).
- Pros: standard interface; any tool can read it without knowing the frame.
- Cons: more kernel code (formatting, no floating point in the kernel); the format is fixed in
  the kernel; there is one reader only, so a standard interface has no second user.

## Decision

Option B. The swing factor is that the pause and the frame check are the two things that only
make sense in one place, and a driver is that place; decoding does not need the hardware, so
it stays out of the kernel (a bug in a program does not crash the system).

Details:

- **Attached through the device tree.** The overlay `stm32-node.dts` disables the `spidev@0`
  node and adds `stm32@0` with `compatible = "mdimopoulos,stm32-sensor-node"`, `reg = <0>` and
  `spi-max-frequency = <1000000>`. Chip select 0 can have one driver, so `/dev/spidev0.0` is gone
  and `spi-frame-test` does not run on this image.
- **One blocking transfer of 14 bytes** (`spi_sync_transfer()`), buffers from `devm_kzalloc()`.
  A blocking call is on purpose: 14 bytes at 1 MHz are not a bulk transfer, and `probe` and
  `read()` may sleep. The SPI core and the controller driver choose by themselves whether to use
  DMA.
- **The driver checks the frame, the program decodes it.** The driver checks the marker `0xA5` and
  the CRC-16/CCITT-FALSE (my own function, the same as in the firmware); the temperature and
  the pressure are decoded by the program that reads the frame.
- **`probe` does not fail when the node does not answer.** It prints a warning and returns 0, so
  a node that is powered later still works; only a failed memory allocation fails it.
- **The mode** is not set by the driver: the device tree gives mode 0, and the `mode 4` that
  `probe` prints is the `SPI_CS_HIGH` flag that the SPI core adds when the controller selects the
  chip with a GPIO descriptor (the line is still active low).

## Consequences

- Easier: programs get a frame that is already checked; the pause between transfers and the
  access to the chip are in one place.
- Harder: the interface is the 14-byte layout of `frame.h`, so a change of the layout means
  changing the driver and every reader; kernel bugs are more costly.
- Measured in step 4.4 (5 frames, STM32 running, all valid): each read returns the frame that the
  node loaded right after the previous read, so a read after a long idle is old (seq 97 at
  boot, and the same frame 86 s later). This is the "stale first read" of ADR-0010, now seen from
  the driver. Decision for step 4.5, as a new ADR: read twice and keep the second, or change the
  firmware to refresh the buffer.
- Follow-up in step 4.5: the character device (`open`, `read`), the minimum gap, the stale-read
  policy. Not verified yet: the clock on the wire (no logic analyzer capture of the driver) and
  that `speed_hz = 0` gives the 1 MHz of the device tree.

## Interview angle

"I used `spidev` first to prove the link, then wrote a kernel driver, so that the transfer, the
frame check and the pause between transfers are in one place and every program gets a checked
frame. I kept the decoding out of the kernel on purpose: the kernel does what only it can do,
and what the bytes mean is the program's job."
