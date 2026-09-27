# STM32 → Embedded Linux IoT Gateway

**Status:** Phase 1 in progress (bare-metal I2C + FreeRTOS on STM32F411RE).
See `docs/architecture.md` for the current, real state of the system.

## What this is

A portfolio project demonstrating industry-grade embedded systems practices,
built as a bridge between academic microcontroller/OS coursework and the
production practices actually used in IoT, automotive, and industrial
embedded roles.

The system (final target — see phases below): a battery-friendly sensor
node built on an STM32F411RE reads environmental data over I2C, transfers
it via DMA-driven SPI to a Raspberry Pi Zero 2 W running a custom-built
Linux image, where a kernel-level driver exposes the data to a user-space
MQTT publisher.

This is **not** a "GPIO blink + Raspberry Pi OS + Python script" project.
Specifically, it deliberately avoids the shortcuts that separate hobbyist
work from production embedded engineering:

| Shortcut avoided | What this project does instead |
|---|---|
| Dynamic memory (`malloc`/`free`) in firmware | Static allocation only (`xTaskCreateStatic`, etc.) — no heap fragmentation, no non-deterministic timing |
| CPU polling for bulk transfers | DMA-driven SPI, freeing the CPU and keeping timing deterministic |
| Off-the-shelf Raspberry Pi OS | Custom-built minimal Linux image (Yocto/Buildroot) |
| `spidev` / user-space "read a file" hacks | A real Linux kernel driver (`probe`/`remove`, character device, Device Tree binding) |
| `printf` debugging | Logic analyzer + `sigrok`/`PulseView` protocol-level signal analysis |

## Project phases

1. **Phase 1 — Hardware & RTOS** *(current)*: bare-metal I2C driver for the
   BME280 sensor, FreeRTOS task structure, static memory allocation only.
2. **Phase 2 — DMA & Logic Analysis**: SPI + DMA transfer to the gateway,
   verified with a logic analyzer.
3. **Phase 3 — Embedded Linux & Yocto**: custom minimal Linux image for the
   Raspberry Pi Zero 2 W.
4. **Phase 4 — Kernel Modules & IoT**: Linux kernel SPI driver, Device Tree
   overlay, MQTT publishing pipeline.

## Repository layout

```
firmware/       Keil MDK-ARM project, STM32 C code (Phase 1-2)
tools/          scripts, test clients, capture-processing helpers
docs/
  architecture.md   current system architecture (living document)
  decisions/        Architecture Decision Records (ADRs)
  captures/         logic analyzer exports/screenshots
  budget.md         hardware cost tracking
```

## Hardware

- STM32 Nucleo-F411RE (Cortex-M4 @ 84MHz, 128KB SRAM)
- Bosch BME280 (temperature / humidity / pressure, I2C)
- Raspberry Pi Zero 2 W (Cortex-A53, 512MB RAM, Wi-Fi) — Phase 3+
- USB logic analyzer, FX2LP-based, 8ch/24MHz, used with `sigrok`/`PulseView`

See `docs/budget.md` for full cost breakdown.

## Design decisions

Non-trivial design choices are recorded as ADRs in `docs/decisions/`, e.g.
why static allocation, why DMA over polling, why a custom Linux image over
Raspberry Pi OS. Start there for the reasoning behind the architecture.

## Reproducing this

*(To be filled in as each phase is completed — currently no working
firmware yet; this section will document required tools, build steps, and
flashing instructions for `firmware/`.)*