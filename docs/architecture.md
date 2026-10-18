# Architecture

This document reflects the **actual current state** of the system, not the
target design. Update it as subsystems are completed — don't let it drift
into aspiration.

## Current status: Phases 1 and 2 (firmware) work end to end on the board; Phases 3 and 4 not started

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
- Noise on NSS: spikes on that line are ignored by the interrupt handler
  (decision 8). With the wires still, 7 spikes were seen in 3000 transactions and
  none damaged a frame; moving the wires by hand corrupts frames, which the Pi
  detects with the CRC, and the next frame is good.
- The sensor failing travels in the frame: with SDA unplugged the Pi gets frames
  with status `NACK` and zeros, then `NO_READING`, then `OK` when it is back.

**What is not verified:** nothing about the real signals (no logic analyzer in use
yet: the cause of the NSS spikes, the real minimum gap between transactions and
the true clock frequency are not measured); the first read after an idle time
returns a stale frame (see the known gaps).

**What exists:** the Keil project (`firmware/stm32-sensor-node/`) with the
FreeRTOS kernel and eleven application modules (clock, uart, i2c, bmp280, format,
frame, spi, sensor task, transmit task, hardfault, main), ten ADRs (`docs/decisions/`), the
test script `tools/spi_frame_test.py`, the README, and this document.
**What does not exist:** the logic analyzer captures (an analyzer is on order),
and the whole Linux side (Phases 3 and 4).

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
  which nothing reads yet); the public functions assert that the scheduler has
  started. **Verified on the board with the BMP280 at 0x76:** register write,
  chip ID 0x58, the 6-byte burst and the 24 trimming bytes; a missing sensor
  gives NACK without hanging; the sensor-disconnect test recovers by itself.
  Grounding SDA gave TIMEOUT on 2026-10-06 and NACK on 2026-10-08 (not
  investigated). **Limits:** no bus clear (a sensor holding SDA low is not
  freed); the 3-or-more-byte receive sequence is from memory of RM0383 and has
  not been compared with the manual's figure; the pull-ups of the breakout were
  never measured. A sensor locked out by CSB needs a power cycle, which no I2C
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
  3.3 V. The register layouts and codes are from memory of the datasheet and
  have not been checked against its text. Decisions: ADR-0009.
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

- **Nothing about the real signals has been measured.** No logic analyzer has been
  used yet (one is on order), so these are open: the true clock frequency (the
  HSI is about 1% accurate); the real **minimum gap** between two SPI transactions
  (no failure was seen down to back-to-back reads from Python, whose own overhead is
  probably 100 to 300 microseconds, not measured; a kernel driver or a C program may
  be much faster, so the Phase 4 driver has to enforce a delay); the re-arm time;
  the 1 s sensor period (only checked by the lines in Tera Term).
- **Spikes on NSS** (ADR-0010, decision 8): about 7 in 3000 transactions with the
  wires still (0.2%), many when the wires are moved. They are ignored by the
  interrupt handler, but their cause is only suspected (long jumpers and a
  breadboard in the path), not measured. Frames hit by a real loss of contact are
  corrupted, and the CRC finds them; the next one is good.
- **The first read after an idle time is stale.** The buffer is refilled right
  after each transaction and nothing refreshes it, so a Pi that has not read for a
  minute gets a frame about a minute old (the sequence number shows it). Harmless
  for a poller that reads every period; to be decided with the Phase 4 driver
  (read twice, or refresh the buffer between transactions).
- The test master is the stock Raspberry Pi OS with Python `spidev`: a temporary
  jig (ADR-0010), not the final image.
- The SPI `OVR` flag (received bytes are never read) has not been examined,
  because nothing has gone wrong that it would explain.
- **Stacks.** Measured on 2026-10-18 after 60 reads from the Pi, as lower bounds:
  sensor task 96 words peak of 256, transmit task 39 words of 128 (the placeholder
  path did not run, and the NACK paths were not exercised). Idle: at least 21 words
  actually written (the `0xA5` pattern said 14, but R4-R11 were saved with the fill
  value, so the pattern was forged by data), 128 words allocated (ADR-0005).
- **Clock accuracy is that of the HSI (about 1%).** The timeout/error paths of
  `clock_init_84mhz()` have never been triggered. `SystemCoreClock` reports HCLK
  only: USART2 and I2C1 must use the 42 MHz APB1 clock.
- `vApplicationGetTimerTaskMemory()` is NOT needed (`configUSE_TIMERS 0`).
- The I2C driver has no bus clear (a sensor holding SDA low is not freed), its
  3-or-more-byte receive sequence has not been compared with the manual's
  figure, and nothing reads `i2c_recovery_count()`.
- The BMP280 can be locked out of I2C by a glitch on CSB until its power is
  cycled; the sensor task reports it and keeps trying, but only a power cycle
  fixes it.
- `docs/captures/` has only a terminal screenshot. `docs/budget.md` still lists
  the BMP280 as not purchased and has no figures for the new purchases.
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

### Phase 2: what is left

1. **The remaining tests of the plan**, with `tools/spi_frame_test.py`: the Pi
   aborting a run at random moments (the Decision 7 property) and a 30-minute run.
   (The stack measurement of both tasks is done: ADR-0005.)
2. **The logic analyzer** (a Raspberry Pi Pico with the sigrok-pico firmware is on
   order; PulseView on the PC). Capture NSS, SCK, MISO, MOSI, SCL, SDA and the
   USART2 TX in one trace, triggered on NSS falling, to: see the spikes on NSS and
   find their cause; measure the re-arm time and so the real minimum gap; check the
   SPI clock; check the UART bit time against the HSI clock error. The captures
   go in `docs/captures/`; that is what Phase 2 promised.
3. **Wiring**: shorter jumpers, the SPI wires direct between the Pi header and the
   Nucleo (no breadboard in the path), a firm ground next to SCK. The counter
   `ulNssGlitchCount` is the way to measure the improvement.
4. **A quiz** on the SPI, DMA and re-arm material (the project rules ask for one
   after a milestone).
5. Update `docs/budget.md` with the real purchases (BMP280, the Pico, the USB-UART
   adapter, the resistor kit).

### Small leftovers (optional; none blocks Phase 3)

- Compare the I2C 3-or-more-byte receive sequence with the RM0383 figure, and
  check the BMP280 register descriptions in the code against the datasheet text
  (the follow-ups of ADR-0007 and ADR-0009).
- Learning checkpoints: quizzes done for the clock module, the UART and the I2C
  driver (questions 1 to 7); not done yet for the formatter, the BMP280 module,
  the sensor task, the FreeRTOS configuration, and the SPI link.

### Phases 3 and 4

Not started. No decisions yet beyond the plan (Yocto or Buildroot is undecided). For
Phase 3 a USB-to-UART adapter (3.3 V) is on order: the serial console is how a custom
image that does not boot is debugged. Phase 4 has two requirements that come from
Phase 2: the kernel driver must leave a minimum delay between SPI transfers, and it
must decide what to do about the stale first read (read twice, or refresh the buffer
on the node).

## Open questions / not yet decided

- What produces the spikes on NSS (wiring is suspected; to be shown with the
  analyzer).
- How to deal with the stale first read after an idle time (ADR-0010, consequences).
- The real minimum gap between SPI transactions (to be measured).
- Whether the HSI accuracy (about 1%) stays good enough, or the clock moves to
  the ST-LINK's 8 MHz HSE (needs `HSE_VALUE=8000000`, see ADR-0006). To be
  decided after the real frequency has been measured (logic analyzer and the
  MCO pin).
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
