# ADR-0010: SPI link to the gateway: the Pi as master, a fixed frame with CRC, a latest-frame queue and a re-armed DMA

**Status:** Accepted, and working end to end on the board (2026-10-14): the sensor task
builds a frame every second, the NSS interrupt wakes the transmit task, which loads the
newest frame and re-arms the DMA, and the Pi reads real frames with a valid CRC, one
transaction after another (see "Verified on the board" and "Found on the board"). Still
open: what produces the occasional extra NSS edges (a loose wire is suspected, never
measured), the staleness of the first read after an idle time, and everything about the
real signals (no logic analyzer yet).
**Date:** 2026-10-11
**Phase:** 2

## Context

The sensor node has to hand its readings to the Raspberry Pi Zero 2 W over SPI1
(PA4 NSS, PA5 SCK, PA6 MISO, PA7 MOSI), with DMA for the transfer (CLAUDE.md: bulk
transfers never by CPU polling; SPI clock at or below about 6 MHz because the logic
analyzer samples at 24 MHz). Phase 4 is a Linux `spi_driver` on the Pi with a Device
Tree overlay, so the Pi side is fixed by the plan.

SPI has properties that shape everything below (RM0383, chapter 20):
- only the master clocks, so only the master can start a transfer; a slave answers when
  it is clocked, and the byte it shifts out was loaded before the clock started;
- there is no acknowledge, no error detection and no flow control; an unplugged slave
  reads as all ones or all zeros;
- in slave mode the clock comes from the master and the `BR` bits do not affect the
  rate (p. 555), and the slave's data register must be ready before the first clock edge
  or before the end of the transfer in progress (p. 555);
- there is no underrun flag in SPI mode (`UDR` is "not used in SPI mode", p. 597), so a
  slave cannot tell that it was clocked with nothing new to send;
- the DMA is a second bus master that reads RAM by itself: a critical section or a mutex
  does not stop it.

The sensor produces one reading per second (ADR-0009). The Pi's custom Linux image does
not exist until Phase 3, and the logic analyzer is not purchased.

## Options considered

### Decision 1: who is the SPI master

#### Option A: the Pi is the master, the STM32 is the slave
- How it works: the Pi's SPI controller drives SCK and NSS; the STM32's SPI1 is a slave
  with hardware NSS.
- Pros: it is the only arrangement that fits Phase 4: a Linux `spi_driver` binds to a
  device behind the Pi's SPI controller, which is a master. The 6 MHz limit is enforced
  by the maximum frequency in the Pi's device node.
- Cons: the STM32 cannot decide when data leaves; a valid frame must always be ready.
  The ADR-0006 note "SPI1 prescaler /16" does not apply (the `BR` bits are irrelevant
  for a slave).

#### Option B: the STM32 is the master, the Pi is the slave
- Pros: the STM32 decides when to send; plain normal-mode DMA.
- Cons: as I understand it, mainline Linux support for the Pi's SPI controller as a slave
  is thin or absent (not verified), and the Phase 4 kernel-driver plan would have to be
  reinvented.

A build switch for both roles was not considered: it is generality nothing needs.

### Decision 2: what plays the master during Phase 2

#### Option A: the Pi Zero 2 W with the stock Raspberry Pi OS and `spidev`, as a test jig
- Pros: no cost; a real master with real wiring (3.3 V, common ground); a short Python
  script with `spidev` clocks frames out.
- Cons: not the final image. CLAUDE.md forbids the stock OS for the final deliverable;
  as a throwaway test jig it is acceptable, and it is flagged as temporary here.

#### Option B: the STM32 as master with MISO wired to MOSI (loopback)
- Cons: contradicts Decision 1; it would test DMA mechanics but not the real design.

#### Option C: wait for the Phase 3 image
- Cons: it blocks Phase 2 on Yocto, the largest unknown of the project.

### Decision 3: what one reading looks like on the wire

#### Option A: the raw struct (8 bytes: temperature, pressure)
- Cons: nothing detects a lost frame, corruption or an unplugged slave; struct padding
  and endianness are hidden risks.

#### Option B: text (a JSON line)
- Cons: 40 or more bytes, a variable length (the frame length is a promise between the
  two sides, which a variable length weakens), and parsing code on both sides.

#### Option C: a fixed 14-byte binary frame with marker, status, sequence and CRC-16
- Pros: fixed length; every cheap check the bus does not give: the marker shows a dead
  slave (`0xFF` or `0x00`) or a misaligned read, the sequence shows a repeat or missed
  frames, the CRC shows corruption and torn frames; the STM32 can also report a sensor
  failure through the same path.
- Cons: a little code on the STM32 (the CRC) and a layout that must be written down and
  kept identical on both sides.

### Decision 4: how the Pi learns that a new frame exists

#### Option A: the Pi polls, with the sequence number
- How it works: the Pi clocks one frame every N seconds, whenever it likes. The STM32
  always keeps its latest frame armed. The sequence number tells the Pi whether the
  frame is new and how many were skipped.
- Pros: no extra wire; works at any poll rate.
- Cons: latency up to one poll period; wasted transfers when nothing is new.

#### Option B: a "data ready" GPIO from the STM32 to the Pi
- Pros: no wasted transfers, lower latency.
- Cons: one more wire and a GPIO interrupt in the Phase 4 driver, for a benefit that is
  irrelevant at one reading per second.

### Decision 5: how a finished frame reaches the DMA without tearing

The sensor task writes frames; the DMA reads them at times the Pi chooses. A transfer of
14 bytes at 4 MHz lasts about 28 microseconds and building a frame takes a few, so the
overlap probability is small (my estimate: about 3 x 10^-5 per reading, roughly once in
8 hours at one poll per second), but it is the kind of bug that cannot be reproduced.

#### Option A: one buffer, written only "when idle"
- Cons: the check and the write are separate, so a transfer can start in between. Not
  safe by construction.

#### Option B: two buffers and a swap inside the interrupt handler
- Pros: the lowest latency (microseconds); no extra task.
- Cons: more logic in an ISR and a task/ISR handshake that must be written with care.

#### Option C: a one-slot overwriting queue (the latest frame) and a priority-2 transmit task
- How it works: the sensor task builds the frame in a local array and hands the finished
  frame as the latest frame (`xQueueOverwrite`, a statically created queue of length 1). An
  interrupt wakes the transmit task, which stops the DMA, copies the latest frame
  (`xQueuePeek`) into the DMA buffer, and arms the DMA again. The DMA buffer is written
  only by that task and only while the DMA is stopped.
- Pros: the two actors never use the same memory at the same time, by construction. The
  latest-frame queue is shared by two tasks, so the queue's own critical section protects it (the
  copy in `xQueueGenericSend` and `xQueuePeek` is inside `taskENTER_CRITICAL()`,
  `queue.c` lines 971 to 985 and 1908 to 1921 of the V11.3.1 source in the repository).
  The logic lives in a task, not in an ISR.
- Cons: more latency than Option B (interrupt, context switch, copy, arm: my estimate is
  tens of microseconds, not measured). That becomes a requirement on the Pi: a minimum
  gap between transactions.

### Decision 6: DMA mode and the re-arm trigger

#### Option A: normal mode, re-armed after every transaction, triggered by NSS rising
- Pros: every transaction starts from a known state (count 14, buffer start), so one odd
  transaction cannot misalign the next ones. NSS rising happens whether the Pi clocked 14
  bytes or fewer. The remaining count (`DMA_SxNDTR`) says how many bytes were clocked, a
  free frame-length check.
- Cons: the re-arm must finish before the next NSS falling edge (the minimum gap).

#### Option B: circular mode
- Pros: no re-arm in the normal case.
- Cons: after one transaction of a different length every later frame is misaligned and
  nothing resynchronises it (the "data wiping" risk in `plan.md`).

#### Option C: normal mode re-armed from the DMA transfer-complete interrupt
- Cons: in non-circular mode the stream stops by itself at count 0, and `TCIF` is also
  set when software stops the stream early (RM0383 p. 178 and 179), so it cannot tell a
  complete transaction from an interrupted one, and it does not fire at all if the Pi
  clocks fewer bytes than the count.

### Decision 7: what the SPI data register holds when the stream is re-armed

*Added 2026-10-14, after a failure found on the board that the first version of
Decision 6 did not foresee.*

**The rule that causes it.** `DR` is always one byte ahead: `TXE` goes to 1 when a byte
moves from `DR` into the shifter (the start of that byte) and the DMA then loads the
next one. With n bytes clocked, bytes 0 to n have been loaded, so `DR` ends up **empty
only if n is exactly 14**. After a short transaction, an aborted one, or one that was
restarted in the middle, `DR` still holds an unsent byte and `TXE = 0`.

**The failure.** The first version of the re-arm only enabled the stream and relied on
the SPI raising a request (`TXE = 1`) to have byte 0 loaded. With a leftover byte there
is no request: the leftover goes out first, the frame follows one byte late, the last
byte (the CRC high byte) is loaded but never shifted and becomes the next leftover. The
node stays shifted until reset. Measured in a 50-read test (reads 1.1 s apart): reads 1
to 19 correct; read 20 hit by a re-arm 3 bytes into the transaction (an NSS rising edge
in the middle of it); reads 21 to 49 all shifted by one byte, and in every one the junk
first byte was the CRC high byte of the frame before (checked for four frames).

#### Option A: the CPU writes byte 0 into `DR` at every re-arm; the DMA sends bytes 1 to 13
- How it works: stop the stream, copy the frame, `SPI1->DR = frame[ 0 ]`, then enable
  the stream with `M0AR = &frame[ 1 ]` and `NDTR = FRAME_SIZE - 1`.
- Pros: the state after a re-arm is the same whatever happened before (byte 0 in `DR`,
  `TXE = 0`, the stream waiting for byte 1). It matches the manual's rule that the
  data register must be ready before the master's first clock edge (p. 555). One
  register write.
- Cons: it relies on a write to `DR` replacing a byte that is still there; that is how
  the manual reads (one transmit buffer). **Seen on the board** (2026-10-14): after a
  deliberate short read, the next full read was a correct frame.

#### Option B: toggle `TXDMAEN` after enabling the stream, to force a request
- Cons: helps only when `DR` is empty. With a leftover byte there it would still go out
  first. Does not solve the problem.

#### Option C: disable and enable the SPI (`SPE`) at every re-arm
- Cons: I do not know that it empties the transmit buffer, and it opens a window in
  which the Pi may clock a disabled slave.

#### Option D: detect the situation and live with it (count the shifted frames)
- Cons: the node would stay wrong until reset. A robustness gap, not a fix.

**Chosen: Option A.** What swung it is that it removes the dependence on a hidden state
instead of trying to predict it. Whatever disturbance leaves a byte in `DR` (a short
read, a glitch, a restart), the next re-arm replaces it. A disturbed transaction is
still corrupted (its bytes had already left the chip), but the next one is correct; the
CRC and the sequence number are how the Pi finds the damaged one.

**The order matters:** stop, copy, write `DR`, then enable the stream. Enabled earlier,
with `TXE = 1`, the stream would serve a request at once, load byte 1 into `DR`, and
the CPU's byte 0 would be written over it.

**What this does not explain.** What produced the extra NSS rising edge in read 20 (and
the earlier episode of 28 edges for one read) is not known; I cannot see NSS. A check of
the NSS level in the handler (a rising edge with NSS still low is a glitch: count it and
do not wake the task) is the next cheap instrument, and the logic analyzer is the right
tool.

### Decision 8: a rising edge on NSS counts only if NSS is high when the handler looks

*Added 2026-10-15, after the gap, clock and sensor-unplug tests.*

**The problem.** EXTI has no glitch filter. A spike on NSS in the middle of a transaction
is a rising edge like any other: it woke the transmit task, which re-armed the DMA and
restarted the frame from byte 0. Seen on the board more than once. The cleanest example:
a bad frame at 1 MHz that was its **first 7 bytes twice** (`A5 00 56 01 D6 08 00 A5 00
56 01 D6 08 00`), perfectly aligned, which suggests that the SPI peripheral kept its
place and only the re-arm broke the frame. Rate: about 2 bad frames in about 2000 reads
with the wires still, and more when the wiring is touched (the whole set-up is a
breadboard and long jumpers).

#### Option A: read the NSS level in the handler (`GPIOA->IDR` bit 4)
- How it works: clear the pending bit, then read the pin. High: a real end of a
  transaction, count it and wake the task. Low: a spike, count it in `ulNssGlitchCount`
  and do nothing else.
- Pros: one `if`; no new task or object; the new counter is an instrument that tells how
  often the line spikes without a logic analyzer.
- Cons: a spike still there when the handler looks (a few microseconds after the edge) is
  not caught. A real end could be ignored only if the Pi started the next transaction
  within those microseconds; the Pi's gap is at least tens of microseconds even back to
  back from Python (gap test).

#### Option B: read NSS in the transmit task
- Cons: the task runs tens of microseconds later, when a spike is long gone and a real
  end may already be followed by the next start. Worse than A on both sides.

#### Option C: leave the firmware, fix the wiring only
- Pros: free; it is the real cure for the noise.
- Cons: tells nothing about how often it happens, and the firmware stays exposed to any
  spike.

**Chosen: Option A**, and the wiring improvements in parallel. It does not remove the
noise and does not help against a real loss of contact (the SPI peripheral loses the
frame, and the CRC finds it); it stops the firmware from turning a spike into a restart
of a good frame.

**Verified on the board (2026-10-15).**

| Run | Result |
|---|---|
| one read after a reset | rising 1, glitch 0 |
| 3000 reads, 10 ms apart, **wires still** | **3000 good**; rising +3000 (3001 in all), glitch **7**: seven spikes on NSS in 3000 transactions (about 0.2%), all ignored, **no damaged frame** |
| 3000 reads, 10 ms apart, **the NSS wire touched by hand** | good 2792, shifted 0, **bad 208**; rising +3046 (**46 above the 3000 real ends**), glitch +3017 |
| `gap` (down to back to back) and `speed` (up to 6 MHz) | 100% good: the check never ignored a real end of a transaction |

Reading of it:

- The filter does what it was written for: of about 3060 spikes while the wire was
  being touched, **3017 were ignored** and 46 got through (NSS was still high when the
  handler looked: the contact had really opened for longer).
- With the wires still, 7 spikes in 3000 transactions would each have restarted a frame
  before this change (the earlier rate was about 2 bad in 2000, the same order of
  magnitude): **zero** bad frames now. This is consistent, not proven: the same run was
  not made on the old firmware.
- The 208 bad frames of the touched run are mostly **not** restarts (46 at most). The
  rest is corruption of the bits themselves by a contact that opens and closes while the
  frame is clocked: nothing in the firmware can recover that frame (the SPI peripheral
  loses it too), and the CRC finds it. Each bad frame was followed by good ones.
- Still open: what the spike is. A few hundred nanoseconds on a long jumper over a
  breadboard is the suspect; it needs the logic analyzer to be shown.

## Decision

- **Roles:** Option A, the Pi is the master and the STM32 is the slave. What swung it is
  Phase 4: a kernel `spi_driver` sits behind a master controller. SPI mode 0 on both
  sides, written explicitly in the code.
- **Test master:** Option A, the stock-OS Pi with `spidev`, as a temporary jig, with the
  SPI clock set to 4 MHz or less (the hard limit is 6 MHz).
- **Frame:** Option C, 14 bytes, little-endian, packed byte by byte (never by copying a
  struct):

  | Bytes | Field | Meaning |
  |---|---|---|
  | 0 | marker | `0xA5` |
  | 1 | status | a `frame_status_t` value (named in `frame.h`): 0 = OK, 1 = no reading yet, 2 to 6 = the cause of a sensor failure (NACK, TIMEOUT, BUS, ID, NO_DATA) |
  | 2-3 | sequence | `uint16`, +1 for every frame the sensor task builds, wraps to 0 |
  | 4-7 | temperature | `int32`, hundredths of a degree Celsius |
  | 8-11 | pressure | `uint32`, pascals |
  | 12-13 | CRC | CRC-16 over bytes 0 to 11, low byte first |

  The CRC is CRC-16/CCITT-FALSE: polynomial `0x1021`, start value `0xFFFF`, bits not
  reflected, no final XOR, computed in software. **Test vectors**, computed with a
  Python script: the body `A5 00 01 00 3C 09 00 00 51 8E 01 00` (status OK, sequence 1,
  23.64 C, 101969 Pa) gives `0x4D5E`, sent as `5E 4D`; the same body with the temperature
  byte `09` changed to `08` gives `0xF53F`; the placeholder body
  `A5 01 00 00 00 00 00 00 00 00 00 00` gives `0x9610`, sent as `10 96`. The STM32's C
  code produced exactly what the script predicted: a frame with a real reading and
  sequence 2 (`A5 00 02 00 DC 07 00 00 C3 8A 01 00 17 49`, checked in the debugger) and
  the placeholder, which the Pi also read over the wire.
- **Collection:** Option A, polling by the Pi with the sequence number; no extra wire.
  The data-ready line stays a known upgrade path.
- **Hand-off:** Option C, a latest-frame queue plus a transmit task at priority 2. What swung it is that
  it is safe by construction, keeps the ISR minimal, and is the design the task table
  already assumed. Option B stays valid if the latency ever matters.
- **DMA:** Option A, normal mode on DMA2 stream 3, channel 3 (SPI1_TX, Table 28 of
  RM0383), re-armed by the transmit task after the EXTI line 4 interrupt (NSS rising).
  There is no DMA interrupt. This is a deliberate deviation from `plan.md`, which wakes
  the task from the DMA's transfer-complete interrupt.
- **The stream is stopped and checked before EVERY reload (a robustness rule).** The
  code that reloads the stream first clears `EN` and waits, with a bounded loop, until
  `EN` reads 0; only then does it clear the flags, reload `NDTR`, and enable the stream.
  It never assumes that the master clocked exactly 14 bytes.
  - *Why.* The master is not ours. NSS rising means "the Pi ended the transaction", not
    "14 bytes were clocked". After a short transaction (a script aborted, a glitch, a
    bug) the stream is still enabled with `NDTR > 0`, and `NDTR`, `PAR` and `M0AR` are
    write-protected while `EN = 1` (RM0383, section 9.5, and p. 172 for `M0AR`). A reload
    without the stop would be silently ignored: the next transaction would start in the
    middle of the buffer, and since nothing clears `EN` the system would stay wrong until
    reset.
  - *Cost.* In the normal case the stream has already stopped by itself (it copies the
    14th byte into `DR` about one byte time before the transaction ends), so the loop
    does not run: two register accesses.
  - *Rejected.* Checking only at start-up, or trusting `NDTR` to be 0 at NSS rising: one
    misbehaving transaction would break every later one, permanently and without any
    error being reported.
  - *By-product.* `NDTR` read at the NSS edge says how many bytes were clocked, so short
    transactions can be counted.
- **Order, and what is set once.** Once, in `prvDmaConfigure()`: the DMA2 clock, `PAR`,
  `M0AR` (= `&ucDmaFrame[ 1 ]`, see Decision 7) and the `CR` fields (channel, priority,
  memory increment, direction), with `EN` left off. For every frame, in
  `prvDmaReloadAndEnable()`: stop and wait, copy the frame into `ucDmaFrame`, **write
  byte 0 into `SPI1->DR`**, clear the stream's flags, `NDTR = FRAME_SIZE - 1`, `EN`. The
  frame buffer is written only after the stop and before the enable. `M0AR` is a **base
  address**: it keeps its value during and after the transfer (it read back the address
  it had been given, with one byte already copied and after a complete transaction; the
  DMA walks through the buffer with its own pointer), so it is a setting like `PAR` and
  is written once.
- **A rising edge on NSS counts only if NSS is high when the handler looks (Decision 8).**
  The handler clears the pending bit, reads `GPIOA->IDR` bit 4, and wakes the transmit
  task only when the pin is high; otherwise it counts a glitch and returns.
- **The CPU sends byte 0 of every frame itself (Decision 7).** Option A of Decision 7:
  the re-arm no longer depends on what `DR` held, so a short, aborted or restarted
  transaction cannot leave the node shifted by one byte.

## Consequences

- **Makes easier:** nothing about the bus can fail silently: a dead slave, a lost frame,
  a repeated frame, a corrupted frame and a wrong-length transaction are each detectable
  by the Pi, or by the STM32 through `NDTR`.
- **Makes harder:**
  - the Pi side must respect a **minimum gap** between the end of one transaction and the
    start of the next. I will write 1 ms into the Pi script and the Phase 4 driver; the
    real re-arm time still has to be measured;
  - the layout and the CRC variant must be identical in the STM32 code, the test script
    and the Phase 4 driver (the test vector exists for that);
  - the EXTI interrupt and its handler are at level 5 or a larger number (ADR-0004),
    because the handler calls a `FromISR` function.
- **Forecloses:** the Pi cannot be told "measure now": the STM32 measures on its own
  1 s period and the Pi only collects.
- **The Pi gets the frame prepared after the previous transaction, however long ago
  that was.** The DMA buffer is refilled right after each transaction and nothing
  refreshes it in between. With the Pi polling at 1 Hz the frame is about one period
  older than the newest one in the queue; **after an idle time it is as old as the idle
  time**: in the burst test the first read of a run returned a frame about a minute old
  (sequence 272, then 331 on the next read). The sequence number shows it, nothing else
  does. Accepted for now, because a poller that reads every period never sees it. What
  would remove it: the Pi discards the first frame of a run (read twice), or the node
  refreshes the buffer between transactions, which needs a safe way to know that no
  transaction is running. To decide when the Phase 4 driver is written.
- **A read with nothing loaded returns the last byte again (observed).** After the
  stream has finished, or beyond the 14th byte, the slave repeats the last byte it was
  given: the CRC high byte (`0x96` for the placeholder). A Pi that polls before the
  re-arm therefore reads 14 times the same byte, and the marker rejects it (`0xA5`
  expected). The marker is not enough on its own, since the last byte could be `0xA5`
  for some frame; the CRC covers that case.
- **Follow-up work:**
  - done: the sensor task builds the frame and keeps the latest-frame queue filled;
    `spi.c` (SPI1 slave, DMA2 stream 3, EXTI line 4); the transmit task, woken by the
    NSS interrupt;
  - to do: a public function in `spi.c` that stops the stream, copies a frame into
    `ucDmaFrame` and reloads it; `sensor_task_latest_frame()`; the transmit task calling
    both after each wake-up; its stack size measured (ADR-0005);
  - `docs/architecture.md`, the task table (priority 2 stays), and the README list of
    ADRs need the new entry when this is finished;
  - buy the logic analyzer; nothing about the signals can be called verified without it.

### Verified on the board (2026-10-13)

Setup: the Pi Zero 2 W with Raspberry Pi OS Lite (64-bit) and Python `spidev`, mode 0,
1 MHz. Wires: Pi header pins 24 (CE0), 23 (SCLK), 21 (MISO), 19 (MOSI), 25 (GND) to
PA4, PA5, PA6, PA7 and GND. The Pi's own loopback (MOSI to MISO) was tested first and
returned what was sent.

| What | Result |
|---|---|
| First 14-byte read | `A5 01 00 00 00 00 00 00 00 00 00 00 10 96`: the placeholder, CRC `0x9610` as computed by the script |
| Alternate function 5 for PA4 to PA7, wiring, mode 0 | work |
| DMA2 stream 3, channel 3 (Table 28) | works: the bytes came out in order |
| After the transaction: `CR` | `0x06020440`: only `EN` was cleared (by the hardware); the settings stay |
| After the transaction: `PAR`, `NDTR`, `M0AR` | unchanged, `0`, unchanged (a base address) |
| After the transaction: `DMA2->LISR` | `0x0C400000`: `TCIF3` (complete), `HTIF3` (half) and `FEIF3` (FIFO error). The cause of `FEIF3` is not understood; the manual says it does not stop the stream and loses no data; the reload clears it |
| Second read, with no re-arm | 14 times `0x96` (the last byte repeated) |
| 16-byte read right after a reset | the frame, then `96 96` |
| Firmware | the sensor line kept printing on the terminal during all of it |
| `M0AR` | a base address: `0x200000B0` (= `&ucDmaFrame`) with byte 0 already copied (`NDTR` 13), and again after a full transaction (`NDTR` 0); it is written once, with `PAR` |
| **EXTI line 4 on PA4 while PA4 is the SPI's NSS** (rising edge, NVIC level 6) | **works**: `ulNssRisingCount` went from 0 to 1 for one read made from the Pi. The CPU was halted by the debugger during the read: the DMA and the SPI still moved the 14 bytes, and the interrupt was served as soon as the CPU ran again |
| **Transmit task woken by the NSS interrupt** (task notification, priority 2) | **works**: `ulWakeUps` followed `ulNssRisingCount` one for one |
| **The priority-2 transmit task preempting the sensor task, and the I2C driver under preemption** | **soak test**: 3000 reads from the Pi, 10 ms apart, about 35 s, with the node running freely. Both counters ended equal (3003 in total since reset, the 3 being earlier single reads), no `BMP280 error` line on the terminal, the Pi's script finished. By my estimate (not measured) about a hundred of those interrupts fell inside an I2C transfer of the sensor task, so this is real evidence, not a proof that a rare overlap cannot hurt |

### Measured on the board (2026-10-15): gap, clock, sensor unplugged, reset

Tests run with `spi_frame_test.py` on the Pi (200 reads per line), with the firmware
before Decision 8:

| Test | Result |
|---|---|
| **minimum gap**: pause between reads 100, 10, 5, 2, 1, 0.5, 0.2, 0 ms | 200 good in every line but one: 1 ms had 1 bad (a bit-mix, `80 09 A9 40 ...`). No failure down to back-to-back reads. So the re-arm finishes within the time Python needs between two `xfer2` calls (probably 100 to 300 µs, **not measured**: the real minimum needs the analyzer). The "1 ms" of this ADR is a safe upper bound |
| **SPI clock**: 100 kHz, 500 kHz, 1, 2, 4, 6 MHz | all 200 good, apart from 1 bad at 1 MHz (the frame that was its first 7 bytes twice: a restart by a spike, see Decision 8). The slave keeps up to the 6 MHz limit (the controller rounds the requested clock) |
| **first read of each run** | stale: sequence 111 then 269, 294 then 340 (the frame loaded at the end of the previous run). The staleness consequence, seen again |
| **sensor unplugged** (SDA only) during polling | `status=NACK` with T=0 P=0, then `NO_READING`, then `OK` again; the sequence kept moving. Touching the wires while doing it produced bad SPI frames that recovered by themselves |
| **reset of the STM32** during polling | as expected: the sequence restarts and the first frame is the placeholder |
| overall | about 2 bad frames in about 2000 reads with the wires still, each followed by good ones |

### Verified on the board (2026-10-18): runs of the script aborted with Ctrl+C

Hypothesis: after any aborted run, the next reads are valid, because every real end of
a transaction re-arms the stream and primes `DR` (Decision 7).

| Test | Result |
|---|---|
| 10 runs of `poll --count 100 --period 0.2 --verbose`, each stopped with Ctrl+C at an arbitrary moment | **no shifted and no bad frame** in any run. Runs 2 to 10 each started right after an aborted run, so their first reads (stale, as expected) were also valid |
| then `poll --count 5 --period 1.0 --verbose` | 5 good frames |

Limit: a Ctrl+C in Python lands between two `xfer2` calls (one call is the whole 14-byte
transaction in the kernel), so this tests restarting the script (the SPI device closed
and opened again) and not a transaction cut in the middle. That case is covered by the
short-read test of 2026-10-14.

### Found on the board (2026-10-14): the re-arm without priming `DR`

| What | Result |
|---|---|
| after a reset, one 14-byte read, then `NDTR` | 13: the first re-arm served its request |
| second read | a correct real frame (seq 10, 21.15 C, 1017.16 hPa, CRC valid) |
| 50 reads, 1.1 s apart, no reset | reads 0 to 19 correct (the sequence `+1`, and `+2` when a sensor period fell between two reads); read 20 corrupted; reads 21 to 49 **all shifted by one byte** with the last byte missing: the failure of Decision 7 |
| a single read, after a reset, in the same run of tests | **28** rising edges on NSS counted for one 14-byte read, and a frame with its first 4 bytes right and the rest wrong. The next tries (reads of 2, 7 and 14 bytes, reset between) counted exactly 1. Cause not known: a contact, or noise on the NSS wire, suspected; not shown |

Option A of Decision 7 was written after these results. Its verification follows.

### Verified on the board (2026-10-14): Option A of Decision 7, the CPU writes byte 0

| What | Result |
|---|---|
| after a reset: `NDTR`, `M0AR` | 13, and the address of `ucDmaFrame` plus 1 |
| first read | the placeholder: so `DR` may be written while `SPE` is 0 |
| `NDTR` after the first read | 13 |
| a deliberate **short read** (7 bytes) | `A5 00 3D 00 58 08 00`: the first 7 bytes of a real frame (seq 61) |
| the next full 14-byte read | a **correct frame** (seq 116, CRC valid): the short read did not leave the node shifted. This is exactly the case that used to flip it |
| `NDTR` after each read | 13 |
| 50 reads, 1.1 s apart | **47 good, 0 shifted, 3 bad.** The 3 bad ones (reads 35, 40 and 47) happened while the NSS wire was being **moved by hand on purpose**. In every case the next read was good again: the node recovered by itself |
| `NDTR` at the end | 13 |

Two things follow from the burst:

- **Moving the NSS wire corrupted frames** (3 of 50 reads, and only while it was being
  touched). That supports the suspicion that the earlier extra NSS edges were a contact
  or noise problem on that wire, not firmware. It is still a suspicion: nothing was
  measured on the pin. The bad frames were not shifted, they were mixes of bits (for
  example `A5 00 80 1E 20 0A 50 0A 50 07 80 07 80 15`), as a wire that connects and
  disconnects would give.
- **The first read after an idle time is stale.** In that burst, read 0 had sequence 272
  and read 1 had 331: the first one was the frame loaded at the end of the **previous**
  transaction, about a minute earlier. The sequence number shows it, but nothing else
  does. See the consequences.

### Not verified (from memory, or open until tested on the board)

- **What produces extra rising edges on NSS** during a transaction (read 20 of the first
  50-read test, and the episode of 28 edges). Moving the wire by hand reproduces
  corrupted frames, which points at the contact or at noise, but nothing was measured on
  the pin; the logic analyzer would show it. The check of the NSS level in the handler
  (Decision 8) hides their effect, but it does not say what they are.

- **`OVR` handling.** In transmit-only DMA use the received bytes are never read, so
  `OVR` is set (p. 571); whether it must be cleared before re-arming (read `DR`, then
  `SR`, p. 573) is not known. The `SR` value read after the first transaction (`0x0002`)
  is not reliable: the debugger's Memory window reads a block that includes `SPI1->DR`,
  and reading `DR` clears `RXNE`. A single-address read (a Watch cast) is needed.
- **What a byte shifted halfway does when NSS rises early**: not found in the manual;
  it decides whether the SPI must be disabled and enabled again on every re-arm.
- **The timing estimates** (re-arm in tens of microseconds, torn-frame probability) are
  mine, not measured.
- **`crc_ccitt()` in the Linux kernel** (helpful for Phase 4): from memory.
- **The claim that mainline Linux cannot be an SPI slave on the Pi's controller**
  (Option B of Decision 1): from memory.
- **Everything about the real signals:** no logic analyzer exists yet.

## Interview angle

"On SPI the master owns the clock, so the slave can only answer when it is clocked, and
nothing on the bus tells either side that something went wrong. I put the safety into the
frame: a marker, a sequence number and a CRC, because SPI has no ACK and no error flag.
The DMA is a second bus master that reads RAM on its own, so a critical section cannot
protect the buffer it reads: I used a one-slot latest-frame queue between the tasks and a transmit
task that re-arms the DMA only while it is stopped, so the two never touch the same memory
at the same time. I re-arm on the chip-select rising edge, not on the DMA's
transfer-complete flag, because that flag cannot tell a complete transfer from an
interrupted one. And I stop and check the stream before every reload, because the master
is not mine: a short transaction would leave the stream enabled, its registers are
write-protected while it runs, and the reload would be silently ignored."
