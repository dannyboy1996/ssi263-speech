# src/csrc/cpu: the CPU cores behind the emulated boards

A board (the Braille Lite's `../blazie/bl_board.c`, later the Accent SA's) runs its processor only through `cpu.h`.
The cores never know which board they are in.

| File | What it is for |
|---|---|
| `cpu.h` | The interface every core implements: create, step/run, interrupts, cycle and step counts, registers. MIT. |
| `CONTRACT.md` | What a step is, when interrupts are sampled, what HALT, SLP and EI do, and how a core is accepted. Read it before touching a core. |
| `z180_legacy.c` | The Z180 on z180emu (GPL-2.0+), legacy path only: reproduces today's timing so the current goldens hold. It is retired once the MAME core is accepted. |
| `mame_z180/` | MAME's Z180 instruction core, **copied unchanged** (BSD-3-Clause), with `PINNED.txt` (the upstream revision and exactly which files) and the licence text. Never edited in place. |
| `extract_z180_machine.py` | Generates `z180_mame_machine.cpp` from MAME's `z180.cpp`: exact upstream line ranges, each anchored, with every change named and marked `CHANGED`. Re-run it against a MAME checkout to update. |
| `z180_mame_machine.cpp` | **Generated; do not edit.** MAME's machine-level Z180 code: the internal registers (Z8S180's included), the MMU, DMA, the timers, the interrupt check, reset. |
| `z180_mame.hpp` | Our class around MAME's code: the member names it expects (from MAME's `z180.h`), the flag tables as members, and small stand-ins for the MAME framework (the bus, the daisy chain, logging). The class is the Z8S180. |
| `z180_mame.cpp` | **Our step driver** (the contract's phases A-G, following MAME's `execute_run`) and the `cpu.h` functions with C linkage. It includes `z180_mame_machine.cpp`: build this file, never the generated one alone. The header comment lists every deliberate difference from MAME. |
| `z180_asci.hpp`, `z180_asci.cpp` | Our own byte-level serial ports (ASCI 0 and 1) with MAME's register method names, and a CSI/O stub. Divisors from the chip's registers, `/DCD0`, the interrupt as a level. |
| `test_z180_zex.c` | Runs a CP/M instruction exerciser (zexdoc/zexall, not in the repo) on either core. |

Licences: `cpu.h`, `CONTRACT.md` and our own files are MIT. A build containing `z180_legacy.c` is GPL (z180emu). A
build using only the MAME core is MIT plus MAME's BSD-3 notice.

## The MAME core against the legacy one (first comparison, 2026-09-29)

- The instruction exercisers: zexdoc's and zexall's first 50 groups give the same result on both cores, CRCs
  included. Then the legacy core TRAPs on an undefined opcode (z180emu added the Z180's TRAP); MAME's core has none,
  runs the plain H/L form of a DD/FD-prefixed H/L instruction, and finishes (zexdoc: every group OK).
- The Braille Lite (`nvda/tools/bns_equiv.py`, English): the same 3,709 SSI-263 writes, every value identical. The
  times differ: phonemes inside an utterance within 11 microseconds, utterance starts by up to about 2 ms (the
  serial port: back-to-back bytes took 12 bit times on the legacy core, 10 here, as on the chip), and one wait at
  start-up by 34-47 ms (under investigation). The unit's XON/XOFF stream differs with those times.
- Known limits of the MAME core: no TRAP; MAME's wait states (DCNTL, charged on every access; the legacy core
  charged them only in DMA).
