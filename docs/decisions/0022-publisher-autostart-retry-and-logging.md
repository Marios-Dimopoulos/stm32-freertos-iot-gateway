# ADR-0022: The publisher starts at boot from an init script, waits for its inputs, and logs to syslog

**Status:** Accepted (tested on the board, 2026-11-01)
**Date:** 2026-11-01
**Phase:** 4

## Context

ADR-0020 and ADR-0021 fixed what the publisher does. They did not say how it is started. A gateway that
needs me to log in and type a command after every power-on is not a gateway. Three questions:
who starts the program, what it does when what it needs is not ready yet, and where its messages go
when it has no terminal.

Facts that decide them:
- The image uses sysvinit with `busybox-syslog`. The broker is already started by an init script
  (`S30mosquitto`), so there is a working pattern on this image.
- At boot the program's two inputs may be late: the device file `/dev/stm32_sensor_node` appears when
  udev has loaded the driver, and the broker may not be listening yet when the program starts.
- The node may be off or unplugged for a while. That is already handled by the loop (ADR-0021: `-EIO` is
  logged and nothing is published).
- A program started by init has no terminal, so `printf` goes nowhere.

## Options considered

### Starting the program
- **An init script in the recipe, linked with `update-rc.d` (chosen).** `defaults 40 20` gives `S40` (after
  the broker's `S30`) and `K20` (before its `K30`). Same pattern as the broker. Cons: nothing restarts
  the program if it crashes.
- **A line in `/etc/inittab` with `respawn`.** init restarts the program when it ends. Cons: it is a
  different mechanism from the broker's, it edits a file that belongs to another recipe, and a program
  that exits at once would be restarted in a loop.
- **Starting it from the udev rule that loads the driver.** Ties the program to one event and not to the
  system start; the broker may not be there. Not considered further.

### When the inputs are not ready
- **Rely on the start order only.** `S40` after `S30` does not mean the broker is already listening, and
  says nothing about udev and the driver.
- **The program waits and retries by itself (chosen), in addition to the order.** Every 2 s until the
  device opens and the broker accepts. The order is a convenience and not a requirement.

### Where the messages go
- **`syslog()` from the program (chosen).** The log daemon of the image keeps them; with `LOG_PERROR` the
  same lines also appear on the terminal when I start the program by hand.
- **A shell wrapper that pipes the output to `logger`.** Another process and another script for the same
  result.
- **Output to a file the program opens.** I would own rotation and the file system writes.

## Decision

Init script, retry in the program, `syslog()`. The swing factor: all three use what the image already has
(sysvinit like the broker, syslog from BusyBox), and none adds a program or a package.

Small rules that follow from it:
- The signal handlers are installed first, so `stop` (SIGTERM) also ends the waiting.
- While waiting, the first failure is logged and the repeated ones are not (a broker that is down would
  otherwise add a line every 2 s).
- A connection failure logs the library's numeric code and not `errno`: on the board, with the broker
  stopped, `strerror(errno)` said "Bad file descriptor" and not "Connection refused". My guess, not checked
  in the library source, is that the library makes more system calls after the failed `connect()` and they
  overwrite `errno`.

## Consequences

- The program starts by itself after power-on, in the order broker, then publisher.
- Tested on the board: start at boot with no manual command; `stop` and `start` by hand (a second `start`
  does nothing); the node powered on after the Pi (the program logged `read failed` and then `read works
  again` without a restart); the broker stopped while the program started (one log line, then
  `connected to the broker` when the broker came back).
- `logread` does not work on this image (`can't find syslogd buffer`). The log daemon writes the file
  `/var/log/messages`, so that is where to look. The Pi has no clock, so the dates in it are not real; the
  order of the lines is.
- **Nothing restarts the program if it crashes.** The loop has no known way to crash. Tested on the
  board on 2026-11-03 with `kill -9` (SIGKILL, which the program cannot catch): 10 seconds later there was
  no `gateway-publisher` process (`pidof` printed nothing), so nobody restarts it. `kill -TERM` also ended it,
  as the signal handler is meant to. After the kill, `/etc/init.d/gateway-publisher start` worked although the
  pid file of the dead process was still there, because `start-stop-daemon` checks that the process exists.
  `respawn` in `inittab`, or a small supervisor, is the way out if it ever matters.
- Read in the build folder of the image (2026-11-02), not on the Pi: `/etc/inittab` has `id:5:initdefault:`,
  so the default runlevel is 5. The `rcS.d` stage runs first and its `S04udev` starts `udevd` and then runs
  `udevadm trigger --action=add` and `udevadm settle`, which replays the events of devices that already exist
  (this loads my driver module). The module can still appear after `S40` runs, which is why the program waits
  for the device file.
- In `rc6.d` the links `K20gateway-publisher` and `K20syslog` have the same number; as far as I know `rc` runs
  equal numbers in alphabetical order, so the publisher stops before syslog and before `K30mosquitto`. That is
  from the file listing only.
- Not tested: the stop order at shutdown on the board, and a power cut with the program running.
- The log file is in RAM or on the root filesystem; I have not looked at how big it can grow.

## Interview angle

"The program does not trust the start order. init starts it after the broker, but that only means the
broker's script ran, not that it is listening, so the program retries by itself and logs the reason once.
Its messages go to syslog because a service has no terminal."
