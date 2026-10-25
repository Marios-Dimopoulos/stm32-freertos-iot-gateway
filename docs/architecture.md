# Architecture

This document reflects the **actual current state** of the system, not the
target design. Update it as subsystems are completed — don't let it drift
into aspiration.

## Current status: Phases 1 and 2 (firmware) are finished and verified; Phase 3 has a minimal image of my own (`gateway-image`, 37 MB root filesystem, 76 packages) that boots, joins Wi-Fi by itself, can be reached over SSH and reads valid frames from the STM32 over SPI with a test tool (no kernel driver yet); Phase 4 has not started

FreeRTOS runs on the Nucleo at **84 MHz** (HSI + PLL) with three tasks. The
**sensor task** reads the BMP280 over I2C1 once a second, prints
`T=24.08 C  P=1017.61 hPa` over USART2 (115200 baud, ST-LINK virtual COM port,
Tera Term; a screenshot is in `docs/captures/phase1-terminal-output.png`), and
builds one 14-byte **frame** per period (marker, status, sequence, temperature,
pressure, CRC-16) that it keeps in a one-slot "latest frame" queue. SPI1 is a
**slave**; the master is a Raspberry Pi Zero 2 W running the stock Raspberry Pi OS
Lite with Python `spidev`, used as a **temporary test jig** (the custom image is
Phase 3). The **transmit task** (priority 2) is woken by an interrupt on the rising
edge of NSS (the end of a transaction) and loads the newest frame for the next
one; DMA2 stream 3 moves its bytes into the SPI data register, with no CPU
instruction in the transfer. All the reasoning, the failures found on the board
and the numbers are in **ADR-0010**.

**Verified on the board:**
- Phase 1: register values and SysTick `LOAD = 83999` checked in the debugger;
  disconnecting the BMP280 gives `NACK` and everything comes back by itself; a
  falling edge on its CSB pin locks it out of I2C until its power is cycled
  (ADR-0007).
- The Pi reads a real frame every second with a valid CRC, at SPI clocks from
  100 kHz to 6 MHz, and back to back from Python (no pause between reads).
- 3000 reads in about 35 s with the node running freely: every interrupt and
  wake-up counted, no I2C error on the sensor task (the transmit task preempts it).
- A short or interrupted transaction does not leave the node shifted by one byte:
  at every re-arm the CPU writes byte 0 into the SPI data register itself
  (ADR-0010, decision 7).
- A 30-minute run (1800 reads, 1 s apart): 1797 good, 0 shifted, 3 bad. The node's
  counters agreed with the Pi (1800 ends, 1800 wake-ups, 0 reload failures), and 15
  spikes on NSS were ignored. One of the bad frames is a 2-bit slip, probably noise
  on SCK (ADR-0010).
- Ten runs of the test script stopped with Ctrl+C, and a run after them: no shifted
  or bad frame (ADR-0010).
- Noise on NSS: spikes on that line are ignored by the interrupt handler
  (decision 8). With the wires still, 7 spikes were seen in 3000 transactions and
  none damaged a frame; moving the wires by hand corrupts frames, which the Pi
  detects with the CRC, and the next frame is good.
- The sensor failing travels in the frame: with SDA unplugged the Pi gets frames
  with status `NACK` and zeros, then `NO_READING`, then `OK` when it is back.
- On the wire, with a logic analyzer (2026-10-20, `docs/captures/`): every BMP280 read
  is START, `0xEC`, `0xF7`, repeated START, `0xED`, six bytes (ACK on the first five,
  NACK on the last), STOP, 834 us (ADR-0007); the SPI frame is 112 clocks in mode 0 and
  decodes with a valid CRC; SCK measured 1.000, 2.000, 3.997 and 5.88 MHz for 1, 2, 4
  and 6 MHz requested; NSS stays high 154 to 228 us between back-to-back reads, and
  30000 such reads were good (ADR-0010). The CPU clock runs about 1.1% fast (ADR-0006).

**What is not verified:** the re-arm time (only an upper bound: the node was ready
within the 154 us minimum gap of back-to-back reads); the cause of the firmware's NSS
spikes (the analyzer showed spikes only with a probe wiring that disturbed the link);
MOSI on the wire; the UART bit time (PA2 is not reachable on the headers, see below);
the first read after an idle time returns a stale frame (see the known gaps).
Measured on the wire (logic analyzer, 2026-10-20): the I2C read of the BMP280, the
clock error (about 1.1% fast), the SPI frame (112 clocks, mode 0, valid CRC), the SCK
frequency at 1 to 6 MHz and the gap between back-to-back reads.

**What exists:** the Keil project (`firmware/stm32-sensor-node/`) with the
FreeRTOS kernel and twelve application modules (clock, uart, i2c, bmp280, format,
frame, spi, sensor task, transmit task, hardfault, freertos_callbacks, main), seventeen ADRs (`docs/decisions/`), the
test script `tools/spi_frame_test.py`, the README, this document, and the logic analyzer
captures of the I2C and SPI links (`docs/captures/`).
**What does not exist:** a UART capture, a measured re-arm time, and a gateway that is
part of the pipeline (the Phase 3 image boots, joins Wi-Fi at boot and accepts SSH
logins and reads valid frames over SPI with a user-space test tool, but has no kernel
driver; Phase 4 has not started).

**Where to continue:** see "Where to continue" below.

### Toolchain / project setup (done)

- Keil MDK-ARM project: `firmware/stm32-sensor-node/stm32-sensor-node.uvprojx`.
- Compiler: **Arm Compiler 6** (armclang V6.24).
- Single-precision FPU enabled in Target options (required by the
  `ARM_CM4F` port).
- Device support from the Keil STM32F4 DFP: `startup_stm32f411xe.s`,
  `system_stm32f4xx.c` (under `RTE/Device/STM32F411RETx/`).

### FreeRTOS kernel (V11.3.1): source integrated, configuration complete

```
firmware/stm32-sensor-node/FreeRTOS/
  event_groups.c list.c queue.c tasks.c timers.c   → group "FreeRTOS Core"
  include/                                          (vendor headers, unmodified)
  portable/GCC/ARM_CM4F/port.c, portmacro.h        → group "FreeRTOS Portable"
  config/FreeRTOSConfig.h                           → group "FreeRTOS Config"
```

- Include paths: `.\FreeRTOS\include`, `.\FreeRTOS\portable\GCC\ARM_CM4F`,
  `.\FreeRTOS\config`, `.\Src`.
- **Port:** `GCC/ARM_CM4F`, not `RVDS/ARM_CM4F`. The RVDS port uses armcc
  embedded assembler, which Arm Compiler 6 doesn't support. See ADR-0002.
- **No `heap_x.c`** is compiled on purpose, so `pvPortMalloc()` does not exist.
- Vendor files (`include/`, `portable/`, core `.c` files) are kept
  unmodified. Project-specific configuration lives only in `config/`.

### `FreeRTOSConfig.h`: all sections complete

| Section | Status | Settings |
|---|---|---|
| Memory allocation model | ✅ done | `configSUPPORT_STATIC_ALLOCATION 1`, `configKERNEL_PROVIDED_STATIC_MEMORY 0`, `configSUPPORT_DYNAMIC_ALLOCATION 0`, `configTOTAL_HEAP_SIZE 0` (ADR-0001) |
| Clock / tick | ✅ done | `configCPU_CLOCK_HZ (SystemCoreClock)`, `configTICK_RATE_HZ 1000`, `configUSE_PREEMPTION 1`, `configUSE_TIME_SLICING 1`, 32-bit tick (ADR-0003) |
| Tasks / stacks | ✅ done | `configMAX_PRIORITIES 3` (see task table below), `configMINIMAL_STACK_SIZE 128` words, idle/tick hooks off, `configUSE_TIMERS 0`, CLZ task selection, stack overflow check method 2, high-water mark enabled |
| Interrupt priorities (NVIC/BASEPRI) | ✅ done | `configMAX_SYSCALL_INTERRUPT_PRIORITY` = level 5 (raw `0x50`), via `configPRIO_BITS 4` + `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5` |
| `configASSERT` | ✅ done | disable interrupts (BASEPRI) + infinite loop; enables the port's priority and handler-installation checks |
| Port handler mapping (SVC/PendSV/SysTick → startup file names) | ✅ done | direct routing: `#define vPortSVCHandler SVC_Handler` etc. |
| Optional API functions | ✅ done | `INCLUDE_vTaskDelay 1`, `INCLUDE_xTaskDelayUntil 1`, `INCLUDE_xTaskGetSchedulerState 1` (the I2C driver asserts with it) and `INCLUDE_uxTaskGetStackHighWaterMark 1` (set in the tasks section); the others stay at the default 0 |

### Application code (`firmware/stm32-sensor-node/Src/`)

- `main.c`: start-up only, in a fixed order: `clock_init_84mhz()`,
  `SystemCoreClockUpdate()`, `uart_init()`, `i2c_init()`, `spi_init()`,
  `sensor_task_create()`, `transmit_task_create()`, `spi_start()`,
  `vTaskStartScheduler()` (ADR-0003). The transmit task must exist before
  `spi_start()`, which enables the interrupt that notifies it. At the scheduler start
  the transmit task (priority 2) runs first and blocks at once, then the sensor task
  runs. The temporary blink task and the LED code are gone (PA5 is SPI1_SCK); they
  are in the git history.
- `sensor_task.c` / `sensor_task.h`: the sensor task, which owns the BMP280 and
  the I2C bus (priority 1, static 256-word stack + TCB, `vTaskDelayUntil` with a
  1 s period). Each period: if the sensor is not initialised it calls
  `bmp280_init()`; otherwise `bmp280_read()` and prints the line. **Any failure**
  is counted, printed as `BMP280 error: <cause> (failures: n)`, and the next
  period initialises the sensor again, so a sensor that was power-cycled
  recovers without resetting the MCU. Every line is built in a 64-byte buffer
  (sized for the worst case) and sent with one `uart_write()`. **Every period, in
  every case**, it also builds one frame (status `NO_READING` after a successful
  init, `OK` with the reading, or the matching failure cause with zeros; a private
  `switch` translates `bmp280_status_t` into `frame_status_t`), numbers it (the
  counter goes up before the build, so the first frame is 1 and 0 is kept for the
  placeholder), and hands it to a one-slot queue (`xLatestFrame`, private to the
  file, `xQueueOverwrite`). The sequence therefore keeps moving while the sensor
  fails: it is a heartbeat. `sensor_task_latest_frame()` copies the newest frame
  out with `xQueuePeek` and never waits. The stack was measured at 89 words peak in
  Phase 1 and again with the frame code at 96 words (160 free), and kept at 256
  (ADR-0005).
- `format.c` / `format.h`: three tiny text functions with no `printf` and no
  `malloc`: `format_text`, `format_u32` and `format_fixed2` (a number in
  hundredths with two decimals: 2364 gives `23.64`, -5 gives `-0.05`). They write
  into the caller's buffer and return the length, or 0 with nothing written if the
  text does not fit. The algorithm was checked in Python on edge cases
  (including the smallest `int32_t`); the C was exercised on the board with real
  values only.
- `freertos_callbacks.c`: `vApplicationGetIdleTaskMemory()` (static Idle TCB +
  stack sized with `configMINIMAL_STACK_SIZE`) and
  `vApplicationStackOverflowHook()` (raise BASEPRI and loop forever).
- `hardfault.c`: the HardFault handler. The start-up file's weak handler is only a
  branch to itself; this one replaces it. A small naked function picks the stack
  that was in use (bit 2 of `EXC_RETURN`: MSP for start-up code and interrupts, PSP
  for a task) and passes its pointer to a C function, which copies the eight
  registers the CPU pushed (R0-R3, R12, LR, **PC**, xPSR), the stack pointer,
  `EXC_RETURN` and the fault status registers (`CFSR`, `HFSR`, `MMFAR`, `BFAR`) into
  a static record, `xFaultRecord`, and stops. The stacked PC is the address of the
  faulting instruction. BusFault, MemManage and UsageFault are not enabled, so they
  all arrive here; the kind is in `CFSR`. It cannot help if the stack pointer itself
  is bad. **Tested on the board** (2026-10-17) with a deliberate read of an unmapped
  address, removed afterwards: the record in the debugger was what the comment at the
  top of the file predicts. How to repeat the test is in that comment.
- `clock.c` / `clock.h`: `clock_init_84mhz()` takes the CPU from the 16 MHz HSI
  to 84 MHz through the PLL (ADR-0006). Five steps in order: flash wait
  states (2) + prefetch + caches, bus prescalers (AHB /1, **APB1 /2 = 42 MHz**,
  APB2 /1), PLL numbers (M 8, N 168, P /4, source HSI), PLL on + bounded wait
  for `PLLRDY`, switch + bounded wait for `SWS`. Returns a status code and
  never hangs; on failure the CPU stays on the HSI. PLL limits are checked
  with `_Static_assert`. `main()` calls it before `SystemCoreClockUpdate()`.
- `uart.c` / `uart.h`: my own register-level USART2 driver (TX only, PA2 =
  AF7). `uart_init()` enables the USART2 and GPIOA clocks, sets PA2 as a
  push-pull, no-pull, low-speed alternate function (stated explicitly even
  where it equals the reset value; AFR written before MODER, MODER last),
  configures 8N1 at 115200 with
  the divisor `BRR = round(PCLK1 / baud)` computed from the real clock
  registers (365 = 0x16D at 42 MHz; it would still be readable on the HSI),
  enables the transmitter, and sets the interrupt priority before enabling it.
  `uart_write()` copies a whole message into a 256-byte static ring buffer
  inside one FreeRTOS critical section and sets `TXEIE`; `USART2_IRQHandler`
  (level 10, i.e. at or below the FreeRTOS line, checked with
  `_Static_assert`) sends one byte per interrupt and clears `TXEIE` when the
  buffer is empty. Only the sensor task calls it today. The critical section
  protects the shared `CR1` register (a benign race, kept anyway) and, for any
  future second caller, task against task and the free-space check with the
  copy as one unit. **All or nothing:** if the message does not fit, nothing is
  stored and `UART_ERR_FULL` is returned, so a line is never cut and tasks'
  messages never interleave. There is no drop counter (removed 2026-10-16: a
  drop cannot happen with one short line per second). Never waits. Tasks
  only, after the scheduler has started (asserted at the start of `uart_write()`,
  like the I2C driver's calls).
  Verified in Tera Term, including the drop behaviour with a temporary burst
  test (removed): two 102-byte lines arrived complete and the third was
  dropped whole.
- `i2c.c` / `i2c.h`: my own register-level I2C1 master driver for the BMP280
  (PB8 = SCL, PB9 = SDA, AF4). `i2c_init()` enables the GPIOB and I2C1 clocks,
  resets I2C1 (`SWRST`), sets the pins as open-drain alternate function with
  the internal pull-up as a backup (MODER written last), programs the 100 kHz
  timing (`CR2.FREQ` = 42, `CCR` = 210, `TRISE` = 43, computed from the real
  APB1 clock) and enables the peripheral. `i2c_write_reg()` and
  `i2c_read_reg()` (1 byte, or 3 or more; 2 is refused because nothing needs
  it) do a whole register transaction and return `I2C_OK`, `I2C_ERR_NACK`,
  `I2C_ERR_TIMEOUT` or `I2C_ERR_BUS`. The waits are bounded in real time by the
  RTOS tick (about 10 ms each), so the driver is task-only; the receive
  deadline operations (clear ACK, ADDR, STOP) run in a short critical section.
  After a timeout, a bus error or a bus that stays busy the driver pulses
  `SWRST` and re-initialises I2C1 itself (counted by `i2c_recovery_count()`,
  which nothing reads); the public functions assert that the scheduler has
  started. **Verified on the board with the BMP280 at 0x76:** register write,
  chip ID 0x58, the 6-byte burst and the 24 trimming bytes; a missing sensor
  gives NACK without hanging; the sensor-disconnect test recovers by itself.
  Grounding SDA gave TIMEOUT on 2026-10-06 and NACK on 2026-10-08 (not
  investigated). **Limits:** no bus clear (a sensor holding SDA low is not
  freed); the pull-ups of the breakout were never measured. **Seen on the wire
  (2026-10-20):** the 3-or-more-byte receive sequence gives ACK on every byte but the
  last and NACK on the last, and SCL runs at a typical 101.1 kHz for the 100 kHz set.
  A sensor locked out by CSB needs a power cycle, which no I2C
  driver can fix (see the BMP280 entry). Decisions (register API, 7-bit
  address, distinct error codes, tick-based bounded polling, critical section
  for the time-critical receive operations, single owner without mutex,
  self-recovery, 100 kHz): ADR-0007.
- `bmp280.c` / `bmp280.h`: the BMP280 module, on top of `i2c_read_reg()` /
  `i2c_write_reg()`; one task owns it (no mutex). `bmp280_init()` does, in
  order: the soft reset (0xB6 to 0xE0), a 10 ms `vTaskDelay`, the chip-ID check
  (0xD0, expected 0x58), a read of the 24 factory trimming bytes into a local
  buffer and, only if it worked, their unpacking (low byte first; `dig_T1` and
  `dig_P1` unsigned, the other ten signed) into a static struct, and then the
  two configuration registers: `config` (0xF5: standby 0.5 ms, IIR filter 16)
  before `ctrl_meas` (0xF4: x16 oversampling for both, normal mode), built from
  named fields (0x10 and 0xB7). Writing `ctrl_meas` starts the measurements.
  The registers are not read back: a wrong value would show in the
  measurements. It must run in a task, after the scheduler has started, and a
  flag set only after a complete init lets `bmp280_read()` assert that init
  succeeded. `bmp280_read()` does one 6-byte burst from 0xF7, assembles the
  two 20-bit raw values (0x80000 means no measurement yet:
  `BMP280_ERR_NO_DATA`) and applies Bosch's integer compensation as two pure
  functions (temperature in 32 bits, pressure in 64 bits; left shifts of
  possibly negative terms are written as multiplications). It returns a
  `bmp280_reading_t`: temperature in hundredths of a degree, pressure in Pa.
  Status codes: `BMP280_OK`, the I2C cause (NACK, TIMEOUT, BUS), `ERR_ID`, and
  `ERR_NO_DATA` (also used if the pressure divisor would be zero);
  `bmp280_status_text()` gives the text. **Verified on the board:** OK with the
  sensor connected; NACK with CSB at GND after a power cycle; the trimming
  bytes in the typical ranges; a reading of 2364 (23.64 C) and 101969 Pa in the
  debugger, now printed every second, stable. With the sensor powered, a
  falling edge on CSB makes it answer NACK until its power is cycled, even
  after CSB goes back to 3.3 V (reproduced twice): CSB must be wired firmly to
  3.3 V. The register layouts and codes were compared with the BMP280 datasheet
  (2026-10-20) and match; the filter code is the one item its text does not list.
  Decisions: ADR-0009.
- `frame.c` / `frame.h`: the contract with the Pi. `frame_build()` writes a 14-byte
  frame byte by byte (little-endian, no struct): marker `0xA5`; status
  (`frame_status_t`: `OK`, `NO_READING`, `SENSOR_NACK`, `SENSOR_TIMEOUT`,
  `SENSOR_BUS`, `SENSOR_ID`, `SENSOR_NO_DATA`, with fixed numbers because they go
  on the wire); sequence (`uint16`); temperature (`int32`, hundredths of a degree);
  pressure (`uint32`, pascals); CRC-16/CCITT-FALSE (polynomial `0x1021`, start
  value `0xFFFF`) over the first 12 bytes, low byte first. A pure function with no
  state. The bytes the C code produces match a Python replica, in the debugger and
  over the wire (test vectors in ADR-0010).
- `spi.c` / `spi.h`: SPI1 as a **slave** (mode 0, 8-bit, MSB first, hardware NSS;
  PA4 NSS with pull-up, PA5 SCK with pull-down, PA6 MISO push-pull, PA7 MOSI; all
  AF5). `spi_init()` enables the clocks, sets the pins and configures SPI1, and
  leaves it **disabled**. `spi_start()` configures DMA2 stream 3 once (channel 3 =
  SPI1_TX, normal mode, memory increment; `PAR` = `SPI1->DR`, `M0AR` = the buffer,
  both base addresses written once), loads a placeholder frame, enables the NSS
  interrupt and only then enables the SPI. `spi_load_frame()`, called by the
  transmit task, stops the stream with a bounded wait, copies the frame into the
  DMA buffer (`ucDmaFrame`, written nowhere else), **writes byte 0 into `DR`
  itself**, sets `NDTR` for the remaining 13 bytes and enables the stream: the
  state after it is the same whatever the previous transaction was. The EXTI line 4
  handler (NVIC level 6) clears the pending bit and reads PA4: high means a real
  end of a transaction (counted in `ulNssRisingCount`, the transmit task is woken),
  low means a spike (counted in `ulNssGlitchCount`, nothing else). Counters for the
  debugger: `ulNssRisingCount`, `ulNssGlitchCount`, `ulReloadFailures`. Decisions
  and failures: ADR-0010.
- `transmit_task.c` / `transmit_task.h`: priority 2, static 128-word stack (measured:
  39 words peak, ADR-0005). It waits without limit for a task notification (`ulTaskNotifyTake( pdTRUE,
  portMAX_DELAY )`: the count is cleared, so ends that merge cost one wake-up), then
  fetches the newest frame with `sensor_task_latest_frame()` (the placeholder if the
  queue is still empty) and calls `spi_load_frame()`. `ulWakeUps` counts its
  wake-ups. The NSS handler gives it the notification through
  `transmit_task_end_of_transaction_from_isr()`; the handle stays private.
- Footprint, from the Keil build of 2026-10-17 10:52
  (`Listings/stm32-sensor-node.map`): Code 18,316 B, RO data 612 B (18,928 B = 18.5 KB
  of flash in total), RAM 4,072 B of 128 KB (RW 8 B + ZI 4,064 B, including the 1 KB
  main stack, the stacks of the sensor task (1 KB) and the transmit task (512 B), the
  UART buffer, and the 56-byte fault record). The start-up heap is 0 bytes since
  2026-10-17: the 512-byte C-library heap that nothing used is gone (RAM was 4,536 B
  in the map of 2026-10-15, before it and before the record were added). The growth
  since Phase 1 (12.8 KB of flash, 3.8 KB of RAM) is the frame, SPI, DMA, EXTI,
  transmit-task and fault-handler code, plus the queue code of the kernel. **No
  `malloc` is linked** (the only matches in the map are the library function
  `_get_argv_nomalloc`). The 64-bit division comes from the C library
  (`__aeabi_ldivmod`). Keil's static stack report gave 324 B for the start-up call
  chain on the main stack in an earlier build (interrupts not included).

### Drivers

The course/teacher drivers (ARM University Program, ST Standard Peripheral
Library, teacher-written I2C) are kept **locally only** in
`firmware/**/reference/` (git-ignored, outside the Keil project) as reading
material. The clock, USART2, I2C1 and SPI1/DMA drivers were written from scratch at
register level, and the BMP280 module is built on them (the compensation
formulas are Bosch's, as in the `functions.c`). The decision to write own
register-level drivers instead of using a vendor library is ADR-0008.

### Known gaps (real current state, not bugs)

- **What the logic analyzer (a Raspberry Pi Pico with sigrok-pico) showed on
  2026-10-20** (ADR-0006, ADR-0007, ADR-0010): the clock runs about 1.1% fast, so the
  "1 s" sensor period is 0.988 to 0.989 s; the SPI frame is 112 clocks in mode 0; SCK is
  1.000, 2.000, 3.997 and 5.88 MHz for 1, 2, 4 and 6 MHz requested (once 0.625 MHz for
  1 MHz); the Pi adds one extra clock period between bytes. Back-to-back reads from
  Python leave NSS high for 154 to 228 us (30 gaps), and 30000 such reads were all good.
  Still open: the **re-arm time** itself (only the upper bound of 154 us); a kernel driver
  or a C program may be much faster than Python, so the Phase 4 driver has to enforce a
  delay; the UART bit time (PA2 is wired to the ST-LINK only: with the solder bridges at
  their default, SB62 and SB63 open, the `D1` socket and the morpho pin do not carry it,
  per the Nucleo-64 manual UM1724; I did not change the bridges).
- **Spikes on NSS** (ADR-0010, decision 8): 7 in 3000 transactions in one run and 15
  in 1800 in the 30-minute run with the wires still (0.2% to 0.8%), many when the
  wires are moved. They are ignored by the
  interrupt handler. The logic analyzer saw spikes of 50 ns on NSS exactly at SCK's
  falling edge with a probe wiring that disturbed the link, and none in about 40 frames
  with a clean wiring, so the cause of the counted spikes is still not settled (long
  jumpers are the suspect). Frames hit by a real loss of contact are
  corrupted, and the CRC finds them; the next one is good.
- **The first read after an idle time is stale.** The buffer is refilled right
  after each transaction and nothing refreshes it, so a Pi that has not read for a
  minute gets a frame about a minute old (the sequence number shows it). Harmless
  for a poller that reads every period; to be decided with the Phase 4 driver
  (read twice, or refresh the buffer between transactions).
- The test master is the stock Raspberry Pi OS with Python `spidev`: a temporary
  jig (ADR-0010), not the final image.
- The SPI `OVR` flag (received bytes are never read) is set and not cleared;
  nothing has gone wrong that it would explain (30000 back-to-back reads were good).
- **Stacks.** Measured on 2026-10-18 after 60 reads from the Pi, as lower bounds:
  sensor task 96 words peak of 256, transmit task 39 words of 128 (the placeholder
  path did not run, and the NACK paths were not exercised). Idle: at least 21 words
  actually written (the `0xA5` pattern said 14, but R4-R11 were saved with the fill
  value, so the pattern was forged by data), 128 words allocated (ADR-0005).
- **Clock accuracy is that of the HSI: measured about 1.1% fast (ADR-0006).** The
  timeout/error paths of
  `clock_init_84mhz()` have never been triggered. `SystemCoreClock` reports HCLK
  only: USART2 and I2C1 must use the 42 MHz APB1 clock.
- `vApplicationGetTimerTaskMemory()` is NOT needed (`configUSE_TIMERS 0`).
- The I2C driver has no bus clear (a sensor holding SDA low is not freed), and
  nothing reads `i2c_recovery_count()`.
- The BMP280 can be locked out of I2C by a glitch on CSB until its power is
  cycled; the sensor task reports it and keeps trying, but only a power cycle
  fixes it.
- `docs/captures/` holds the Phase 1 screenshot and the logic analyzer captures (its
  README lists them). `docs/budget.md` is not updated with the real purchases.
- There is no LICENSE file (a decision, not an oversight).

### Architecture decisions so far

- ADR-0001: static-only memory allocation for FreeRTOS objects.
- ADR-0002: GCC/ARM_CM4F FreeRTOS port with Arm Compiler 6.
- ADR-0003: RTOS tick derived from `SystemCoreClock`, at 1 kHz.
- ADR-0004: interrupt priority threshold for FreeRTOS API calls at level 5.
- ADR-0005: task stacks sized by measurement with a margin; Idle stays at 128
  words; the sensor task (96 words peak) and the transmit task (39 words peak) were
  measured and keep 256 and 128.
- ADR-0006: CPU at 84 MHz from the HSI through the PLL, in its own module.
- ADR-0007: I2C1 driver: register-oriented polling API with tick-based timeouts and
  self-recovery.
- ADR-0008: own register-level drivers instead of a vendor library (decided
  2026-10-02, recorded 2026-10-08).
- ADR-0009: BMP280 module: normal mode, integer results, initialise again after
  every error.
- ADR-0010: the SPI link: the Pi as master and the STM32 as slave, a fixed 14-byte
  frame with CRC-16, a one-slot latest-frame queue, a transmit task that re-arms a
  normal-mode DMA on the NSS rising edge; the CPU primes the data register
  (decision 7); NSS spikes are ignored (decision 8).
- ADR-0011: the gateway image is built with Yocto (not Buildroot): the industry standard,
  and its layers keep my driver and recipes separate from the Raspberry Pi support.
  Phase 3 has started.
- ADR-0012: Yocto Wrynose 6.0 (LTS); BitBake 2.18, openembedded-core, meta-yocto and
  meta-raspberrypi cloned by hand into `~/yocto/` in WSL, outside the repo.
- ADR-0013: the `synaptics-killswitch` license flag is accepted so that the Pi Zero 2 W's
  Wi-Fi firmware is built into the image (the gateway needs Wi-Fi).
- ADR-0014: the `buildpaths` quality check is switched off for the `perl` recipe only
  (a workaround; the cause is not found; perl is not in the image).
- ADR-0015: the Wi-Fi network is typed on the board for now (the password stays out of the
  repo and the build) and OpenSSH is the SSH server; it also records the first OpenSSH build
  that shipped seven 0-byte programs.
- ADR-0016: a layer of my own, `meta-gateway`, with a `.bbappend` that replaces
  `/etc/network/interfaces` (`auto wlan0`, `nl80211`), so Wi-Fi starts at boot.
- ADR-0017: an image recipe of my own, `gateway-image`, built up from `packagegroup-core-boot`
  one tested step at a time (142 MB and 2015 packages down to 37 MB and 76).

## System design (implemented: this is what runs)

```
 BMP280 ──I2C1 (PB8 SCL, PB9 SDA)──┐
                                    ▼
        ┌─────────────────────────────────────────────────────────────┐
        │ STM32F411RE (Nucleo, 84 MHz), FreeRTOS, static allocation   │
        │                                                             │
        │  Sensor task (prio 1, 1 s) ──frame──► latest-frame queue    │
        │       │                                    │ xQueuePeek     │
        │       └─ lines ─► USART2 (PA2) ─► ST-LINK  │                │
        │                   virtual COM, Tera Term   ▼                │
        │  Transmit task (prio 2) ◄─ notify ◄─ EXTI4 ISR (NSS rising) │
        │       │ spi_load_frame()                                    │
        │       ▼                                                     │
        │  ucDmaFrame ──DMA2 stream 3──► SPI1->DR  (SPI1 slave)       │
        └───────────────────────────────────────────┬─────────────────┘
                              PA4 NSS, PA5 SCK, PA6 MISO, PA7 MOSI │
                                                                    ▼
                                       Raspberry Pi Zero 2 W (SPI master, test jig)
```

- One task reads the BMP280 over I2C1 (PB8/PB9) once a second and builds a frame.
- **Sensor: Bosch BMP280** (temperature + pressure; no humidity, unlike the
  BME280). Chip ID register `0xD0` reads `0x58`. I2C address `0x76` or
  `0x77` depending on the SDO pin.
- **Debug logging:** the sensor task prints readings directly over USART2
  (PA2 = TX; PA3 = RX is not used), which the Nucleo's ST-LINK exposes as a
  virtual COM port, by interrupt through a ring buffer, whole messages or
  nothing. This is a debug-only aid, not part of the data path (that is SPI +
  DMA). No dedicated logging task.
- No MPU-6050 IMU: it was dropped from the design.
- All RTOS objects (the tasks, the latest-frame queue) are statically allocated.

### The frame (the contract with the Pi, `frame.h`, ADR-0010)

| Bytes | Field |
|---|---|
| 0 | marker `0xA5` |
| 1 | status (`frame_status_t`: 0 OK, 1 no reading yet, 2 to 6 the sensor failure causes) |
| 2-3 | sequence (`uint16`, +1 per frame built, the first is 1; 0 is the placeholder) |
| 4-7 | temperature (`int32`, hundredths of a degree Celsius) |
| 8-11 | pressure (`uint32`, pascals) |
| 12-13 | CRC-16/CCITT-FALSE over bytes 0 to 11, low byte first |

All multi-byte fields are little-endian. The Pi decides when to read (it polls); the
node always keeps the newest frame loaded. A dead node reads as `0xFF` or `0x00`,
never `0xA5`; a repeated sequence means "nothing new"; a jump means frames were
skipped; a wrong CRC means damage.

### Task set and priorities (implemented)

Task priority: higher number = more urgent. `configMAX_PRIORITIES = 3`.

| Priority | Task | Why this level |
|---|---|---|
| 2 | Transmit task (re-arms the SPI DMA after every transaction) | The Pi is the SPI master and clocks data out when it decides, so the deadline is imposed from outside; when the interrupt wakes it, it must run at once, even in the middle of an I2C transfer of the sensor task. |
| 1 | Sensor task (BMP280 over I2C1, frames, debug prints) | Period chosen by me; a few ms of lateness is harmless. |
| 0 | Idle | Runs when nothing else is ready. |

At start-up the transmit task runs first (it is the highest priority), blocks at once,
and the sensor task runs next.

### Interrupt levels (lower number = more urgent)

ISRs that call `...FromISR()` functions must be at level 5 or a larger number
(ADR-0004).

| Level | Interrupt | Note |
|---|---|---|
| 6 | EXTI line 4 (NSS rising edge) | calls `vTaskNotifyGiveFromISR`; more urgent than the UART because the re-arm must finish before the next chip select |
| 10 | USART2 (transmit) | calls no FreeRTOS function |
| 15 | PendSV, SysTick | set by the kernel port |

No DMA interrupt is used: the stream stops by itself after its count, and the end of a
transaction is detected on the NSS pin (ADR-0010, decision 6).

## Where to continue

### Phases 1 and 2: finished

Phase 2 is closed. What was done: the long run and the aborted runs with
`tools/spi_frame_test.py` (ADR-0010), the stack measurements (ADR-0005), and the logic
analyzer captures of I2C and SPI (2026-10-20, `docs/captures/`). Open items that are
accepted and not blocking:

- the re-arm time (only the 154 us upper bound; measuring it would need a marker pin in
  the firmware);
- the UART bit time (PA2 does not reach the headers with the default solder bridges;
  the board was not modified);
- whether the firmware's own NSS spikes are real (none were seen in about 40 frames
  with a clean wiring);
- the stale first read after an idle time, which the Phase 4 driver decides.

Learning checkpoints: quizzes were done for the clock module, the UART and the I2C
driver, and an interview-style question set on Phases 1 and 2 was answered and graded.

### Phases 3 and 4

Phase 3: the image is built, flashed and boots on the board to a root login on the serial
console (see the next section). Wi-Fi starts at boot (ADR-0016) and the network is in the
image (key kept outside the repo, ADR-0015), so SSH works after a flash with nothing typed.
SPI is on and the test tool `spi-frame-test` read valid frames from the STM32 (see "SPI and
the test tool"). The image is my own minimal recipe (ADR-0017). Root has a password (hash kept
outside the repo, ADR-0015 update of 2026-10-24); the serial console and SSH both ask for it. The
clean-up of the image is done as far as I decided (ADR-0017 update of 2026-10-25); Phase 4 is next.
Phase 4 has not started. It has two requirements that come from Phase 2: the kernel
driver must leave a minimum delay between SPI transfers, and it must decide what to do
about the stale first read (read twice, or refresh the buffer on the node).

## Gateway image (Phase 3: built, booted, reachable over Wi-Fi and SSH)

The image is built with Yocto Wrynose 6.0 (ADR-0011, ADR-0012) in WSL2 (Ubuntu 24.04,
12 GB of RAM given to WSL through `.wslconfig`). The repos are cloned by hand into
`~/yocto/`, outside this repo. Cloned on 2026-10-18:

| Repo | Branch | Commit | Date of the commit |
|---|---|---|---|
| `bitbake` | `2.18` | `6e260eb6ab16` | 2026-10-07 |
| `openembedded-core` | `wrynose` | `636e5d3b12d5` | 2026-10-12 |
| `meta-yocto` | `wrynose` | `7c25b11156553` | 2026-09-30 |
| `meta-raspberrypi` | `wrynose` | `919588393f41` | 2026-10-10 |

### Build configuration

The configuration is used from `~/yocto/build/conf/` in WSL. Copies are in
`linux-gateway/conf/`: `bblayers.conf` byte for byte, and `local.conf` trimmed to its
active lines with my comments (the 20 active lines were compared with the real file on
2026-10-25 and are identical). `bblayers.conf` lists four layers: `openembedded-core/meta`, `meta-yocto/meta-poky`,
`meta-raspberrypi` and `meta-gateway` (mine, ADR-0016; it is in `~/yocto/` and a copy is in
`linux-gateway/meta-gateway/`). `local.conf` adds, on top of the template:

```
MACHINE = "raspberrypi0-2w-64"
DISTRO = "poky"
ENABLE_UART = "1"
LICENSE_FLAGS_ACCEPTED = "synaptics-killswitch"       (ADR-0013)
INSANE_SKIP:perl += "buildpaths"                      (ADR-0014)
ENABLE_SPI_BUS = "1"                                  (added 2026-10-23: `dtparam=spi=on` in config.txt)
USE_VT = "0"                                          (added 2026-10-25: no getty on tty1)
```

The package list and the `IMAGE_FEATURES` (`ssh-server-openssh allow-root-login`) of the current
image are not in `local.conf` but in my image recipe
`meta-gateway/recipes-core/images/gateway-image.bb` (ADR-0017), together with the function that
writes the root password hash. Until 2026-10-24 the `IMAGE_FEATURES`, including the two that
allowed an empty root password, were in `local.conf` and applied to every image.

### Build result (`bitbake core-image-base`, 2026-10-18)

- First long run 4 h 12 min (ended on the perl check, ADR-0014); last run 8 min 43 s with
  5291 of 5695 tasks reused from the cache. Build folder 63 GB. Details in ADR-0011.
- Output in `build/tmp/deploy/images/raspberrypi0-2w-64/`: the image
  `core-image-base-raspberrypi0-2w-64.rootfs.wic.bz2` (79 MB compressed) with a `.bmap`
  file, the kernel (Linux 6.18.33), the device tree `bcm2710-rpi-zero-2.dtb` and
  `bootfiles/`.
- 2026-10-22: second build with `ssh-server-openssh` (ADR-0015): 2015 packages, 80.7 MB
  compressed. The first run of it had seven 0-byte OpenSSH programs in the image; a forced
  `bitbake -f -c install openssh` and a new `bitbake core-image-base` fixed it (ADR-0015).
  The image flashed for the SSH test is `...rootfs-20261022155006.wic.bz2`.
- 2026-10-22: third build, with the layer `meta-gateway` (ADR-0016): 78 MiB compressed,
  `...rootfs-20261022194428.wic.bz2`; no zero-byte file in the root filesystem folder outside
  `/etc`, `/var` and `/usr/share`.
- 2026-10-23: builds with `ENABLE_SPI_BUS` (`...rootfs-20261023093302.wic.bz2`; `config.txt`
  line `dtparam=spi=on` checked in `deploy/bootfiles/` and on the board), with the Wi-Fi key
  from a folder outside the repo (`...rootfs-20261023095901.wic.bz2`, 78 MiB), and with the
  package `spi-frame-test`. The last one was flashed for the SPI test. These images contain
  the Wi-Fi key: they and the build folder must not be shared.
- 2026-10-24: `bitbake gateway-image` (ADR-0017), the image recipe of my own. Last build
  `gateway-image-raspberrypi0-2w-64.rootfs-20261024145944.wic.bz2`: 37 MB root filesystem,
  76 packages, 35 MiB compressed (`core-image-base`: 142 MB, 2015 packages, 78 MiB). Flashed
  and tested: boots, `wlan0` up, `/dev/spidev0.0`, 10 of 10 frames. The steps in between are in
  ADR-0017.

### What is in the image

This describes the first image, `core-image-base`. The current image, `gateway-image`, is
described in its own section below. Read from the package manifest (2008 packages), the root
filesystem folder (142 MB) and the boot files in `deploy/` (the first boot below checked part
of it on the board):

- Boot partition (FAT): the Pi firmware, `config.txt`, `cmdline.txt`, the kernel and the
  device tree. `ENABLE_UART = "1"` produced `enable_uart=1` in `config.txt` and
  `console=serial0,115200` in the `cmdline.txt` of `deploy/bootfiles/`. That is **not** what
  the kernel received: its command line had `console=ttyS0,115200` (see "First boot").
  `cmdline.txt` also has `root=/dev/mmcblk0p2 rootfstype=ext4 rootwait`.
- Root partition (ext4): busybox (provides `ls` and the other basic commands; there is no
  `coreutils`), **sysvinit** as init (no systemd), `wpa-supplicant`, `iw`, the Wi-Fi firmware
  `linux-firmware-rpidistro-bcm43436`, and the kernel modules (including `spidev`).
- `/etc/inittab` starts a login prompt on `ttyS0` at 115200 baud. With the empty root
  password, `root` logs in without a password.
- The first image (2008 packages) had no SSH server; the current one (2015 packages) has
  OpenSSH (ADR-0015). Not in the image: perl, systemd, and a package manager (no `rpm`, `dnf`,
  `opkg` or `apt` in the manifest), so every change to the software is a rebuild and a new flash.
- On the board, `/etc/fstab` ends with `/dev/mmcblk0p1  /boot  vfat  defaults  0  0`. That
  line is not in the `rootfs/` folder of the build: it is added when the image is made. So
  partition 1 **is mounted** on `/boot`, read-write. It holds `kernel8.img` (27,052,544 bytes,
  the same size as the kernel `Image` of the build), the firmware files (all variants of
  `start*.elf` and `fixup*.dat`, of which this board uses `start.elf` and `fixup.dat`),
  `config.txt`, `cmdline.txt`, the `.dtb` and `overlays/`. The files read from the board
  match the ones in `deploy/bootfiles/`.

### First boot (2026-10-21)

Flashing: the image was decompressed from the `.wic.bz2` to `gateway.wic` (380 MiB, MBR
table; partition 1 FAT32 of 130 MiB from sector 8192, partition 2 Linux of 244 MiB from
sector 278528) and written to a 32 GB microSD with Raspberry Pi Imager ("Use custom",
without its customisation settings). Windows then showed the two partitions with the same
sizes as `fdisk` on the image (136,314,880 and 256,271,360 bytes).

Console: an FT232 USB-UART adapter, with its jumper on VCCIO-3V3 (3.3 V signals), on header
pins 6 (GND), 8 (Pi TX) and 10 (Pi RX); Tera Term at 115200, 8 data bits, no parity, 1 stop
bit.

What the boot log shows (times are the kernel's, in seconds):

- Kernel `6.18.33-v8`, built with `aarch64-poky-linux-gcc 15.3.0`; machine model
  "Raspberry Pi Zero 2 W Rev 1.0" (from the device tree); 4 CPUs.
- The kernel's command line ended with `dwc_otg.lpm_enable=0 console=ttyS0,115200
  root=/dev/mmcblk0p2 rootfstype=ext4 rootwait net.ifnames=0`, after options added by the
  firmware (`coherent_pool`, `8250.nr_uarts=1`, `smsc95xx.macaddr`, `vc_mem.*` and others).
  **The console is `ttyS0`** (the mini UART, `3f215040`), and `/etc/inittab` starts the login
  on `ttyS0`: **they are the same port.** The `cmdline.txt` inside partition 1 (read later
  on the board) says `console=serial0,115200`, and the kernel received `ttyS0`. So the
  firmware replaces `serial0` by the real port: I did not watch it do that, but nothing else
  sits between the file and the kernel.
- Root filesystem: `Waiting for root device /dev/mmcblk0p2` at 3.869 (`rootwait`), the SD
  card found at 3.946, `mmcblk0: p1 p2` at 3.958, ext4 mounted **read-only** at 4.029,
  `Run /sbin/init` at 4.060, remounted read-write at 11.643.
- `INIT: version 3.14` (sysvinit); login prompt about 13 s after the kernel started; `root`
  logs in without a password and gets a busybox shell. The banner says Poky 6.0.4.
- No U-Boot output before `Booting Linux`, and `RPI_USE_U_BOOT` is not set: the Pi firmware
  loads the kernel directly.
- Wi-Fi: the `brcmfmac` driver loaded the chip firmware (`BCM43430/2`, version 9.88.4.77) and
  the Bluetooth chip got its patch. No connection was made at that boot (see "Wi-Fi and SSH").
- Memory: 154,540K of 458,752K available, because 256 MiB are reserved as a CMA pool (for the
  GPU/camera). The clock after boot was `Mar 9 2018` (no battery clock, no network time).
- Messages that did not stop the boot, causes not investigated: `/usr/bin/psplash: not
  found` (`S00psplash.sh`), `ip: SIOCGIFFLAGS: No such device` twice during network start,
  `dbus: Unknown username "pipewire"`, `brcmf_c_process_txcap_blob: no txcap_blob available`.

### Wi-Fi and SSH (2026-10-22, ADR-0015)

**Wi-Fi, typed on the board** over the serial console. The image has `wpa_supplicant` 2.11,
`iw` and the busybox DHCP client. `wlan0` is in `/etc/network/interfaces` but is **not**
`auto`, and `/etc/wpa_supplicant.conf` held only a placeholder. I wrote the file with the
header lines and `country=GR`, added the network with `wpa_passphrase` (the password typed at
its prompt; the plain-text comment line it writes is deleted), and ran `ifup wlan0`. Results:

- The scan listed the network and about 13 others, all on 2.4 GHz (2412 to 2462 MHz); mine on
  2462 MHz (channel 11), signal -34 dBm, receive rate 65 Mbit/s.
- DHCP gave `192.168.1.4/24` from the router `192.168.1.1`, lease 86,400 s; default route via
  the router; DNS added from the lease.
- `ping 192.168.1.1`: 3 of 3, about 5.7 ms. `ping 1.1.1.1`: 3 of 3, about 15 ms. `ping
  example.com` resolved a name and answered.
- Warnings: the hook starts `wpa_supplicant` with the old `wext` interface
  (`wpa-driver wext`); the kernel warned that wireless extensions will stop working for newer
  hardware, and `wpa_supplicant` printed `ioctl[SIOCSIWENCODEEXT]: Invalid argument` twice.
  The connection worked; I do not know what the two errors affected.
- The settings stay across reboots but a reflash erases them. `wlan0` did not start at boot in
  this first image; the layer `meta-gateway` (below) fixed that.

**SSH.** `IMAGE_FEATURES += "ssh-server-openssh"` adds OpenSSH 10.3p1 (`sshd`, `ssh`, `scp`,
`sftp-server`, `ssh-keygen`; 7 more packages). `sshd_config` has `PermitRootLogin yes` and
`PermitEmptyPasswords yes` (from the two `allow-*` features), so I set a root password with
`passwd` after every flash. `S09sshd` starts the server at boot.

**The first OpenSSH image did not work.** `ssh` to the Pi answered `Connection refused`; on
the board there was no `sshd` process, no host keys and no log line. A trace of
`/etc/init.d/sshd start` showed that the script exits silently after `test -x
/usr/sbin/sshd`. In the build, `/usr/sbin/sshd` and six more programs were 0 bytes in the
`image/` folder of the openssh recipe, while the compiled programs in its source folder were
complete. The details and my hypothesis (a race in a parallel `make install`, not proven)
are in ADR-0015. `bitbake -f -c install openssh` and a new `bitbake core-image-base` gave
complete files (package 654,588 bytes instead of 29,913), and an SSH login from Windows then
worked.

### Wi-Fi at boot: the layer `meta-gateway` (2026-10-23, ADR-0016)

The recipe `init-ifupdown` (openembedded-core) installs `/etc/network/interfaces` from a file
next to the recipe. My layer has `recipes-core/init-ifupdown/init-ifupdown_%.bbappend` with
`FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"` and, in `init-ifupdown/`, my copy of
`interfaces`: the original plus `auto wlan0`, and `wpa-driver nl80211` instead of `wext`.

- `bitbake-layers show-appends` listed the `.bbappend` under `init-ifupdown_1.0.bb`; the root
  filesystem folder of the new image had both changes.
- On the board (after a flash, `passwd` and a hand-written `/etc/wpa_supplicant.conf`): after a
  reboot, with no `ifup wlan0`, SSH from Windows worked. `/proc/<pid>/cmdline` of
  `wpa_supplicant` ended with `-D nl80211`, and `dmesg` had no line about wireless extensions.
- Not checked: whether the `SIOCSIWENCODEEXT` errors are gone from the output of `wpa_supplicant`.

### Wi-Fi network in the image (2026-10-23, ADR-0015 update)

The recipe `wpa-supplicant` installs `/etc/wpa_supplicant.conf` (mode 600) from a file named
`wpa_supplicant.conf-sane`. My layer has `recipes-connectivity/wpa-supplicant/wpa-supplicant_%.bbappend`
with `FILESEXTRAPATHS:prepend := "/home/mariosdimopoulos/yocto/secrets:"`. The folder
`~/yocto/secrets/` is outside the repo and holds my `wpa_supplicant.conf-sane` (country, SSID and
the derived `psk`; `.gitignore` has a rule for any `secrets/` folder). With the image flashed, the
Pi joined Wi-Fi at boot and SSH worked with nothing typed on the board. If the folder is missing
the build does not fail and the placeholder network is installed.

### SPI and the test tool (2026-10-24)

`ENABLE_SPI_BUS = "1"` makes `config.txt` contain `dtparam=spi=on`; on the board `/dev/spidev0.0`
and `/dev/spidev0.1` exist. The image has no Python, so I wrote `spi-frame-test` in C
(`linux-gateway/meta-gateway/recipes-test/spi-frame-test/`): one SPI transfer of 14 bytes at
1 MHz in mode 0 through `spidev`, then the checks of `tools/spi_frame_test.py` (marker `A5`,
CRC-16). It is a bring-up tool, not the Phase 4 driver.

Wired as in the README table (Pi pins 24, 23, 21, 19, 25), the Nucleo running the Phase 2
firmware, `spi-frame-test 10` printed 10 lines, all with a valid CRC:

- Frame 0: `seq=0 status=1` (NO_READING), `T=0.00 P=0.00`. The next frame has `seq=26`.
  This is consistent with the stale first read of ADR-0010 (an old frame on the first read after
  a long idle); I did not measure the mechanism in this run.
- Frames 1 to 9: `seq` 26 to 34 (one more per second), `status=0`, `T=21.57 to 21.58 C`,
  `P=1015.69 to 1015.71 hPa`.

A second run on 2026-10-24, on the `gateway-image` build (modules `spi-bcm2835` and `spidev`
loaded by udev), gave 10 of 10 valid frames again, `seq` 183 to 191, `T=21.90 to 21.91 C`,
`P=1014.99 to 1015.02 hPa`. Frame 0 was byte for byte the same as in the first run
(`A5 01 00 00 ... 10 96`, `seq=0`, `status=1`): the first read after a long idle returned the
frame the node loaded at boot, in both runs. This is for the Phase 4 decision about the stale
first read; nothing is decided.

### The image recipe `gateway-image` (2026-10-24, ADR-0017)

`meta-gateway/recipes-core/images/gateway-image.bb` inherits `core-image` for the image tasks, but
replaces its package list: `IMAGE_INSTALL = "packagegroup-core-boot"` plus Wi-Fi (`wpa-supplicant`,
`iw`, `wireless-regdb-static`, the firmware packages `...bcm43430` and `...bcm43436`, the modules
`brcmfmac`, `brcmfmac-cyw`, `brcmutil`, `cfg80211`), SPI (`spi-bcm2835`, `spidev`) and
`spi-frame-test`. `BAD_RECOMMENDATIONS += "kernel-image-*"` keeps the 26 MB kernel copy out of the
root partition (the kernel is loaded from partition 1), and `IMAGE_LINGUAS = ""` drops the
language files. The comments in the recipe say why each name is there.

| Image | Root fs | Packages | Compressed |
|---|---|---|---|
| `core-image-base` | 142 MB | 2015 | 78 MiB |
| `gateway-image`, minimal (step 1) | 29 MB | 47 | 32 MB |
| `gateway-image`, final of 2026-10-24 | 37 MB | 76 | 35 MB |

On the board: boots from the kernel on partition 1, `wlan0` up with an address from the router,
`/dev/spidev0.0`, `spi-frame-test 10` gave 10 of 10 valid frames. The `getty` on `tty1` was removed
on 2026-10-25 (`USE_VT = "0"`); one `getty` runs, on `ttyS0`. Still in it: `kbd` and `keymaps` (1.7
MB, hard dependency of `packagegroup-core-boot`, left on purpose), the three `ip: SIOCGIFFLAGS`
lines at boot, and the Wi-Fi key and the root password hash (both in every image). Not measured:
boot time, memory use.

### Not verified / known problems

- I did not compare the SSH host key fingerprint on the board with the one the client
  showed, and I did not log in with the name `raspberrypi0-2w-64.local` (the error type
  suggests it resolved).
- The cause of the 0-byte OpenSSH programs is a hypothesis; I also cannot explain why
  `test -x /usr/sbin/sshd` was false on the board for an empty file with the executable bit.
  After every build I should look for zero-byte files in the root filesystem folder.
- The root filesystem on the board is writable, and `/boot` (FAT) is mounted read-write with
  `defaults`: a power cut during a write there could corrupt it. For the final image it should
  be read-only or not mounted.
- The Wi-Fi key and the root password hash are inside every image and in the build folder.
  Anyone with the SD card can read them. SSH accepts a password for `root` from anyone on the
  Wi-Fi, so the password is the only protection; SSH keys are not set up (they can be added later).
- The SPI controller is enabled and `spidev` makes `/dev/spidev0.0`, but no device-tree entry
  exists for our device (needed for the Phase 4 driver).
- Two runs of `spi-frame-test` (10 frames each) have been made: no speed sweep, no minimum-gap
  test, nothing on the logic analyzer from this image.
- `perl` is built although it is not in the image; why is not known (ADR-0014).
- `core-image-base` carried things a headless sensor gateway does not need (the services
  `alsa-state`, `avahi-daemon`, `bluetooth`, `ofono`, `neard`); `gateway-image` does not. The
  boot partition still has graphics and audio lines in `config.txt` (`dtoverlay=vc4-fkms-v3d`,
  `dtparam=audio=on`) and the kernel still reserves a 256 MiB CMA pool. Switching the graphics
  off with `DISABLE_VC4GRAPHICS = "1"` failed the build in `cairo` (the `meta-raspberrypi`
  `cairo_%.bbappend` adds an `egl` option that `cairo` 1.18.4 does not have), so I left it as it
  is (ADR-0017 update of 2026-10-25).
- Copies of `local.conf`, `bblayers.conf` and the layer are in `linux-gateway/`, but I have
  not tried a rebuild from an empty folder with them. `bblayers.conf` has absolute paths
  (`/home/mariosdimopoulos/yocto/...`). BitBake reads `~/yocto/meta-gateway`, not the copy in
  git; the two are kept the same by hand (identical on 2026-10-23).

Next: the Phase 3 quiz, then Phase 4. Open from Phase 3, optional: the graphics overlay and the CMA
pool (needs a `cairo` workaround in my layer), `kbd`, a read-only root, SSH keys.

## Open questions / not yet decided

- What produces the spikes on NSS that the firmware counts (the analyzer showed that a
  disturbing probe wiring creates spikes at SCK falling edges and a clean one none;
  ADR-0010).
- How to deal with the stale first read after an idle time (ADR-0010, consequences).
- The real re-arm time (only the 154 us minimum gap between back-to-back reads is
  measured).
- Whether the HSI accuracy stays good enough, or the clock moves to the ST-LINK's
  8 MHz HSE (needs `HSE_VALUE=8000000`, see ADR-0006). Measured: about 1.1% fast
  (ADR-0006), which nothing in the project needs better than so far; to be decided
  again if a precise timestamp or USB is ever needed, and checked against the UART
  bit time.
- Whether a bus clear is ever needed in the I2C driver (only if a sensor is
  seen holding SDA low).

## Change log

- 2026-09-27: Document created. No implementation yet.
- 2026-09-28: FreeRTOS V11.3.1 source added to the Keil project.
  `FreeRTOSConfig.h` created in `FreeRTOS/config/` with the memory
  allocation section (ADR-0001).
- 2026-09-29: FreeRTOS port switched from `RVDS/ARM_CM4F` to
  `GCC/ARM_CM4F` for Arm Compiler 6 compatibility (ADR-0002).
- 2026-09-29: `FreeRTOSConfig.h` clock/tick section: tick derived from
  CMSIS `SystemCoreClock` (correct on 16 MHz HSI today and after the future
  84 MHz PLL setup), 1 kHz tick. `main()` must run clock setup ->
  `SystemCoreClockUpdate()` -> `vTaskStartScheduler()` in that order.
- 2026-09-30: `FreeRTOSConfig.h` completed: tasks/stacks, interrupt
  priorities (threshold level 5), `configASSERT`, handler mapping,
  `INCLUDE_` settings, `configMAX_PRIORITIES 3`.
- 2026-09-30: Design decisions: sensor is the BMP280 (not BME280); the
  MPU-6050 IMU is dropped; debug logging over USART2 directly from the
  sensor task; task set and priorities fixed (table above).
- 2026-10-02: First application code: `Src/main.c` (temporary PA5 blink task)
  and `Src/freertos_callbacks.c`. The kernel builds and links; the blink runs
  on the board (period not yet measured). Teacher/course drivers moved to a
  git-ignored `reference/` folder; own register-level drivers planned.
- 2026-10-03: Stack usage measured with the `0xA5` pattern in the debugger
  (blink 27 words; Idle at least 21 words once cross-checked against the
  17-word context size). Recorded as ADR-0005. Real Keil `.map` checked: no
  `malloc`, Code 6200 B, RAM 3000 B.
- 2026-10-03: `Src/clock.c` added: the CPU now runs at 84 MHz from the HSI
  through the PLL, written and checked one step at a time on the board
  (flash wait states, bus prescalers, PLL numbers, PLL start, switch).
  Recorded as ADR-0006.
- 2026-10-04: `Src/uart.c` added (own USART2 driver, TX): clocks and PA2,
  115200 baud from the real APB1 clock, and a temporary blocking
  `uart_write_byte_blocking()`. Verified on the board: two dots per second in
  Tera Term.
- 2026-10-04: USART2 send is now interrupt-driven: ring buffer + TX
  interrupt (level 10) replace the temporary blocking call. Verified on the
  board: the dots still arrive at two per second.
- 2026-10-05: USART2 writes are now whole messages (`uart_write`,
  `uart_write_string`) with a drop counter. Verified on the board with a
  burst of three 102-byte lines: two arrived complete, the third was dropped
  whole each second.
- 2026-10-05: `Src/i2c.c` added (step 1: clocks, reset, pins). Register values
  checked in the debugger.
- 2026-10-06: I2C1 timing + enable and `i2c_probe()` done; the BMP280
  (CSB high, SDO low) answers ACK at 0x76.
- 2026-10-06: `i2c_write_reg()` added and verified (soft-reset write returns
  OK). `i2c_probe()` removed: the real transactions report ACK/NACK themselves.
- 2026-10-06: `i2c_read_reg()` added and verified on the BMP280 (chip ID,
  6-byte burst, 24 trimming bytes). Blink task stack raised to 256 words for
  the temporary I2C tests (to be measured).
- 2026-10-06: I2C error recovery (step 6) added and verified with an SDA-to-ground
  test; `INCLUDE_xTaskGetSchedulerState` enabled for the driver's assertion.
  Recorded as ADR-0007.
- 2026-10-07: I2C bring-up tests removed from `main.c` (the chip-ID check now
  lives in `bmp280_init()`). `bmp280.c` step 1 added (soft reset, wait, chip ID) and verified: OK
  with the sensor connected, NACK with CSB at GND. A falling edge on CSB while
  powered locks the sensor out of I2C until the USB cable is unplugged.
- 2026-10-07: BMP280 steps 2 to 6 done and verified (trimming bytes,
  configuration, burst read, integer compensation: 23.64 C and 101969 Pa in
  the debugger). `format.c` added.
- 2026-10-08: `uart_write_string()` removed: its only caller (the sensor task's
  init line) now builds the line with the formatter and sends it with
  `uart_write()`, so the UART has a single way to send.
- 2026-10-08: the real sensor task replaces the blink task and the LED code;
  `main.c` is start-up only. The sensor is read once a second and re-initialised
  after any error. Stack measured (89 words peak, 256 kept), recorded in
  ADR-0005. Disconnect/reconnect tests passed; CSB lock-out reproduced again.
- 2026-10-08: this document brought back in line with the code: the UART, I2C
  and BMP280 descriptions in the present tense (no more "built one step at a
  time" or removed tests), the `volatile` calibration struct and the blink
  task removed from the text, the optional-API row of the FreeRTOS table
  completed, the footprint taken from the 2026-10-08 map, the Phase 1 section
  marked as implemented, and the open questions and known gaps refreshed.
- 2026-10-08: ADR-0008 (own register-level drivers, decided 2026-10-02) and
  ADR-0009 (BMP280 module design) written; ADR-0006 and ADR-0007 now point to
  ADR-0008.
- 2026-10-09: `prvUartPinsAndClocks()` now sets PA2's output type, pull and
  speed explicitly (same values as the reset state, stated for intent, like the
  I2C pins); `GPIOA->MODER` expected value is now `0xA8000020` without the LED.
- 2026-10-09: `README.md` rewritten around what is verified and what is not, with
  the terminal screenshot in `docs/captures/`. The comments and ADRs are being
  moved to the first person.
- 2026-10-09: status brought up to date: Phase 1 complete, footprint from the
  2026-10-09 map (Code 12,452 B, RO data 604 B, RAM 3,816 B), a "Where to
  continue" section added (Phase 1 leftovers, Phase 2 candidate steps, including
  the open question of the SPI roles).
- 2026-10-11: ADR-0010 written (the SPI link): the Pi is the master, the frame format,
  the latest-frame queue and transmit task, normal-mode DMA re-armed on NSS rising.
  `frame.c` added: the 14-byte frame and its CRC-16; the C bytes were checked against
  a Python replica in the debugger.
- 2026-10-12: the sensor task builds one frame per period into a one-slot queue; the
  status codes got their own enum (`frame_status_t`) instead of a sentinel value.
  `spi_init()` added: SPI1 as a slave, pins and registers checked in the debugger.
- 2026-10-13: DMA2 stream 3 feeds `SPI1->DR` (configure once, reload per frame).
  First real transaction with the Raspberry Pi (stock OS, `spidev`): the placeholder
  frame, CRC valid. EXTI line 4 on NSS rising (works with the pin in alternate-function
  mode), transmit task woken by a task notification; 3000 reads without an I2C error.
- 2026-10-14: the transmit task re-arms the DMA with the newest frame. A 50-read test
  found the node staying shifted by one byte after a disturbed transaction; fixed by
  writing byte 0 into `DR` from the CPU at every re-arm (ADR-0010, decision 7).
- 2026-10-15: gap, clock, sensor-unplug and reset tests on the Pi. The interrupt
  handler ignores a rising edge on NSS when NSS is low again (decision 8); 7 spikes in
  3000 transactions with the wires still, none damaged a frame.
- 2026-10-16: the UART drop counter removed (nothing could fill the buffer, nothing
  read it); comments of `main.c` and `uart.c` brought up to date.
- 2026-10-17: this document and the README brought up to date with Phase 2;
  `tools/spi_frame_test.py` added. Footprint from the 2026-10-15 map: Code 18,160 B,
  RO data 612 B, RAM 4,536 B.
- 2026-10-17: three small leftovers closed: the start-up heap set to 0 bytes, an
  assertion that the scheduler has started in `uart_write()`, and a HardFault handler
  (`hardfault.c`) that records the stacked registers and the fault status registers.
  Built and run on the board; the handler was triggered on purpose and recorded the
  fault as expected, and RAM went from 4,536 B to 4,072 B (the heap is gone, the
  56-byte record added).
- 2026-10-18: stack peaks measured with the `0xA5` pattern after 60 reads from the Pi:
  sensor task 96 words of 256, transmit task 39 words of 128. Neither size changes
  (ADR-0005). Only comments changed in the code.
- 2026-10-18: test of aborted runs (10 runs stopped with Ctrl+C, then a 5-read run): no
  shifted or bad frame. Recorded in ADR-0010.
- 2026-10-18: the 30-minute run (1800 reads): 1797 good, 0 shifted, 3 bad; counters
  agree with the Pi; 15 NSS spikes ignored. One bad frame decoded as a 2-bit slip
  (probably noise on SCK), two without a found cause. Recorded in ADR-0010.
- 2026-10-18: Phase 3 started: ADR-0011 chooses Yocto for the gateway image. Nothing built yet.
- 2026-10-18: ADR-0012: Yocto release (Wrynose) and how the layers are fetched. Nothing built yet.
- 2026-10-18: the four Yocto repos cloned in WSL (commits in the Gateway image section).
- 2026-10-18: first `bitbake core-image-base` stopped on the Wi-Fi firmware license; ADR-0013 accepts it. Build started.
- 2026-10-18: the first long build run (4 h 12 min) failed the `buildpaths` check on `perl`;
  ADR-0014 skips that one check for that one recipe.
- 2026-10-18: `bitbake core-image-base` finished: image of 2008 packages, 79 MB compressed,
  build folder 63 GB. Build figures recorded in ADR-0011.
- 2026-10-20: the Pico logic analyzer works (sigrok-pico firmware, PulseView nightly).
  First captures, on I2C: the BMP280 read (address 0x76, register 0xF7, six bytes, ACK
  and NACK as expected, 834 us) and the clock: about 1.1% fast (SysTick period and SCL
  agree). Files in `docs/captures/`, results in ADR-0006, ADR-0007 and ADR-0009.
- 2026-10-20: first SPI captures: the frame is 112 clocks in mode 0 with a valid CRC; SCK
  1.000 / 2.000 / 3.997 / 5.88 MHz for 1 / 2 / 4 / 6 MHz requested; the Pi pauses one
  extra clock between bytes; NSS stays high 154 to 228 us between back-to-back reads
  (30000 such reads all good). NSS spikes on SCK falling edges appeared with a disturbing
  probe wiring and not with a clean one. Re-arm time and UART bit time not measured.
  Results in ADR-0010.
- 2026-10-21: the Phase 3 image flashed to a microSD and booted for the first time: root
  login on the serial console about 13 s after the kernel started. The console is
  `ttyS0`, the same port as the login (the `serial0` in `deploy/` was not what the kernel
  received). Details in "First boot" in the Gateway image section.
- 2026-10-21: the Yocto configuration copied into `linux-gateway/conf/` (committed on
  2026-10-22).
- 2026-10-22: Wi-Fi typed on the board and working (association, DHCP, routing, DNS); the
  boot partition read on the board (`kernel8.img`, mounted on `/boot` through a line that
  `wic` adds to `/etc/fstab`); the `serial0`/`ttyS0` question closed.
- 2026-10-22: OpenSSH added (`ssh-server-openssh`). The first build shipped seven 0-byte
  programs, the init script exited silently and SSH answered `Connection refused`; a forced
  reinstall of `openssh` fixed it and an SSH login from Windows worked. ADR-0015.
- 2026-10-23: the layer `meta-gateway` (ADR-0016) makes Wi-Fi start at boot with `nl80211`;
  after a flash and a reboot, SSH from Windows worked with no `ifup wlan0`. The layer and
  the new `bblayers.conf` are copied into `linux-gateway/`.
- 2026-10-23: `ENABLE_SPI_BUS` (`dtparam=spi=on`), and the Wi-Fi network baked into the image
  from `~/yocto/secrets/` (outside the repo, `.gitignore` rule for `secrets/`). The Pi joined
  Wi-Fi and accepted SSH with nothing typed on the board.
- 2026-10-24: recipe `spi-frame-test` (C, in `meta-gateway`) added to the image. With the
  Nucleo wired to the Pi, 10 of 10 frames had a valid CRC (`T=21.58 C`, `P=1015.7 hPa`); the
  first frame was the old `seq=0` one.
- 2026-10-24: ADR-0017: the image recipe `gateway-image`, built up in four tested steps from
  `packagegroup-core-boot`. Root filesystem 142 MB to 37 MB, packages 2015 to 76, image 78 MiB to
  35 MiB. Three failures on the way, each found from a log: the firmware symlink package
  (`...bcm43430`), the exclusion that did nothing because kernel packages are renamed with the
  version, and the locale packages that were requests and not recommendations. Boots, Wi-Fi up,
  10 of 10 SPI frames. A second SPI run showed the same first frame (`seq=0`) as the first run.
- 2026-10-24: root has a password. The hash (`openssl passwd -6`) is in `~/yocto/secrets/root.hash`,
  outside the repo, and `gateway-image.bb` writes it into `/etc/shadow` after the root filesystem
  is built; the build stops if the file is missing. `IMAGE_FEATURES` moved from `local.conf` into
  the recipe, without `empty-root-password` and `allow-empty-password`. On the board the serial
  console and SSH both ask for the password, and an empty one is refused over SSH.
- 2026-10-25: `USE_VT = "0"` removes the `getty` on `tty1` (checked in `inittab` and with `ps`).
  `DISABLE_VC4GRAPHICS = "1"` was tried and undone: `cairo` failed a QA check because of an
  out-of-date `cairo_%.bbappend` in `meta-raspberrypi`. `kbd` and `keymaps` (1.7 MB) left on
  purpose. Image `...rootfs-20261025113045.wic.bz2` (37 MB, 76 packages, 35 MiB). ADR-0017.
