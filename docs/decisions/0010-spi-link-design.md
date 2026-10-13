# ADR-0010: SPI link to the gateway: the Pi as master, a fixed frame with CRC, a latest-frame queue and a re-armed DMA

**Status:** Accepted. Built and checked on the board: the frame and its CRC, the
latest-frame queue, the SPI1 slave configuration, the DMA arming, and one real
transaction with the Pi (2026-10-13, see "Verified on the board"). Not built yet: the NSS
interrupt, the transmit task, and the re-arm after each transaction.
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
  `EN` reads 0; only then does it clear the flags, rewrite `M0AR` and `NDTR`, and enable
  the stream. It never assumes that the master clocked exactly 14 bytes.
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
- **Order, and what is set once.** Once, in `prvDmaConfigure()`: the DMA2 clock, `PAR`
  and the `CR` fields (channel, priority, memory increment, direction), with `EN` left
  off. For every frame, in `prvDmaReloadAndEnable()`: stop and wait, clear the stream's
  flags, `M0AR`, `NDTR`, `EN`. The frame buffer (`ucDmaFrame`) is written only after the
  stop and before the reload: stop, copy, reload. `M0AR` is a **base address**: it keeps
  its value during and after the transfer (it read back `0x200000B0`, the address of
  `ucDmaFrame`, after a complete transaction; the DMA walks through the buffer with its
  own pointer), so writing it for every frame is redundant but harmless. It could move
  into `prvDmaConfigure()` next to `PAR`.

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
- **The Pi gets the frame prepared after the previous transaction.** The DMA buffer is
  refilled right after each transaction and nothing refreshes it in between, so with the
  Pi polling at 1 Hz a frame can be up to about one period older than the newest one in
  the latest-frame queue. The sequence number still moves normally. Accepted: refreshing
  the buffer between transactions would need a safe way to know that no transaction is
  running, and nothing has shown that it is needed.
- **A read with nothing loaded returns the last byte again (observed).** After the
  stream has finished, or beyond the 14th byte, the slave repeats the last byte it was
  given: the CRC high byte (`0x96` for the placeholder). A Pi that polls before the
  re-arm therefore reads 14 times the same byte, and the marker rejects it (`0xA5`
  expected). The marker is not enough on its own, since the last byte could be `0xA5`
  for some frame; the CRC covers that case.
- **Follow-up work:**
  - the sensor task builds the frame and keeps the latest-frame queue filled (the next step);
  - `spi_slave` module: SPI1 slave, DMA2 stream 3, EXTI4, the transmit task;
  - `docs/architecture.md`, the task table (priority 2 stays), and the README list of
    ADRs need the new entry when this is built;
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

### Not verified (from memory, or open until tested on the board)

- **EXTI on PA4 while it is SPI NSS.** The manual says the pin's input stage is active in
  alternate-function mode (section 8.3.11), which suggests it works; EXTI itself was not
  read and nothing was tested.
- **`OVR` handling.** In transmit-only DMA use the received bytes are never read, so
  `OVR` is set (p. 571); whether it must be cleared before re-arming (read `DR`, then
  `SR`, p. 573) is not known. The `SR` value read after the first transaction (`0x0002`)
  is not reliable: the debugger's Memory window reads a block that includes `SPI1->DR`,
  and reading `DR` clears `RXNE`. A single-address read (a Watch cast) is needed.
- **What a byte shifted halfway does when NSS rises early**: not found in the manual;
  it decides whether the SPI must be disabled and enabled again on every re-arm.
- **The timing estimates** (re-arm in tens of microseconds, torn-frame probability) are
  mine, not measured.
- **That the I2C driver survives being preempted** for tens of microseconds by the
  transmit task. ADR-0007 was designed for preemption, but nothing has preempted it for
  real yet.
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
