# Architecture

This document reflects the **actual current state** of the system, not the
target design. Update it as subsystems are completed — don't let it drift
into aspiration.

## Current status: Phase 1, only a few things have been done

No firmware has been written yet. The only work that has been done
for the phase 1 is the following: I installed the right packs 
from the `Pack Installer`. Then i configured, a bit, the `Options for Target...`
according to some tips my teacher gave me. Then i created the project directory
and downloaded the FreeRTOS source code. I picked the whole `include\` directory
and the `event_groups.c`, `list.c`, `queue.c`, `tasks.c`, `timers.c`
`port.c` and `portmacro.h` files. Then i managed the project items, by creating 
in the `Target_1` the `FreeRTOS Core` directory (which includes the `event_groups.c`,
`list.c`, `queue.c`, `tasks.c`, `timers.c` files) and the `FreeRTOS Portable` directory
(which includes only the `port.c` file). Lastly, i configured the `include Paths`
so the preprocessor is able to find the .h files.

This file will be filled in as work progresses. Below is the target shape for Phase 1,
kept here as a near-term plan, clearly marked as such.

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