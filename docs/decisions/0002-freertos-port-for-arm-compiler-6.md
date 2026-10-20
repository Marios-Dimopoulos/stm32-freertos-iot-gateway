# ADR-0002: Use the GCC/ARM_CM4F FreeRTOS port with Arm Compiler 6

**Status:** Accepted
**Date:** 2026-09-29
**Phase:** 1

## Context

FreeRTOS keeps its CPU- and compiler-specific code (context switching,
SysTick setup, the SVC/PendSV handlers, critical sections) in a "port":
`portable/<Compiler>/<Architecture>/port.c` plus `portmacro.h`. The compiler
part of that path matters because these files contain assembly, and each
compiler family accepts a different assembly syntax.

The project was first set up with `portable/RVDS/ARM_CM4F`, which looks like
the natural choice for Keil ("RVDS" is Arm's compiler lineage). But the Keil
project uses **Arm Compiler 6** (`armclang` V6.24, `<uAC6>1</uAC6>` in the
`.uvprojx`). The RVDS port is written for the older **Arm Compiler 5**
(`armcc`). It implements the SVC handler, first-task start, VFP enable,
PendSV handler and IPSR read as *embedded assembler* functions
(`__asm void vPortSVCHandler( void ) { ... }`). That is an armcc-only
extension, which armclang does not support, so `port.c` would not compile.

This was found by reading the source before the first build, not from a
compiler error.

## Options considered

### Option A: Keep the RVDS port and switch the project to Arm Compiler 5
- How it works: Select AC5 in *Options for Target → Target → ARM Compiler*.
- Pros: The RVDS port compiles unchanged.
- Cons: Arm has deprecated AC5, and recent MDK versions no longer ship it.
  Building a new portfolio project on a toolchain that is being retired is
  a bad signal and a maintenance dead end.

### Option B: Use the GCC/ARM_CM4F port with Arm Compiler 6
- How it works: Replace `port.c` / `portmacro.h` with the files from
  `portable/GCC/ARM_CM4F` (same kernel release, V11.3.1), and point the
  include path and the `FreeRTOS Portable` group at them. armclang is built
  on Clang/LLVM and accepts GCC-style inline assembly (`__asm volatile(...)`),
  which is what this port uses.
- Pros: Works with the current, supported compiler. It's FreeRTOS's own
  guidance for Keil + AC6. The same port code is used by the GCC
  (arm-none-eabi) toolchain, so it is heavily exercised.
- Cons: The directory name ("GCC" in a Keil project) confuses readers at
  first; this ADR is why it's there.

## Decision

Option B. Arm Compiler 6 is the supported toolchain, and the GCC port is the
one written in the assembly syntax that AC6 accepts. The folder name refers
to the assembly dialect, not to the compiler actually used.

## Consequences

- `port.c` compiles under AC6. The first full build then failed on missing
  `FreeRTOSConfig.h` settings (clock/tick, priorities, etc.), until the config file
  was complete.
- Include path is now `.\FreeRTOS\portable\GCC\ARM_CM4F`, and the
  `FreeRTOS Portable` group references the GCC `port.c`.
- Kernel upgrades must take `port.c` / `portmacro.h` from `portable/GCC/ARM_CM4F`
  of the same release as the core files. Mixing port and core versions is
  not supported.
- The `ARM_CM4F` port assumes the FPU is enabled
  (`__FPU_PRESENT` / *Floating Point Hardware: Single Precision* in Keil
  Target options). Confirmed: the `.uvprojx` has `<RvdsVP>2</RvdsVP>`
  (single-precision FPU enabled).
- The startup file (`startup_stm32f411xe.s`) names the handlers `SVC_Handler`,
  `PendSV_Handler` and `SysTick_Handler`, while the port defines `vPortSVCHandler`,
  `xPortPendSVHandler` and `xPortSysTickHandler`. They are mapped to each other with
  `#define`s in the interrupt section of `FreeRTOSConfig.h`.

## Interview angle

"A FreeRTOS port is really chosen by the compiler's assembly dialect, not by
the IDE. Keil with Arm Compiler 6 is Clang-based, so it takes the GCC port.
The RVDS port relies on armcc's embedded assembler, which AC6 dropped. I
caught that by reading `port.c` before the first build."
