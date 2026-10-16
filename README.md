# STM32 → Embedded Linux IoT Gateway

A sensor node on an **STM32F411RE** (FreeRTOS, static allocation only, my own
register-level drivers) that reads a **BMP280** over I2C and prints temperature
and pressure once a second. The plan is to send the data over DMA-driven SPI to a
**Raspberry Pi Zero 2 W** running a custom-built Linux image, where a kernel
driver hands it to an MQTT publisher.

**Status: Phase 1 works on the board. Phases 2 to 4 are not started.** The
file `docs/architecture.md` describes the real current state, and I keep it
honest: what is verified, and what is not.

This is a learning project built to prepare for embedded software and embedded
Linux roles. The goal is not only a working system but to be able to defend every
design decision, which is why the reasoning is written down in ADRs.

![Terminal output of the running node](docs/captures/phase1-terminal-output.png)

*Tera Term on the ST-LINK virtual COM port. The `Sensor task stack free` lines
come from temporary code that I used to measure the task's stack (ADR-0005); it is
removed now, so the node prints only the init line and the readings.*

## What works today (Phase 1)

| What | Evidence |
|---|---|
| **Static allocation only.** No `heap_x.c` is compiled, and `configSUPPORT_DYNAMIC_ALLOCATION` is 0. | The link map contains no `malloc` (the only match is the library function `_get_argv_nomalloc`). ADR-0001 |
| **84 MHz from the internal oscillator through the PLL**, in its own module with bounded waits and build-time checks of the PLL limits. | Register values and the SysTick reload (83999) checked in the debugger. ADR-0006 |
| **Interrupt-driven UART** with a ring buffer. A message is stored whole or dropped whole. | Verified in Tera Term, including a burst test of the drop behaviour. |
| **I2C1 master driver at register level.** Waits bounded in RTOS ticks, one error code per cause, a critical section only around the few operations with a deadline, and self-recovery after a bus error. | Chip ID, 6-byte and 24-byte reads on the real sensor. ADR-0007 |
| **BMP280 module** with Bosch's integer compensation (no floats, no `printf`). | Stable, plausible readings (see the screenshot). ADR-0009 |
| **Self-healing sensor task.** After any error the task initialises the sensor again on the next period. | Disconnected the sensor completely while it ran: `NACK` lines, then `OK` and readings again, with no reset. |
| **Stack sizes from measurement**, not guesses. | The sensor task peaks at 89 words of its 256. ADR-0005 |
| **A real failure found and documented.** A falling edge on the sensor's CSB pin locks it out of I2C until its power is cycled. | Reproduced twice on the board. ADR-0007 |

Size from the last build map: about 13 KB of flash and 3.8 KB of RAM (of 128 KB),
FreeRTOS included.

### What is not verified, or not done

- The real CPU clock frequency has never been measured with an instrument (the
  internal oscillator is about 1% accurate), and the error paths of the clock module
  have never been triggered.
- A sensor locked out by CSB needs a power cycle, which no I2C driver can fix.
- There is no logic analyzer yet, so no signal captures exist.

## Architecture

What runs today:

```
                 I2C1, 100 kHz                    USART2, 115200 8N1
   BMP280 ───────(PB8 SCL, PB9 SDA)───►  STM32F411RE  ───────(PA2 TX)───►  ST-LINK ──USB──► PC
 (address 0x76)                          Nucleo, 84 MHz                   (virtual COM)     Tera Term
                                              │
                          FreeRTOS, static allocation, 1 kHz tick
                          ┌─────────────────────────────────────────┐
                          │ Sensor task (priority 1, 1 s period)    │
                          │   bmp280_init / bmp280_read             │
                          │     └─ i2c driver (register level)      │
                          │   formatter ─► uart_write ─► ring buffer│
                          │                    └─ USART2 interrupt  │
                          │ Idle task (priority 0)                  │
                          └─────────────────────────────────────────┘
```

The plan (Phases 2 to 4):

```
   STM32 sensor node ──SPI + DMA──► Raspberry Pi Zero 2 W ──► kernel driver ──► MQTT ──► network
                                    (custom Linux image)     (Device Tree)
```

## Design decisions

Every non-trivial choice has an ADR in `docs/decisions/`, with the options I
considered, the one I chose and why, and what I did not verify.

| ADR | Decision |
|---|---|
| 0001 | Static-only memory allocation for the FreeRTOS objects |
| 0002 | The GCC/ARM_CM4F FreeRTOS port, with Arm Compiler 6 |
| 0003 | The RTOS tick derived from `SystemCoreClock`, at 1 kHz |
| 0004 | Interrupt priority threshold for FreeRTOS calls at level 5 |
| 0005 | Task stacks sized by measurement, with a margin |
| 0006 | CPU at 84 MHz from the HSI through the PLL, in its own module |
| 0007 | I2C1 driver: register-oriented polling API with tick-based timeouts and self-recovery |
| 0008 | Own register-level drivers instead of a vendor library |
| 0009 | BMP280 module: normal mode, integer results, initialise again after every error |

## Hardware

- STM32 Nucleo-F411RE (Cortex-M4 at 84 MHz, 128 KB SRAM)
- Bosch BMP280 breakout (temperature and pressure, I2C)
- Raspberry Pi Zero 2 W (Phase 3 onward)
- A USB logic analyzer (FX2LP, 8 channels, 24 MHz) with `sigrok`/`PulseView`,
  planned for Phase 2, not purchased yet

The extra hardware budget is 50 EUR. See `docs/budget.md`.

### Wiring the BMP280

| BMP280 | Nucleo |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | PB8 |
| SDA | PB9 |
| SDO | GND (address 0x76) |
| **CSB** | **3V3, wired firmly** |

CSB must stay high. A falling edge on it, even a loose wire, switches the sensor to
SPI mode until its power is cycled (see ADR-0007). Most breakouts already have
pull-ups on SCL and SDA; the firmware also enables the weak internal ones as a backup.

## Build and run

Needed:
- **Keil MDK-ARM** with Arm Compiler 6 (I used armclang V6.24) and the device pack
  `Keil.STM32F4xx_DFP.2.17.0`
- A Nucleo-F411RE connected over USB (the ST-LINK flashes it and provides the serial port)
- A serial terminal, for example Tera Term

Steps:
1. Open `firmware/stm32-sensor-node/stm32-sensor-node.uvprojx` in Keil.
2. Build, then download to the board. FreeRTOS V11.3.1 is in the repository,
   unmodified; the project's own configuration is in
   `FreeRTOS/config/FreeRTOSConfig.h`.
3. Open the ST-LINK virtual COM port in the terminal at **115200, 8N1**.

Expected output:

```
BMP280 init: OK
T=24.08 C  P=1017.61 hPa
T=24.08 C  P=1017.61 hPa
...
```

If the sensor is disconnected, you get `BMP280 error: NACK (failures: n)` every
second, and the readings come back by themselves when it is reconnected.

## Roadmap

| Phase | What | State |
|---|---|---|
| 1 | Hardware and RTOS: I2C driver, BMP280, FreeRTOS with static allocation | Working on the board |
| 2 | SPI + DMA transfer to the gateway, checked with a logic analyzer | Not started |
| 3 | Custom minimal Linux image for the Raspberry Pi Zero 2 W (Yocto or Buildroot, not decided yet) | Not started |
| 4 | Linux kernel SPI driver, Device Tree overlay, MQTT publishing | Not started |

Practices that belong to the later phases (DMA for bulk transfers, a custom Linux
image, a kernel driver instead of `spidev`, protocol-level debugging with a logic
analyzer) are goals, not achievements, until they exist.

## Repository layout

```
firmware/stm32-sensor-node/   Keil project
  Src/                        main, clock, uart, i2c, bmp280, format, sensor_task
  FreeRTOS/                   kernel V11.3.1 (unmodified) and the project's config
docs/
  architecture.md             the current state of the system (living document)
  decisions/                  Architecture Decision Records
  captures/                   screenshots and, later, logic analyzer captures
  budget.md                   hardware cost tracking
  plan.md                     the original plan of the whole project (in Greek)
tools/                        scripts and helpers (empty so far)
```

The `linux-gateway/` directory will appear in Phase 3.
