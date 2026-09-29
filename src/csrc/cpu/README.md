# src/csrc/cpu: the CPU cores behind the emulated boards

A board (the Braille Lite's `../blazie/bl_board.c`, later the Accent SA's) runs its processor only through `cpu.h`.
The cores never know which board they are in.

| File | What it is for |
|---|---|
| `cpu.h` | The interface every core implements: create, step/run, interrupts, cycle and step counts, registers. MIT. |
| `CONTRACT.md` | What a step is, when interrupts are sampled, what HALT, SLP and EI do, and how a core is accepted. Read it before touching a core. |
| `z180_legacy.c` | The Z180 on z180emu (GPL-2.0+), legacy path only: reproduces today's timing so the current goldens hold. It is retired once the MAME core is accepted. |
| `mame_z180/` | MAME's Z180 instruction core, **copied unchanged** (BSD-3-Clause), with `PINNED.txt` (the upstream revision and exactly which files) and the licence text. Never edited in place. |

Coming with the MAME extraction (each its own file, per `CONTRACT.md` 10):

| File | What it will be for |
|---|---|
| `z180_mame.hpp` | Our small class around the vendored instruction files: the registers and member names they expect, and no MAME framework. |
| `z180_mame.cpp` | The machine-level parts re-created from MAME's `z180.cpp` (interrupt check, MMU, internal registers, DMA, the timers), each marked with the upstream function it follows, and **our own step driver** (the contract's phases A-G). It exposes the `cpu.h` functions with C linkage. |
| `z180_asci.c` | Our own byte-level serial port (ASCI): divisors from the chip's registers, `/DCD0` and `/CTS0`, the interrupt level. |
| `test_*.c` | The contract's tests, and a Z80 instruction exerciser under a small CP/M stub. |

Licences: `cpu.h`, `CONTRACT.md` and our own files are MIT. A build containing `z180_legacy.c` is GPL (z180emu). A
build using only the MAME core is MIT plus MAME's BSD-3 notice.
