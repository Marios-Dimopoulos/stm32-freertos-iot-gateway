# ADR-0019: The stale first read is handled in the driver by reading twice

**Status:** Accepted
**Date:** 2026-10-29
**Phase:** 4

## Context

The node loads the newest frame into its output buffer right after every SPI transaction
(ADR-0010). So a transfer does not return the current frame: it returns the frame that was loaded
after the previous transfer, however long ago that was, or the placeholder frame (`seq=0`,
`status=1`, zeros) if there was no previous transfer since the node started.

This was known from Phase 2 and measured through the kernel driver in step 4.4. Reads at 4.7 s,
90.3 s, 123.1 s, 138.6 s and 148.3 s after the boot returned `seq` 97, 97, 183, 217 and 232 while
the newest frame at those moments was about 97, 184, 217 and 232. After a node restart, the first
read returned the placeholder and the next one `seq=36`.

For a poller that reads once a second the frame is about one period old, which is harmless. After
a long idle or a node restart it is minutes old, or not a reading at all.

## Options considered

### Option A: the driver reads twice and keeps the second frame
- How it works: every `read()` does one transfer and throws it away, waits the minimum gap, and
  does a second transfer. The first transfer only matters for its side effect: the node reloads its
  buffer when it ends. The second returns that fresh frame.
- Pros: no firmware change; one place (the driver) for every reader; the result is at most one
  sensor period old.
- Cons: about 0.4 ms more per `read()`; the node's behaviour stays odd; a node that has just
  started can still return the placeholder (the program sees `status`).

### Option B: change the firmware to refresh the buffer
- How it works: the node reloads its output buffer whenever the sensor task makes a frame, when
  no transaction is running, so one transfer always returns a fresh frame.
- Pros: one transfer per read; the node is correct by itself.
- Cons: a firmware change that needs its own Phase 2 testing; a race to handle (the buffer must not
  change while the master is reading it); the DMA design of ADR-0010 is carefully tuned.

### Option C: do nothing in the driver and tell the programs
- How it works: the driver returns what the node gives; programs use `seq` and `status` to judge.
- Pros: simplest.
- Cons: every reader has to know the trap; a program that forgets reads old data without any error.

## Decision

Option A. The swing factor is that the driver is the one place all readers go through, and the fix
is a few lines in one function; B would reopen the tested firmware for a problem the driver can
absorb.

## Consequences

- Tested on the board (the build with this change): a read, then 30 s of idle, then a read gave `seq`
  194 and 224 (difference 30, as for a fresh frame; before the change the second read returned
  about the frame of the first); after a reset of the Nucleo and 3 s of waiting, the first read gave
  `seq=3`, `status=0`; 8 readers at once, 10 reads each, no failure. The 500 sequential reads were
  not repeated for this change.
- A read costs two transfers plus the gap (about 0.4 ms). The driver does not decode `status`, so a
  program must still check it: within the first second after a node start the frame can be the
  placeholder.
- If the cost matters later (many readers, a short period), option B is still open.

## Interview angle

"The node loads the next frame after each transaction, so a read returns the previous one. I
measured it from the kernel driver, then made every read do a throw-away transfer first. It costs
a fraction of a millisecond, needs no firmware change, and every program gets a fresh frame."
