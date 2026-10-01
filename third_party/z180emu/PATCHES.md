# z180emu, as vendored here

The Z180 core of [z180emu](https://github.com/mtdev79/z180emu) (GPL-2.0-or-later, `COPYING`; the core is Juergen
Buchmueller's MAME-era Z180, see the file headers) at upstream commit 32592c7, with this project's five changes.
`local.patch` is exactly them (`git diff` against upstream); `UPSTREAM.json` holds the upstream commit and the
sha256 of every file as vendored. Only `z180/` is here: the Braille Lite board is this project's own
(`src/csrc/blazie`), reaching the core through `src/csrc/cpu/z180_legacy.c`.

| File | Change | Why |
| --- | --- | --- |
| `z180/z180ops.h` | `SLP` backs the PC up one byte (`_PC--`), as `ENTER_HALT` does | `LEAVE_HALT` advances the PC when an interrupt wakes the CPU; without this the instruction after SLP was skipped (the Braille Lite's RAM hook D655 = SLP; RET then ran into the speech buffer at D658) |
| `z180/z180.c`, `.h` | `cpu_icount_z180()` | the cycles still to run in the current slice, for exact cycle stamps (cpu.h's `z180_cycles`) |
| `z180/z180.c`, `.h` | `z180_asci_irq_pending()` | the ASCI interrupt flags written directly, as a level (the board re-asserts it after every instruction) |
| `z180/z180.c`, `.h` | `cpu_inject_call_z180()` | a test-harness hook (a CALL injected at the current PC); not used by the library |
| `z180/z180.c` | reading or writing `TRDR` clears `CNTR`'s `EF` | the Z180's CSI/O as its manual has it: the Blazie units' clock controller clocks bytes through it (`src/csrc/cpu/z180_legacy.c` completes the transfers, `src/csrc/blazie/bl_clock.c`); upstream never set `EF`, so nothing that ran before changes |

The build scripts compile this copy (`build_linux.sh`, `build_android.sh`, `src/csrc/blazie/build_board.py`,
`src/apps/blazie/build_app.py`); `paths.local`'s `Z180EMU` is still used for the harness `bns.c`/`bns_live.exe`
(the NVDA add-on's pipe fallback), which is not vendored.
