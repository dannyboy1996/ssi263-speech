# src/csrc/accentsa: the Aicom Accent SA board on MAME's 8085 core

**Opt-in, not yet accepted.** The Accent SA voice of the Accent add-on still runs its 8085 in Python
(`src/hosts/accent_sa.py` on `src/hosts/i8085.py`). This folder is the same machine and the same host in C, so that
every front end can have it without Python (Android next: Tomi, 2026-09-30). The board is `accent_sa.py`'s machine
with nothing changed; the host is its lockstep with the chip, line for line. Aicom's ROMs are not built in: they are
read from a folder or given in memory (`firmware/aicom-accent-sa`, in the repository with `firmware/AICOM.txt`).

| File | What it is for |
|---|---|
| `as_board.h`, `as_board.c` | The machine: u2's lower half at 0000-77FF, 2 KB of RAM at 7800-7FFF, the 32 KB window at 8000-FFFF banked by port 40h (u2's upper half, u3, u4, nothing); the SSI-263 at ports 03-07, reversed, A/R in bit 7 of port 07; port 40h's latch (bank, and bit 4 gating A/R onto TRAP) and switches; the 8085 through `../cpu/cpu.h`; the TRAP gate, RxRDY on RST 6.5, RST 7.5's edge; and `as_board_run`, which counts T-states as the Python host does (below). The chip is the caller's, through two callbacks, so a write lands inside the slice and IN 07h reads A/R then. |
| `as_usart.h`, `as_usart.c` | The 8251 as the firmware uses it: a mode word after reset, commands (RTS, internal reset), status 85h + RxRDY, the host's bytes queued and loaded one at a time while RTS is up. |
| `as_host.h`, `as_host.c` | The host: `accent_sa.py`'s `AccentSA` in C (say, run, skip, busy, boot, cancel, speaking), around the board and an SSI-263 (`../ssi263.h`): the caller's, or one made from the built-in defaults. ROMs from a folder (`ash_create_dir`) or memory (`ash_create`). The API of `accent_sa.dll` / `libaccent_sa.so`. |
| `as_render.c` | The C API alone: the ROMs from a folder and a text into a WAV, no Python (`as_render <folder> "text" out.wav`). |
| `test_as_board.c` | The board's rules on small programs in a synthetic u2 (no firmware): 11 tests. |
| `as_controls.py` | Its must-fail controls: each rule undone in a scratch copy, exactly its tests must fail (17; in run_tests). |
| `compare_accent_sa.py` | The C host against the Python host (the reference) on a scripted session: every write's value and chip time, the audio, and the counting events, identical; the core's own counting reported and classified. `ACCENTSA_COMPARE_FLIP=1`: its must-fail control. |
| `build_board.py` | Windows build (w64devkit): `nvda/dist/accentsa-lib/` gets `test_as_board.exe`, `as_render.exe`, `x64/` and `x86/accent_sa.dll` (importing `ssi263.dll`, as `bl.dll` does). `../../../build_linux.sh` builds `test_as_board`, `as_render` and `libaccent_sa.so`. |

The CPU is `../cpu/i8085_mame.cpp` (CONTRACT.md's 8085 clauses). Python reaches the host through
`src/hosts/accent_sa_c.py`; `SSI263_ACCENT_SA_CORE=c` makes `AccentSA()` return it. The add-on carries that module,
not the DLL: it is found through `SSI263_ACCENT_SA_DLL` or the research tree's `nvda/dist/accentsa-lib/`.

## Two numbers that are not the unit's

- **`cpu_hz` = 3,072,000 is a guess** (a 6.144 MHz crystal, halved by the 8085). The unit's clock has not been measured.
- **`turbo` = 8 is a host feature**: while the firmware reads a sentence, before its first phoneme, the 8085 runs eight
  times faster, so speech starts sooner. Speech itself is paced by A/R and does not change.

Both are `accent_sa.py`'s, kept so that the C host is the same host; neither is a claim about the hardware.

## Counting: the host's, not the core's (2026-09-30)

The two 8085s count the same: `../cpu/trace_i8085.py` found every instruction's and every acceptance's T-states equal
on this firmware. The hosts differ in how a slice ends, and `as_board_run` counts as the Python host does
(`python_slices`, the default):

- **An acceptance at the end of a slice.** `i8085.py`'s `run()` tests its budget between an interrupt's acceptance and
  the next instruction, so an acceptance that reaches the budget ends the slice, and the vector's first instruction (a
  JMP at every vector of u2) runs in the next slice. The core's step is the acceptance and the instruction together.
  The board carries that instruction's T-states into the next slice's budget, so every later slice ends where the
  Python host's does. It happens when a command arrives on the serial line (the scripted session: twice; random
  sessions with ESC R/P between texts: 115 times in 40).
- **A TRAP raised in EI's shadow.** The Python core samples nothing in the instruction after EI, TRAP included; the chip
  (and the core, CONTRACT.md 5) takes TRAP at once. The board holds such a TRAP through that instruction. Never seen
  on this firmware (0 in every session run), kept for exactness, with its test and control.
- A HALT: the Python core ends its slice there, the core runs 4-T HALT slots. Nothing carries between slices, so the
  two agree; the firmware never halts.

With `SSI263_ACCENT_SA_SLICES=chip` the core's own semantics stand (experimental). On every session compared, even
with 115 carried instructions, no write moved: the firmware waits on A/R or the serial port before any write, which
absorbs the 10 T-states. So this counting is exact but, on this firmware, not audible.

## The C host against the Python host (first comparison, 2026-09-30)

`compare_accent_sa.py` (its three parts, the same SSI263C model on both sides): 5,206 writes, every value and every
chip time identical, the audio identical (791 blocks); two acceptances ended the Python host's slice, and the board
carried two. `--quick` (run_tests): 1,761 writes, one carried, the same; on 32-bit Python 3.7 with the x86 DLL too.
Two longer random sessions with commands between texts (a scratch search, seeds 27 and 36): 19,812 and 21,727 writes,
4 and 5 carried, identical. The add-on's Accent SA voice on the C host (driver_sim, 64- and 32-bit): all 33 scenarios
pass, and 32 of them give the same PCM as on the Python host byte for byte (the 33rd is the cancel timed by the wall
clock). The C host runs the full session in about half a second where the Python host takes 8 to 16.

## Open questions

- Android (the next step): `build_android.sh` compiles no C++ yet, and the 8085 core is C++17 (no exceptions, no RTTI).
  It needs the NDK's clang++ and a choice of C++ runtime (libc++ static, so the one `.so` stays self-contained).
- Linux: `libaccent_sa.so` takes the chip's functions from the `libssi263speech.so` that `ssi263/native.py` loaded
  (made global by `accent_sa_c.py` first). Built by `build_linux.sh`, not yet run on Linux.
- Accepting it: the add-on keeps the Python 8085 until Tomi and Astra accept this one.
