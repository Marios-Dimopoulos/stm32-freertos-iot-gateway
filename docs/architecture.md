# Architecture

This document reflects the **actual current state** of the system, not the
target design. Update it as subsystems are completed — don't let it drift
into aspiration.

## Current status: Phase 1, FreeRTOS configured, no application code yet

No application firmware exists yet: no `main.c`, no tasks, no drivers.
The project has **not been built yet**. `FreeRTOSConfig.h` is complete; the
first build now needs the application-side pieces listed under "Known gaps".

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
  `.\FreeRTOS\config`.
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
| Optional API functions | ✅ done | `INCLUDE_vTaskDelay 1`, `INCLUDE_xTaskDelayUntil 1` (others stay at default 0) |

### Known gaps (real current state, not bugs)

- **CPU clock is 16 MHz (HSI), not 84 MHz.** No PLL/clock-tree setup code
  exists yet. `system_stm32f4xx.c` leaves the chip on HSI after reset
  (`SystemCoreClock = 16000000`). The 84 MHz target in CLAUDE.md is not
  implemented.
- `vApplicationGetIdleTaskMemory()` (static-only allocation) and
  `vApplicationStackOverflowHook()` (overflow check method 2) are not
  implemented yet; the link will fail until they exist.
  `vApplicationGetTimerTaskMemory()` is NOT needed (`configUSE_TIMERS 0`).
- No `main.c` yet (must call `SystemCoreClockUpdate()` before
  `vTaskStartScheduler()`, see ADR-0003).
- No check yet that the C library's `malloc` isn't pulled in (needs a
  successful build and a look at the `.map` file).

### Architecture decisions so far

- ADR-0001: static-only memory allocation for FreeRTOS objects.
- ADR-0002: GCC/ARM_CM4F FreeRTOS port with Arm Compiler 6.
- ADR-0003: RTOS tick derived from `SystemCoreClock`, at 1 kHz.

Below is the target shape for Phase 1, kept here as a near-term plan,
clearly marked as such.

## Phase 1 target (not yet implemented)

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

- One FreeRTOS task reads the BMP280 over I2C1 (PB8/PB9) at a fixed
  interval.
- **Sensor: Bosch BMP280** (temperature + pressure; no humidity, unlike the
  BME280). Chip ID register `0xD0` reads `0x58`. I2C address `0x76` or
  `0x77` depending on the SDO pin.
- **Debug logging:** the sensor task prints readings directly over USART2
  (PA2 = TX, PA3 = RX), which the Nucleo's ST-LINK exposes as a virtual COM
  port. This is a blocking, debug-only aid for bring-up, not part of the
  data path (that is SPI + DMA). No dedicated logging task.
- No MPU-6050 IMU: it was dropped from the design.
- All RTOS objects (the task itself, any queue used to hand data off) are
  statically allocated.
- No SPI/DMA/gateway work yet — that's Phase 2.

### Task set and priorities (decided)

Task priority: higher number = more urgent. `configMAX_PRIORITIES = 3`.

| Priority | Task | Phase | Why this level |
|---|---|---|---|
| 2 | Transmit task (SPI + DMA to the Pi) | 2 (not written) | The Pi is the SPI master and clocks data out when it decides, so the deadline is imposed from outside. |
| 1 | Sensor task (BMP280 over I2C1, plus debug prints) | 1 (not written) | Period chosen by us; a few ms of lateness is harmless. |
| 0 | Idle | kernel | Runs when nothing else is ready. |

Interrupt levels are a separate system (lower number = more urgent): ISRs
that call `...FromISR()` functions must be at level 5 or a larger number.

## Open questions / not yet decided

- Clock source for the PLL: HSI (internal 16 MHz RC) or HSE from the
  ST-LINK's 8 MHz MCO output (depends on Nucleo board revision / solder
  bridges).
- Exact sensor read interval.
- Whether raw or calibration-compensated BMP280 values are computed
  on-device vs. passed through raw (compensation requires the factory
  trimming parameters stored in the sensor's NVM — see BMP280 datasheet
  §3.11.2 "Trimming parameter readout" and §3.11.3 "Compensation formula").
- Whether a queue is used to hand data from the sensor task to a
  (currently nonexistent) transmit task, or whether Phase 1 is entirely
  self-contained and just gets a value into a global static buffer to make
  the RTOS/I2C interaction real first.

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