# Architecture

This document reflects the **actual current state** of the system, not the
target design. Update it as subsystems are completed — don't let it drift
into aspiration.

## Current status: Phase 1 working on the board (sensor task at 84 MHz)

FreeRTOS builds, links and runs on the Nucleo at **84 MHz** (HSI + PLL). One
task, the sensor task, reads the BMP280 over I2C1 once a second and prints
`T=23.64 C  P=1019.69 hPa` over USART2 (115200 baud, ST-LINK virtual COM port,
Tera Term). The readings are stable and plausible for the room. Register values
and SysTick `LOAD = 83999` were checked in the debugger; the true clock
frequency has not been measured with an instrument (no logic analyzer yet).
If the sensor stops answering the task prints the cause and initialises it
again on the next period. **Verified on the board:** disconnecting the sensor
(or its lines) gives `NACK`, and everything comes back by itself when it is
reconnected; a falling edge on CSB locks the sensor out of I2C until its power
is cycled (see ADR-0007). **Still to do for Phase 1:** the README, the ADR for
writing own register-level drivers, an ADR for the BMP280 module, and a fresh
reading of the footprint from the next build.

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
  `SystemCoreClockUpdate()`, `uart_init()`, `i2c_init()`, `sensor_task_create()`,
  `vTaskStartScheduler()` (ADR-0003). The temporary blink task and the LED code
  are gone (PA5 is SPI1_SCK in the Phase 2 design); they are in the git history.
- `sensor_task.c` / `sensor_task.h`: the sensor task, which owns the BMP280 and
  the I2C bus (priority 1, static 256-word stack + TCB, `vTaskDelayUntil` with a
  1 s period). Each period: if the sensor is not initialised it calls
  `bmp280_init()`; otherwise `bmp280_read()` and prints the line. **Any failure**
  is counted, printed as `BMP280 error: <cause> (failures: n)`, and the next
  period initialises the sensor again, so a sensor that was power-cycled
  recovers without resetting the MCU. Every line is built in a 64-byte buffer
  (sized for the worst case) and sent with one `uart_write()`. The stack was
  measured: 89 words peak (167 free), kept at 256 on purpose (ADR-0005).
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
- `clock.c` / `clock.h`: `clock_init_84mhz()` takes the CPU from the 16 MHz HSI
  to 84 MHz through the PLL (ADR-0006). Five steps in order: flash wait
  states (2) + prefetch + caches, bus prescalers (AHB /1, **APB1 /2 = 42 MHz**,
  APB2 /1), PLL numbers (M 8, N 168, P /4, source HSI), PLL on + bounded wait
  for `PLLRDY`, switch + bounded wait for `SWS`. Returns a status code and
  never hangs; on failure the CPU stays on the HSI. PLL limits are checked
  with `_Static_assert`. `main()` calls it before `SystemCoreClockUpdate()`.
- `uart.c` / `uart.h`: my own register-level USART2 driver (TX only, PA2 =
  AF7). `uart_init()` enables the USART2 and GPIOA clocks, sets PA2 as
  alternate function (AFR written before MODER), configures 8N1 at 115200 with
  the divisor `BRR = round(PCLK1 / baud)` computed from the real clock
  registers (365 = 0x16D at 42 MHz; it would still be readable on the HSI),
  enables the transmitter, and sets the interrupt priority before enabling it.
  `uart_write()` copies a whole message into a 256-byte static ring buffer
  inside one FreeRTOS critical section and sets `TXEIE`; `USART2_IRQHandler`
  (level 10, i.e. at or below the FreeRTOS line, checked with
  `_Static_assert`) sends one byte per interrupt and clears `TXEIE` when the
  buffer is empty. The critical section protects task against task (several
  tasks may print), the free-space check and the copy as one unit, and the
  shared `CR1` register (a benign race, kept anyway). **All or nothing:** if
  the message does not fit, nothing is stored, the drop counter
  (`uart_dropped_messages()`) is incremented and `UART_ERR_FULL` is returned, so
  a line is never cut and tasks' messages never interleave. Never waits. Tasks
  only, after the scheduler has started (not asserted, unlike the I2C driver).
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
  3.3 V. The formulas were also checked in Python (with C's integer rules)
  against the example values remembered from the datasheet (25.08 C, 100653 Pa);
  the C code itself has not been run on a PC. The only library function the
  module needs is the 64-bit division (`__aeabi_ldivmod`).
- Footprint, from the Keil build of 2026-10-08 20:05
  (`Listings/stm32-sensor-node.map`). That build still contained the temporary
  stack-report code (since removed), so the real numbers are slightly smaller;
  re-read them from the next build. Code 12,672 B, RO data 644 B (13.3 KB of
  flash in total), RAM 3,816 B of 128 KB (RW 8 B + ZI 3,808 B, including the
  1 KB main stack, the 512 B C-library heap reserved by the startup file, the
  two task stacks and the UART buffer). **No `malloc` is linked** (the only
  matches in the map are the library function `_get_argv_nomalloc`). Keil's
  static stack report gave 324 B for the start-up call chain on the main stack
  in an earlier build (interrupts not included).

### Drivers

The course/teacher drivers (ARM University Program, ST Standard Peripheral
Library, teacher-written I2C) are kept **locally only** in
`firmware/**/reference/` (git-ignored, outside the Keil project) as reading
material. The clock, USART2 and I2C1 drivers were written from scratch at
register level, and the BMP280 module is built on them (the compensation
formulas are Bosch's, as in the author's own `functions.c`). The ADR for the
decision to write own register-level drivers is still to be written.

### Known gaps (real current state, not bugs)

- **Clock accuracy is that of the HSI (about 1%)**, and the true frequency has
  not been measured outside the chip. The timeout/error paths of
  `clock_init_84mhz()` have never been triggered. `SystemCoreClock` reports
  HCLK only: USART2 and I2C1 must use the 42 MHz APB1 clock.
- `vApplicationGetTimerTaskMemory()` is NOT needed (`configUSE_TIMERS 0`).
- No timing has been measured with an instrument (no logic analyzer): the
  1 s sensor period is only checked by the lines in Tera Term.
- Measured stack peaks (ADR-0005): the sensor task 89 words (167 of its 256
  free); Idle at least 21 words actually written (the `0xA5` pattern said 14,
  but R4-R11 were saved with the fill value, so the pattern was forged by data).
  The Idle array is 128 words (`configMINIMAL_STACK_SIZE`).
- The startup file reserves a 512-byte C-library heap that nothing uses;
  setting `Heap_Size` to 0 would reclaim it and make any accidental `malloc`
  fail loudly.
- No HardFault handler that exposes the stacked PC and fault registers.
- Nothing prints the UART drop counter, and `uart_write()` does not assert that
  the scheduler has started (the I2C driver does).
- The I2C driver has no bus clear (a sensor holding SDA low is not freed), its
  3-or-more-byte receive sequence has not been compared with the manual's
  figure, and nothing reads `i2c_recovery_count()`.
- The BMP280 can be locked out of I2C by a glitch on CSB until its power is
  cycled; the sensor task reports it and keeps trying, but only a power cycle
  fixes it. The ADRs for writing own drivers and for the BMP280 module are not
  written yet.

### Architecture decisions so far

- ADR-0001: static-only memory allocation for FreeRTOS objects.
- ADR-0002: GCC/ARM_CM4F FreeRTOS port with Arm Compiler 6.
- ADR-0003: RTOS tick derived from `SystemCoreClock`, at 1 kHz.
- ADR-0004: interrupt priority threshold for FreeRTOS API calls at level 5.
- ADR-0005: task stacks sized by measurement with a margin; Idle stays at 128
  words; the sensor task was measured (89 words peak) and kept at 256.
- ADR-0006: CPU at 84 MHz from the HSI through the PLL, in its own module.
- ADR-0007: I2C1 driver: register-oriented polling API with tick-based timeouts and
  self-recovery.

Not written yet (decided to write them): an ADR for writing own register-level
drivers instead of using a vendor library, and an ADR for the BMP280 module
(normal mode, integer results, the status codes, init after every error).

## Phase 1 design (implemented: this is what runs)

```
┌─────────────────────────────┐
│  STM32F411RE (Nucleo)       │
│                             │
│  FreeRTOS, static allocation│
│                             │
│  ┌────────────┐             │
│  │ SensorTask │──I2C1──►BMP280
│  └────────────┘             │
│        │  └──USART2──►ST-LINK VCP (debug text, Tera Term)
│        ▼                    │
│  (Phase 2: SPI+DMA out)     │
└─────────────────────────────┘
```

- One FreeRTOS task reads the BMP280 over I2C1 (PB8/PB9) once a second.
- **Sensor: Bosch BMP280** (temperature + pressure; no humidity, unlike the
  BME280). Chip ID register `0xD0` reads `0x58`. I2C address `0x76` or
  `0x77` depending on the SDO pin.
- **Debug logging:** the sensor task prints readings directly over USART2
  (PA2 = TX; PA3 = RX is not used), which the Nucleo's ST-LINK exposes as a
  virtual COM port, by interrupt through a ring buffer, whole messages or
  nothing. This
  is a debug-only aid, not part of the data path (that is SPI + DMA). No
  dedicated logging task.
- No MPU-6050 IMU: it was dropped from the design.
- All RTOS objects (the task itself, any queue used to hand data off) are
  statically allocated.
- No SPI/DMA/gateway work yet — that's Phase 2.

### Task set and priorities (decided)

Task priority: higher number = more urgent. `configMAX_PRIORITIES = 3`.

| Priority | Task | Phase | Why this level |
|---|---|---|---|
| 2 | Transmit task (SPI + DMA to the Pi) | 2 (not written) | The Pi is the SPI master and clocks data out when it decides, so the deadline is imposed from outside. |
| 1 | Sensor task (BMP280 over I2C1, plus debug prints) | 1 (written) | Period chosen by me; a few ms of lateness is harmless. |
| 0 | Idle | kernel | Runs when nothing else is ready. |

Interrupt levels are a separate system (lower number = more urgent): ISRs
that call `...FromISR()` functions must be at level 5 or a larger number.

## Open questions / not yet decided

- How the sensor task hands its readings to the Phase 2 transmit task: a
  queue, or a shared static buffer protected some other way. Today the reading
  is only printed and nothing else uses it.
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
