# Project: STM32 → Embedded Linux Gateway IoT Pipeline

## Purpose

This is a portfolio project built to prepare for Embedded Software Engineer /
Embedded Linux Engineer roles. The goal is **not** just a working system —
it is to deeply understand every design decision well enough to defend it
in a technical interview, and to produce a GitHub repository that
demonstrates industry-level practices (not hobbyist/academic shortcuts).

Full background and rationale: see `docs/plan.md` (original research/plan
document), and `docs/architecture.md` for the current state of the project.

## Hardware

- MCU: STM32F411RE on Nucleo-F411RE board, ARM Cortex-M4 @ up to 100MHz
  (project runs at 84MHz via PLL), 128KB SRAM, FPU, NVIC.
- Toolchain for firmware: Keil MDK-ARM, Windows. This is Phase 1-2 work
  and lives under `firmware/`.
- Sensor: BMP280 (temperature/pressure, no humidity) over I2C1
  (SCL: PB8, SDA: PB9). Default I2C address 0x76 or 0x77 depending on SDO pin.
- No secondary sensor: the MPU-6050 IMU was dropped from the design.
- Debug output: USART2 (PA2: TX, PA3: RX) via the Nucleo's ST-LINK virtual
  COM port, viewed in Tera Term. Debug-only, not part of the data path.
- Data link MCU → gateway: SPI1 (NSS: PA4, SCK: PA5, MISO: PA6, MOSI: PA7),
  using DMA for transfer (never CPU polling for bulk transfers).
- Gateway: Raspberry Pi Zero 2 W, ARM Cortex-A53 (arm64), 512MB RAM, Wi-Fi.
  Runs a custom-built Linux image (Yocto or Buildroot — decision recorded in
  an ADR, see `docs/decisions/`). Never the stock Raspberry Pi OS for the
  final deliverable. This is Phase 3-4 work and lives under
  `linux-gateway/`.
- Debugging: USB logic analyzer, Cypress FX2LP-based, 8 channels, 24MHz max
  sample rate. Used with the open-source `sigrok`/`PulseView` toolchain.
  Captures and exports are stored in `docs/captures/`.

## Environments (important — this project spans two OSes)

- **Windows, native Claude Code install, at `firmware/`**: used for Phase 1-2
  (STM32 bare-metal + FreeRTOS + DMA), built with Keil MDK-ARM. Claude Code
  cannot flash or debug the board through the Keil GUI — the user does that
  manually. Claude's job here is code, register-level explanations, and
  static analysis of the firmware source.
- **WSL2 Claude Code, on a clone of this same repo, under `linux-gateway/`**:
  used for Phase 3-4 (Yocto/Buildroot, kernel module, Device Tree, MQTT).
  These builds only work on Linux. Keep this clone's build artifacts out of
  git (see `.gitignore`) — Yocto build directories are huge.
- Changes made in either environment should eventually be synced back into
  one canonical repo history. Don't assume the other environment's latest
  state; if something depends on it, ask or check.

## Hard engineering constraints (do not violate silently)

- **No dynamic memory allocation in the FreeRTOS firmware.** All tasks,
  queues, semaphores and mutexes must be created with the static-allocation
  APIs (`xTaskCreateStatic`, `xQueueCreateStatic`, etc.), never `malloc`/
  `xTaskCreate`. If a library forces dynamic allocation, flag it — do not
  silently work around it.
- **SPI clock must stay at or below ~6MHz.** The logic analyzer samples at
  24MHz max; per Nyquist–Shannon (and the ~4-8x oversampling needed for
  clean square-wave edge capture), anything faster than ~6MHz cannot be
  reliably captured for debugging. Always check the prescaler math before
  changing SPI speed.
- **Total additional hardware budget: 50 EUR.** Do not suggest purchasing
  hardware beyond what's listed in `docs/budget.md` without flagging the
  cost impact first.
- Bulk data transfers (SPI, and any future DMA use) must use DMA, not
  CPU polling or blocking transmit loops. If you ever write a blocking
  transfer loop "to get something working first," say so explicitly and
  flag it as temporary/non-final.

## How we work together (this is the most important section)

This is a learning project first, a deliverable second. Follow these
rules even when they slow things down:

1. **Explain before implementing.** For any new feature or any
   architecturally significant piece of code (RTOS primitives, DMA
   configuration, SPI/I2C register setup, kernel module structures, Device
   Tree bindings, Yocto recipe logic), first explain the 2-3 realistic
   approaches and their trade-offs. Wait for the user to choose before
   writing code, unless they've explicitly said "just implement it."
2. **No code the user can't explain.** Comments should explain *why* a
   decision was made, not restate *what* the line does. If you write
   something non-obvious (a magic timing value, a register bit trick),
   comment the reasoning or link the datasheet section. Let the comments be 
   as extended and large as they need to be, in order for me to fully understand
   everything.
3. **Small, reviewable steps.** Don't implement an entire subsystem in one
   shot. Implement one piece, explain it, stop, and let the user confirm
   before continuing to the next piece.
4. **Debugging is hypothesis-driven.** When something doesn't work: state
   your hypothesis, state what evidence would confirm or refute it
   (specifically: what we'd expect to see in PulseView, in a UART debug
   print, or in a register dump), then test the hypothesis before jumping
   to a fix. Don't silently patch symptoms.
5. **Quiz after each milestone.** When a feature/phase milestone is
   complete, generate 3-5 interview-style questions about what was just
   built (the kind an interviewer would actually ask), so the user can
   check their own understanding.
6. **Don't over-scaffold.** Don't add abstractions, config options, or
   generality that isn't needed yet "for the future." This is a learning
   project — simple and explicit beats clever and generic.

## Documentation & portfolio conventions

- **Architecture Decision Records (ADRs):** any non-trivial design choice
  (Yocto vs Buildroot, static vs dynamic memory, DMA mode choice, kernel
  module vs user-space driver, MQTT broker choice, etc.) gets an ADR in
  `docs/decisions/`, using `docs/decisions/adr-template.md`. Propose one
  whenever such a decision is made — don't wait to be asked.
- **Commit messages explain reasoning, not just the change.** Bad:
  `fix SPI bug`. Good: `Fix SPI CPOL/CPHA mismatch causing garbage RX data
  — STM32 was sampling MISO on the falling edge while the RPi peripheral
  drove it on the rising edge; confirmed via PulseView capture in
  docs/captures/spi-cpol-bug.sr. See ADR-0004.`
- **README.md at repo root** should read like a portfolio piece: problem
  statement, architecture diagram, what makes this "industrial-grade"
  rather than a toy (static allocation, DMA, custom Linux image, kernel
  driver, not user-space spidev hacks), and how to reproduce it.
- Keep `docs/architecture.md` updated as subsystems are completed — it
  should always reflect the current, real state of the system, not the
  aspirational one.

## Repository layout

```
firmware/          Keil MDK-ARM project, STM32 C code (Phase 1-2, Windows)
linux-gateway/      Yocto/Buildroot layer + kernel module (Phase 3-4, Linux)
tools/              scripts, MQTT test clients, capture-processing helpers
docs/
  architecture.md   current system architecture (living document)
  decisions/        ADRs
  captures/         logic analyzer exports/screenshots
  budget.md         hardware cost tracking
  plan.md           The full-original plan of the whole project
```

## Project phases (for context on where things stand)

1. **Phase 1 (Hardware & RTOS):** bare-metal I2C driver for BMP280, FreeRTOS
   integration, static memory allocation only.
2. **Phase 2 (DMA & Logic Analysis):** SPI + DMA transfer to the gateway,
   logic analyzer verification of timing/protocol correctness.
3. **Phase 3 (Embedded Linux & Yocto):** custom minimal Linux image for the
   Raspberry Pi Zero 2 W via Yocto (or Buildroot), SPI/UART enabled via
   machine config.
4. **Phase 4 (Kernel Modules & IoT):** Linux kernel SPI driver (`probe`/
   `remove`, character device), Device Tree overlay binding it to the
   physical SPI bus, MQTT (Mosquitto) publishing pipeline.

Check `docs/architecture.md` for the actual current phase/status before
assuming which stage the project is at.