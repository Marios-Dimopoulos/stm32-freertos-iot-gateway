# ADR-0009: BMP280 module: normal mode, integer results, initialise again after every error

**Status:** Accepted
**Date:** 2026-10-08
**Phase:** 1

## Context

The Bosch BMP280 measures temperature and pressure and sits on I2C1 (address 0x76) behind
the I2C driver of ADR-0007. A module on top of that driver has to configure it, read
it, and turn the sensor's raw numbers into usable ones. The sensor outputs raw 20-bit
ADC counts; the true values come from a compensation formula that uses 12 factory
calibration constants stored in the chip.

The forces:
- the output is printed over USART2 without `printf` (a tiny formatter, no `malloc`) and
  later goes into a fixed-size frame for the SPI link, so fixed-size integers are easier
  than floats;
- one task owns the sensor and the bus (ADR-0007), so there is no need for a handle or a
  lock;
- the sensor can stop answering: it can be unplugged, or locked out of I2C by a falling
  edge on its CSB pin until its power is cycled (reproduced on the board, ADR-0007);
- the sensor is read once a second; low power does not matter (USB powered).

## Options considered

### Decision 1: how the sensor measures

#### Option A: normal mode
- How it works: the sensor measures continuously by itself and pauses between
  measurements; the task reads the latest result whenever it likes.
- Pros: simple; the IIR filter works best on a continuous stream.
- Cons: the sensor draws more current and warms very slightly.

#### Option B: forced mode
- How it works: the task triggers each measurement, waits for it to finish, and the
  sensor sleeps in between.
- Pros: lower power.
- Cons: a wait or a status poll after every trigger, more code, and nothing in this
  project needs the saving.

### Decision 2: the form of the results

#### Option A: integers (Bosch's 32-bit formula for temperature, 64-bit for pressure)
- Pros: temperature in hundredths of a degree and pressure in pascals are fixed-size
  integers, easy to format and to put in a frame; no float printing.
- Cons: the 64-bit division is a library call (`__aeabi_ldivmod`) on the Cortex-M4.

#### Option B: floating point
- Pros: simpler formulas to read; the chip has an FPU.
- Cons: floats must be printed and framed, and a task using the FPU makes the kernel save
  the FPU context (up to 51 words per context, see ADR-0005).

#### Option C: send the raw values and the calibration to the gateway
- Pros: no maths on the MCU.
- Cons: the gateway needs the calibration, the debug output on the MCU shows
  meaningless numbers, and nothing is gained for the work saved.

### Decision 3: what the sensor task does after an error

#### Option A: initialise again on the next period, after any error
- How it works: any failed init or read is counted and reported, and the next period runs
  `bmp280_init()` again (soft reset, calibration, configuration).
- Pros: a sensor that was power-cycled comes back with its default settings and is
  configured again, with no MCU reset; one rule for every cause.
- Cons: a short extra cost after a failure; it cannot fix a sensor locked out by CSB
  (only a power cycle does, and then this rule picks it up).

#### Option B: retry only the read
- Cons: after a power cycle the sensor is in sleep mode and would report "no data" forever.

#### Option C: report once and stop
- Cons: needs an MCU reset to recover; not acceptable for a node that runs unattended.

## Decision

- **Normal mode, integer results, initialise again after any error.**
- **Settings:** temperature and pressure oversampling x16, IIR filter 16, standby 0.5 ms
  (the settings of the course reference code). `config` is written before `ctrl_meas`,
  because writes to `config` can be ignored once the sensor is in normal mode (as I
  remember the datasheet); the values are built from named fields (0x10 and 0xB7).
- **Init order:** soft reset, a 10 ms `vTaskDelay`, chip ID (0x58), the 24 calibration
  bytes into a local buffer and only then into the static struct, `config`, `ctrl_meas`.
  A flag set after a complete init lets `bmp280_read()` assert that init succeeded.
- **Read:** one 6-byte burst from 0xF7 (the sensor freezes the six registers together),
  the two 20-bit raw values, and `0x80000` on either means no measurement yet.
- **The compensation** is two pure functions that take the calibration as an argument.
  The left shifts of terms that can be negative are written as multiplications (a left
  shift of a negative number is undefined in C).
- **Status codes:** OK, the I2C cause kept as it is (NACK, TIMEOUT, BUS), wrong chip ID,
  no data (also used when the pressure divisor would be zero, which only corrupt
  calibration could cause). Programming errors (a NULL pointer, a read before init) are
  assertions.
- **Deliberately left out:** a read-back of the two configuration registers (a wrong
  value would show in the measurements) and a sanity check of the calibration bytes (no
  status code for it, nothing needs one).

What decided Decision 3: the sensor is the part of the system most likely to fail
(unplugged wire, CSB glitch), and one rule that is correct for every cause is easier to
defend than a table of cases that nothing has shown to be needed.

## Consequences

- The sensor task and the future transmit task only see `bmp280_init()`, `bmp280_read()`
  and a status; they never touch I2C.
- A sensor that was disconnected or power-cycled recovers without resetting the MCU.
  Verified on the board by disconnecting the sensor completely and reconnecting it.
- The IIR filter makes the output smooth and slow to follow real changes (a finger takes
  a few seconds to show), and the reading is very stable.
- The sensor itself and the board warm up a little, so the temperature can read slightly
  above the room's.

### What was verified on the board
- Chip ID 0x58; the calibration bytes in the typical ranges; a reading of 23.64 C and
  101969 Pa, stable, and plausible for the room; NACK with the sensor disconnected or
  CSB at GND, and recovery after reconnection.
- The formulas, transcribed into Python with C's integer rules, reproduce the example
  values I remember from Bosch's datasheet (25.08 C and 100653 Pa).

### What is not verified, or not done
- **The register layouts and the codes are from my memory of the datasheet** (the field
  positions of `ctrl_meas` and `config`, the oversampling and filter codes, the 0x80000
  value, the reset behaviour, and the latching of the six data registers during a burst).
  They work on the board, but they have not been checked
  against the datasheet text, and the configuration registers were never read back.
- Whether x16 oversampling on the temperature improves anything (I vaguely remember that
  it does not for the pressure result) has not been checked.
- The example values in the Python check are from memory, and the C code itself has not
  been run on a PC.
- The `no data` path and the zero-divisor path have never been exercised on the board
  (the sensor task waits a full period after init, so the first read always finds data).
- The absolute accuracy of the readings is not known; "plausible" is the only claim.

### Follow-up
- Check the datasheet's register descriptions against the code once, and note the result
  here.
- How the readings reach the Phase 2 transmit task (a queue or a shared buffer) is still
  open (`architecture.md`).

## Interview angle

"The sensor gives raw ADC counts, so the module reads the factory calibration at init and
compensates with Bosch's integer algorithm: 32 bits for the temperature, 64 for the
pressure, as pure functions I can test without the sensor. The results are fixed-size
integers because I print without printf and will put them in a frame. The part I am most
careful about is failure: any error sends the task back to initialise the sensor again,
because a sensor that lost power comes back in sleep mode and would otherwise report no
data forever. I tested it by disconnecting the sensor while it ran, and I know what I
cannot fix from software, a sensor locked into SPI mode by CSB, which needs a power
cycle."
