# src/csrc/pc86: a bare PC for a host that stands in for DOS

MAME's 8086 (`../cpu/i86_mame.cpp`, through `cpu.h`) with 1 MB of flat memory. The host does what DOS, the BIOS,
the EMS manager and the PIC did; the CPU hands it every I/O access and every software interrupt. It exists so the
Accent-mini host (`src/hosts/accent.py`) can run Aicom's SPKEMS.DVC on a BSD-licensed CPU in place of Unicorn
(GPLv2). MAME is the 0.7 release default; no environment switch is needed. Unicorn
is retained only as a development comparison and is not packaged.

| File | What it is for |
|---|---|
| `pc86.h`, `pc86.c` | The library: memory the host reads and writes directly; a run of whole `cpu.h` steps that stops before a given address, after a count, or when a callback asks (the three ways `uc_emu_start` stops); IN/OUT and the INT seam (`cpu_bus.intercept`) as callbacks. MIT. |

Built by `../blazie/build_board.py` as `nvda/dist/blazie-lib/<x64|x86>/pc86.dll` and by `build_linux.sh` as
`libpc86.so`; `../accentmini/am_host.c` (the same host in C, for the portable voice) links it directly. `src/hosts/pc86.py` wraps it in the calls `accent.py` makes of Unicorn; `../cpu/compare_i86_accent.py`
checks the two CPUs against each other (`../cpu/README.md`).
