# Architecture

This document reflects the **actual current state** of the system, not the
target design. Update it as subsystems are completed — don't let it drift
into aspiration.

## Current status: Phase 1, FreeRTOS integration in progress

No application firmware exists yet: no `main.c`, no tasks, no drivers.
The project has **not been built yet**. Until `FreeRTOSConfig.h` is complete,
a build is expected to fail on missing config macros.

### Toolchain / project setup (done)

- Keil MDK-ARM project: `firmware/stm32-sensor-node/stm32-sensor-node.uvprojx`.
- Compiler: **Arm Compiler 6** (armclang V6.24).
- Single-precision FPU enabled in Target options (required by the
  `ARM_CM4F` port).
- Device support from the Keil STM32F4 DFP: `startup_stm32f411xe.s`,
  `system_stm32f4xx.c` (under `RTE/Device/STM32F411RETx/`).

### FreeRTOS kernel (V11.3.1): source integrated, configuration partial

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

### `FreeRTOSConfig.h`: sections completed

| Section | Status | Settings |
|---|---|---|
| Memory allocation model | ✅ done | `configSUPPORT_STATIC_ALLOCATION 1`, `configKERNEL_PROVIDED_STATIC_MEMORY 0`, `configSUPPORT_DYNAMIC_ALLOCATION 0`, `configTOTAL_HEAP_SIZE 0` (ADR-0001) |
| Clock / tick | ⏳ next | `configCPU_CLOCK_HZ`, `configTICK_RATE_HZ`, preemption, time slicing, tick width |
| Task priorities / stack sizes | ❌ not started | |
| Interrupt priorities (NVIC/BASEPRI) | ❌ not started | |
| Port handler mapping (SVC/PendSV/SysTick → startup file names) | ❌ not started | |

### Known gaps (real current state, not bugs)

- **CPU clock is 16 MHz (HSI), not 84 MHz.** No PLL/clock-tree setup code
  exists yet. `system_stm32f4xx.c` leaves the chip on HSI after reset
  (`SystemCoreClock = 16000000`). The 84 MHz target in CLAUDE.md is not
  implemented.
- `vApplicationGetIdleTaskMemory()` / `vApplicationGetTimerTaskMemory()`
  are not implemented yet. They are required by static-only allocation, so
  the link will fail until they exist.
- No check yet that the C library's `malloc` isn't pulled in (needs a
  successful build and a look at the `.map` file).

### Architecture decisions so far

- ADR-0001: static-only memory allocation for FreeRTOS objects.
- ADR-0002: GCC/ARM_CM4F FreeRTOS port with Arm Compiler 6.

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
│  │ SensorTask │──I2C1──►BME280
│  └────────────┘             │
│        │                    │
│        ▼                    │
│  (Phase 2: SPI+DMA out)     │
└─────────────────────────────┘
```

- One FreeRTOS task reads the BME280 over I2C1 (PB8/PB9) at a fixed
  interval.
- All RTOS objects (the task itself, any queue used to hand data off) are
  statically allocated.
- No SPI/DMA/gateway work yet — that's Phase 2.

## Open questions / not yet decided

- `configCPU_CLOCK_HZ`: a fixed `84000000UL` or the CMSIS `SystemCoreClock`
  variable. `configTICK_RATE_HZ`: 1000 or 100.
- Clock source for the PLL: HSI (internal 16 MHz RC) or HSE from the
  ST-LINK's 8 MHz MCO output (depends on Nucleo board revision / solder
  bridges).
- Exact sensor read interval.
- Whether raw or calibration-compensated BME280 values are computed
  on-device vs. passed through raw (compensation requires the factory
  calibration parameters stored in the sensor's NVM — see BME280 datasheet
  §4.2.3).
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