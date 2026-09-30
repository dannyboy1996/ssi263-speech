# src/csrc/speakout: the GW Micro Speak-Out board on MAME's V40 core

**Opt-in, not yet accepted.** The Speak-Out add-on still runs its V40 in Unicorn (`src/hosts/speakout.py`). This
board replaces the CPU only (Astra and Tomi's scope for this step): the memory, the chip's window and the V40's
interrupt controller and serial unit are reduced exactly as that host reduces them, so any difference from Unicorn is
the CPU's. The firmware (`SPEAKOUT.HEX`) is not in the repository.

| File | What it is for |
|---|---|
| `so_board.h`, `so_board.c` | The board: 1 MB of RAM, the firmware's HEX, the SSI-263 at F000:FE00-FE04 (its writes returned in order, the RAM keeping them), ports routed to the ICU and SCU, power-on through the reset vector, the host's slice-start interrupt offer, running by steps or by clocks, and `so_run_steps_unicorn` (steps counted as Unicorn counts instructions, for the comparison). |
| `so_icu.h`, `so_icu.c` | The V40's interrupt controller as the firmware uses it: mask, in-service set, non-specific EOI at ports 8-9; vectors 8 + IR; an offer taken or dropped at once, as `speakout.py`'s `_try_irq`. |
| `so_scu.h`, `so_scu.c` | The V40's serial receiver: queued whole bytes, one loaded at a time; status bit 1 at port 1, the byte at port 0 (reading it loads the next). |
| `so_hex.h`, `so_hex.c` | Intel HEX into the 1 MB image, as `speakout.py`'s `load_intel_hex`. |
| `test_so_board.c` | The board's rules on small programs (no firmware): 11 tests. |
| `so_controls.py` | Its must-fail controls: each rule undone in a scratch copy, exactly its tests must fail (11). |
| `build_board.py` | Windows build (w64devkit): `nvda/dist/speakout-lib/` gets `test_v40_contract.exe`, `test_so_board.exe`, `x64/` and `x86/speakout_v40.dll`. `build_linux.sh` builds the same tests and `libspeakout_v40.so`. |

The CPU is `../cpu/v40_mame.cpp` (CONTRACT.md 12). Python reaches the board through `src/hosts/speakout_v40.py`;
`SSI263_SPEAKOUT_CORE=mame` (clocks at `SSI263_SPEAKOUT_V40_HZ`, default 8 MHz) or `mame-steps` (steps coupled as
Unicorn's instructions) makes `SpeakOut()` return the MAME-core host. `nvda/tools/speakout_core_compare.py` compares
it with Unicorn.

## What the firmware touches (measured under Unicorn, boot and two sentences)

Ports: in 0 and 1 (the SCU's data and status), out 1, 2, 3 once each (the SCU's command 35h, mode 4Eh, mask 02h),
out 7 = 76h and out 5 twice (the TCU's counter 1, mode 3, loaded 2000h: set up once, never read), out 8 = 12h then
20h (EOI) at every interrupt, out 9 = 08h (ICW2) then 41h (OCW1), and the V40's system registers FFF0h-FFFEh once
each (OPSEL 0Eh: ICU, TCU and SCU on, DMA off; IULA 08h, TULA 04h, SULA 00h). No other I/O, no word I/O, no HLT, no
software interrupt, no 0Fh (NEC) opcode; REPNE SCASB is its busiest string instruction.

## What a later peripheral step would change

Modelling the V40's peripherals as the chip has them, a separate step once Tomi has listened: bytes arriving at the
SCU's baud rate (from the TCU's counter 1, clocked by the V40's clock or an external one) instead of whole bytes at
slice starts; the ICU's request register, so an interrupt is held until IE allows it instead of being offered once per
slice and dropped; the ICU's full 8259-style priorities and ICW2's vector base. The firmware uses none of the rest
(DMA, the TCU's other counters, the refresh and wait-state registers).
