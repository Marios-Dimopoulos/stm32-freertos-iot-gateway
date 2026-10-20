# ADR-0007: I2C1 driver: register-oriented polling API with self-recovery

**Status:** Accepted
**Date:** 2026-10-06
**Phase:** 1

## Context

The BMP280 sensor sits on I2C1 (PB8 = SCL, PB9 = SDA). The driver is written at
register level, without ST's HAL or library (see ADR-0008). The STM32F4 I2C peripheral is an older design that leaves the
software to drive each phase of a transfer and to react to flags at precise moments,
especially when receiving: the master must clear ACK and request STOP before the
9th clock of the last byte (a window of about 90 microseconds at 100 kHz), and the
sequence differs for 1, 2, and 3 or more bytes.

The system is a FreeRTOS one, with a higher-priority SPI transmit task (Phase 2) that
can preempt the sensor task at any moment. A hang in the driver would
freeze a task, and a silently wrong byte would give a wrong measurement, because I2C
has no checksum.

Forces:
- the rules of the project: every blocking loop must be flagged; no code that is not
  needed; every decision explainable;
- the sensor is read once a second, so a transfer is about 1 ms of waiting per second;
- the hardware is a breadboard with short wires and a breakout board, which usually
  carries its own pull-ups.

## Options considered

### Option A: a generic byte API, as in the course driver (`write(address, bytes)`, `read(address, bytes)`)
- How it works: the caller builds a register read as write(register) then read(n).
- Pros: small and familiar.
- Cons: the repeated START and the time-critical receive sequence end up in every
  caller, or the register read is split in two transactions with a STOP in between.
  The course version also returns nothing on failure, takes the address already
  shifted, and its last-byte handling in a multi-byte read depends on timing it
  does not protect.

### Option B: a register-oriented API with distinct status codes
- How it works: `i2c_write_reg(device, register, data, length)` and
  `i2c_read_reg(device, register, buffer, length)` do the whole transaction,
  including the repeated START and the receive sequences, and return a status.
- Pros: the delicate code exists once; the sensor module only says "read 6 bytes
  from 0xF7"; failures are reported.
- Cons: tied to the "device with a register map" model (true for the BMP280 and
  most sensors).

### Option C: interrupt-driven or DMA-driven transfers
- How it works: a state machine in the I2C event interrupt, or DMA for the data.
- Pros: the task does not busy-wait.
- Cons: much more code on this peripheral, for about 1 ms of waiting once a second.
  Not justified for 1, 6 or 24 bytes.

## Decision

Option B, with polling (Option C rejected), and these choices:

- **The caller passes the 7-bit address** (0x76); the driver adds the read/write bit.
- **Distinct errors:** `I2C_OK`, `I2C_ERR_NACK` (nobody acknowledged), `I2C_ERR_TIMEOUT`,
  `I2C_ERR_BUS`. The caller can print the cause. A programming error (a bad address,
  a length of 2 or 0, calling before the scheduler starts) is an **assertion**, not a
  return code.
- **Waits bounded in real time by the RTOS tick** (about 10 ms each, a hundred times
  longer than a phase needs), not by loop-pass counts: the driver runs in a task, after
  the scheduler has started, and the unit does not depend on the clock. The flag is
  tested before the clock, so a wait that was preempted for a long time but whose
  hardware phase finished still succeeds. The waits are never inside a critical
  section (the tick does not advance there).
- **The receive-deadline operations run in a short critical section.** For 1 byte:
  clear ACK, clear ADDR (read SR1, SR2), set STOP. For 3 or more: at byte N-2, clear
  ACK, read DR, set STOP. A few microseconds; everything else, including all waits,
  stays outside. Transmitting needs no protection: the peripheral holds SCL low when
  software is late.
- **N = 2 is refused** (assertion): nothing needs it, and a third sequence would be
  code that cannot be justified or tested. Read three bytes instead.
- **One task owns the bus** (the sensor task), as a documented rule. No mutex
  (`configUSE_MUTEXES` is 0).
- **Self-recovery:** after a timeout, a bus error, or a bus that stays busy, the driver
  pulses `SWRST` and sets the peripheral up again, so the next call starts clean; the
  failing call still returns its error. Recoveries are counted
  (`i2c_recovery_count`).
- **100 kHz standard mode**; `CR2.FREQ`, `CCR` and `TRISE` computed from the real APB1
  clock (42 MHz: 42, 210, 43).
- **Pins** open-drain with the internal pull-up on as a backup (about 40 kOhm, from
  memory), `MODER` written last so the pins never drive the bus push-pull.

## Consequences

- The sensor module and the sensor task call two functions and handle a status code;
  they never touch I2C registers.
- Every call is a blocking busy-wait (flagged as such in the code), about 1 ms for a
  6-byte read, in the sensor task. A higher-priority task can preempt it without harm:
  the only deadline-sensitive operations are inside the critical section.
- A long preemption counts toward the timeout, so a preemption of more than about 10 ms
  during a wait that is truly stuck is reported as a timeout, not as a flag that arrived
  late.
- The driver is unusable from `main()` and from interrupts (the assertion stops it).

### What was verified on the board
- The BMP280 acknowledges at 0x76; a register write (soft reset) returns OK.
- A 1-byte read returns the chip ID 0x58; the 6-byte burst returns `80 00 00 80 00 00`
  right after a reset; a 24-byte read of the trimming registers succeeds.
- Touching SDA to ground gives `TIMEOUT`, and the driver is back to `OK` after
  releasing it, without resetting the board. A sensor that does not answer gives
  `NACK` without hanging.
- Retested on 2026-10-08 with the sensor task running (it initialises the sensor
  again after every error): disconnecting the sensor completely gives `NACK`, and
  everything comes back by itself when it is reconnected. Grounding SDA this time
  gave `NACK`, not `TIMEOUT` as on 2026-10-06; the difference was not investigated
  (it may depend on when in a transfer the line is grounded). The CSB lock-out was
  reproduced again: CSB to GND while running gives `NACK`, and putting it back to
  3.3 V still gives `NACK` until the USB cable is unplugged.

- **Seen on the wire (2026-10-20, Pico logic analyzer, `docs/captures/`):** every read of
  the sensor task is `START, 0xEC (address 0x76 + write) ACK, 0xF7 ACK, repeated START,
  0xED (address 0x76 + read) ACK, six data bytes with ACK on the first five and NACK on
  the last, STOP`, 834 us long, once per second. PulseView's I2C decoder and a decoder
  written in Python give the same bytes. SCL: typical period 9.89 us (about 101.1 kHz,
  nominal 100 kHz; see ADR-0006 for the clock error), low time about 5.3 us. A few clock
  periods are longer (11 to 12 us); where in the transfer they occur was not examined.
  A possible cause is the peripheral holding SCL low while the driver services a byte.
- The 3-or-more-bytes receive sequence (the part with the deadline) shows the intended
  result on the wire: ACK on every byte of the 6-byte burst but the last, NACK on the
  last, then STOP.

### What is not verified, or not done
- **No bus clear.** `SWRST` resets only my side. A sensor that holds SDA low would keep
  the bus busy after the reset, and the calls would keep returning `TIMEOUT`. Freeing it
  needs up to nine SCL pulses by hand; not written, because it has not been needed.
- **A power cycle of the sensor can be required.** After the SDA test, the sensor
  answered `NACK` until the board was unplugged and plugged again. The most likely
  cause is `CSB` brushing ground, but that day it was not measured. Later, with the
  BMP280 module, the same symptom was **reproduced on purpose**: with the sensor
  powered, moving `CSB` from 3.3 V to GND gave `NACK`, and moving it back to 3.3 V
  still gave `NACK` until the USB cable was unplugged and plugged in again. So a
  falling edge on `CSB` locks the sensor into SPI mode until the next power-up (the
  datasheet, section "Interface selection", says the same; confirmed on this
  breakout). No driver can fix this from
  the I2C side, so the sensor task has to report it and retry, and `CSB` must be wired
  firmly to 3.3 V (a loose wire or a glitch on it is enough).
- The pull-ups of the breakout board were never measured; the internal ones are weak
  (rise time about 1.7 us against the 1 us allowed, with an assumed 50 pF), so external
  4.7 kOhm resistors are the fix if the bus turns flaky.
- The recovery counter was not confirmed to have gone up during the SDA test.

### Users
- The BMP280 module (`bmp280.c`) is the only user; it retries when the sensor does not
  answer.

## Interview angle

"The F4's I2C needs software to react within a byte time when receiving, so I put the
whole transaction behind one register-level function and protected only the three
operations that have a deadline with a short critical section; everything that waits
stays outside, with timeouts measured in RTOS ticks, so a task that gets preempted
doesn't produce false errors. Faults are reported with distinct codes, and after a
timeout the driver resets the peripheral itself so the next call starts clean. I also
know its limits: a sensor stuck on the bus needs a bus clear or a power cycle, which a
peripheral reset can't provide."
