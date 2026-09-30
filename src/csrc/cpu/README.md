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
| `z180_trap.hpp` | Which prefixed opcodes the Z180 defines, from the manual's op code maps; every other one TRAPs. Ours; it decides, not MAME's tables. |
| `z180_asci.hpp`, `z180_asci.cpp` | Our own byte-level serial ports (ASCI 0 and 1) with MAME's register method names, and a CSI/O stub. Divisors from the chip's registers, `/DCD0`, the interrupt as a level. |
| `test_z180_zex.c` | Runs a CP/M instruction exerciser (zexdoc/zexall, not in the repo) on either core. |
| `mame_i8085/` | Where MAME's 8085 comes from: `PINNED.txt` (the upstream revision, the files' hashes) and the licence text (BSD-3-Clause; Juergen Buchmueller, Roberto Fresca, Grull Osgo). Nothing of it is copied unchanged. |
| `extract_i8085_machine.py` | Generates `i8085_mame_machine.cpp` from MAME's `i8085.cpp` (pinned by revision and sha256): exact line ranges, each anchored, every change named and marked `CHANGED`. |
| `i8085_mame_machine.cpp` | **Generated; do not edit.** MAME's 8085: the tables, reset, the input lines, the interrupt check (acceptance 12 T, Intel's; INTR only accepted, its instruction injected at E), RIM/SIM, every instruction. |
| `i8085_mame.hpp` | Our class around it: the member names MAME's code expects (from `i8085.h`) and small stand-ins for the framework (the bus, the INTR acknowledge, SID/SOD). |
| `i8085_mame.cpp` | **Our step driver** (the contract's phases for the 8085) and the `cpu.h` functions. It includes `i8085_mame_machine.cpp`. The header comment lists every deliberate difference from MAME. |
| `test_i8085_contract.c` | CONTRACT.md's clauses on the 8085 core, one test each (TRAP, the RSTs, INTR's injected instructions, EI, HALT, RIM/SIM, SID/SOD, reset, two instances), T-states from Intel's manual. |
| `i8085_controls.py` | Its must-fail controls: each rule undone in a scratch copy, exactly its tests must fail (the runner guard is `contract_controls.py`'s). |
| `test_i8085_cpm.c` | Runs a CP/M 8080/8085 test program (TST8080, 8080PRE, 8080EXM, CPUTEST; GPL, not in the repo) on the 8085 core. |
| `mame_nec/` | Where MAME's NEC core (the V40, the Speak-Out's CPU) comes from: `PINNED.txt` (the upstream revision, each file's hash) and the licence text (BSD-3-Clause; Bryan McPhail). Nothing of it is copied unchanged. |
| `extract_v40_machine.py` | Generates `v40_mame_machine.cpp` from eight files of MAME's `src/devices/cpu/nec/` (pinned by revision and sha256): exact line ranges, each anchored, every change named and marked `CHANGED`. |
| `v40_mame_machine.cpp` | **Generated; do not edit.** MAME's V20/V40 instruction set: the class's members, fetch and the prefetch queue, the instruction table, reset, interrupts, every instruction. No 8080 mode, no V33 map, no on-chip peripherals. |
| `v40_mame.hpp` | The shim MAME's NEC code compiles against: the bus (8-bit, 20-bit addresses), I/O, the interrupt acknowledge, `logerror`. |
| `v40_mame.cpp` | **Our step driver** (the contract's phases for the V40: one REP iteration per step, HALT, the undefined-opcode count) and the `cpu.h` functions. It includes `v40_mame_machine.cpp`. The header comment lists every deliberate difference from MAME. |
| `test_v40_contract.c` | CONTRACT.md 12's clauses on the V40 core, one test each (22). |
| `v40_controls.py` | Its must-fail controls: each rule undone in a scratch copy, exactly its tests must fail (19). |
| `trace_i8085.py` | The 8085 core against the Python one (`src/hosts/i8085.py`) on the Accent SA firmware's boot: per-step registers at the boundary, the first differences by class. |
| `mame_i86/` | Where MAME's 8086 comes from: `PINNED.txt` (the upstream revision, the files' hashes) and the licence text (BSD-3-Clause; Carl). Nothing of it is copied unchanged. |
| `extract_i86_machine.py` | Generates `i86_mame_machine.cpp` from MAME's `i86.cpp` and `i86inline.h` (pinned by revision and sha256): exact line ranges, each anchored, every change named and marked `CHANGED`. |
| `i86_mame_machine.cpp` | **Generated; do not edit.** MAME's 8086: the inline helpers, the cycle tables, reset, an interrupt's entry (with the INT seam), the input lines, execute_run's own opcodes and every other instruction. |
| `i86_mame.hpp` | Our class around it: one class with the member names MAME's code expects (from `i86.h`) and small stand-ins for the framework (the bus, the INTA vector, no coprocessor, no wait states). |
| `i86_mame.cpp` | **Our step driver** (the contract's phases, following execute_run) and the `cpu.h` functions (`i86_*`, with `i86_regs_set`, `i86_next_pc` and `i86_aliased` for a host that stands in for DOS). It includes `i86_mame_machine.cpp`. The header comment lists every deliberate difference from MAME. |
| `test_i86_contract.c` | CONTRACT.md's clauses on the 8086 core, one test each (24): reset, INTR and its vector, the shadows, NMI, INT n/IRET, the INT seam, HLT, WAIT, prefixes, REP, the trap flag, the alias counter, T-states, I/O, FLAGS. |
| `i86_controls.py` | Its must-fail controls (18): each rule undone in a scratch copy, exactly its tests must fail. |
| `compare_i86_accent.py` | The Accent-mini (`src/hosts/accent.py`) on the MAME 8086 against Unicorn: scripted scenarios, every chip write compared. |
| `census_i86_accent.py` | Which x86 SPKEMS.DVC needs: every block it runs under Unicorn, disassembled (needs capstone). |

Built by `../blazie/build_board.py` (the Braille Lite board on the MAME core: `bl_live_mame.exe`,
`test_bl_board_mame.exe`, beside the legacy ones) and `../../../build_linux.sh` (`test_bl_board_mame`). Gated in
`nvda/tools/run_tests.py`: the spoken values of both goldens (`bns_equiv.py --values-only`; times are not
compared, they legitimately differ), a must-fail control with one value flipped, and two units in one process.
The shipped libraries still use the legacy core.

MAME's 8085 (the Accent SA's CPU, to replace `src/hosts/i8085.py`) is built the same way: `test_i8085_contract.exe`
by `build_board.py`, `test_i8085_contract` by `build_linux.sh`, both gated; no board runs it yet.

MAME's 8086 (the Accent-mini's PC, to replace Unicorn under `src/hosts/accent.py`) likewise: `test_i86_contract.exe`
and `x64/`, `x86/pc86.dll` (`../pc86`) by `build_board.py`, `test_i86_contract` and `libpc86.so` by `build_linux.sh`.
Gated: the contract tests, `compare_i86_accent.py --quick` with its control, and driver_sim on the built add-on with
`SSI263_ACCENT_CORE=mame`. Opt-in only: Unicorn stays the default.

Licences: `cpu.h`, `CONTRACT.md` and our own files are MIT. A build containing `z180_legacy.c` is GPL (z180emu). A
build using only the MAME core is MIT plus MAME's BSD-3 notice.

## The MAME core against the legacy one (first comparison, 2026-09-29)

- The instruction exercisers: zexdoc's and zexall's first 50 groups give the same result on both cores, CRCs
  included. Then the legacy core TRAPs on an undefined opcode (z180emu added the Z180's TRAP; zexall's
  `ld <bcdexya>` runs DD/FD before non-HL instructions); MAME's core has none, runs the unprefixed instruction, and
  finishes (zexdoc: every group OK).
- The Braille Lite (`nvda/tools/bns_equiv.py`): the same SSI-263 writes, every value identical (English 3,709,
  Spanish 3,597). The times differ:
  - phonemes inside an utterance: within 11 microseconds;
  - utterance starts: up to 2.7 ms. That's the serial port: back-to-back bytes took 12 bit times on the legacy
    core, 10 here, as on the chip;
  - one start-up wait: 34 ms (English) and 60 ms (Spanish). The CPU sleeps until the 100 ms PRT0 tick, and on the
    legacy core the timers fall behind board time at each SLP. That's its slice-ending quirk (CONTRACT.md 3),
    measured at 335,299 cycles at that wake.
  - The unit's XON/XOFF stream differs with those times.
- Two units in one process (`../blazie/test_bl_board.c` built with `BL_Z180_MAME`): identical alone and interleaved.
- TRAP (after Astra, Reply 92): the MAME core now TRAPs every opcode the Z180's op code maps leave undefined
  (`z180_trap.hpp`, from the manual's Tables 48-50), with ITC.TRAP/UFO and the stacked PC as the manual says.
- Known limits of the MAME core: MAME's wait states (DCNTL, charged on every access; the legacy core
  charged them only in DMA).

## The MAME 8085 against the Python core (first comparison, 2026-09-29)

- The CP/M test programs (`test_i8085_cpm.c`; not in the repo): TST8080 ("CPU IS OPERATIONAL") and 8080PRE pass.
  8080EXM: 2 of 25 groups pass; CPUTEST stops at its test 000Bh (INR B: F = 00h, "should contain 02h"). Both
  check PSW bits 1, 3 and 5, which Intel leaves undefined on the 8085 (the PUSH PSW listing's "X") and an 8080
  holds at 1, 0, 0; MAME keeps its undocumented V and K flags there. A scratch run with PUSH PSW forced to the
  8080's bits (not a change to the core): 8080EXM 23 of 25, the two left being `aluop` (the 8085's ANA sets AC,
  Intel's ANA listing), and CPUTEST reports no error before it waits for console input.
- The Accent SA firmware (`trace_i8085.py`, both cores in lockstep on a stub board, 400,000 instructions of boot
  and "Hello."): the same 312 SSI-263 writes at the same instructions, the same interrupts (TRAP 55, RST 6.5 14),
  the same T-states for every instruction and acceptance (12), and the same registers except PSW bits 1/3/5 from
  the reset value on. `--control` (an 11-T acceptance) must be reported, and is. With `--trap-after-ei 1` (a
  TRAP raised right after an EI) the cores part: the MAME core takes TRAP at once (CONTRACT.md 5), the Python
  core runs the next instruction first.

## The MAME V40 against Unicorn (first comparison, 2026-09-30)

The Speak-Out on the board in `../speakout/`, against today's Unicorn host, same chip model, same scenario (the
greeting, six sentences spoken to their end, one cut by ^X): `nvda/tools/speakout_core_compare.py`.

- Steps coupled to chip time as Unicorn's instructions (`mame-steps`): all 4,268 SSI-263 writes identical, values
  and times. One counting difference had to be matched first: Unicorn counts a REP string instruction that ends by
  its count as n + 1 instructions (the exit test is one more), this core as n steps (`so_run_steps_unicorn`, tested
  against Unicorn's own counts).
- Clocks at 8 MHz (`mame`): every speech frame of every utterance identical (the cut one up to the cut). The firmware
  writes more idle frames (PA at rate F) while its rules work, and the times move: an utterance's first phoneme lands
  about twice as late after the text is sent (e.g. 74 against 34.5 ms, 160 against 69.5 ms), and phonemes inside
  long utterances lag their first by up to 65 ms more than on Unicorn; the greeting gains 6 idle frames inside it.
- Why: MAME charges this firmware 12.7 clocks per instruction on average (the V20's clock counts and its prefetch),
  so Unicorn's 1.5 million instructions a second are a 19.1 MHz V40. At 19.1 MHz the times fall within a few ms of
  Unicorn's (first phonemes within 5.5 ms, inside utterances within 16 ms, the same idle frames). The unit's crystal
  is not yet read; 8 MHz is the uPD70208-8's rating (MAME's nec.cpp notes the V40 at 10 MHz, the V40HL up to 20).
- No undefined opcode was executed.

## The MAME 8086 against Unicorn (first comparison, 2026-09-30)

- **Which CPU.** An instruction census of SPKEMS.DVC under Unicorn (a block hook, every distinct block disassembled;
  INIT, boot, eight texts with numbers and punctuation, rate/pitch/volume/voice commands, a cancel mid-sentence:
  3,761 distinct blocks, 5,471 chip writes, some 180 million instructions) found 48 mnemonics, all 8086: no 0Fh,
  60h-6Fh, C0h/C1h, C8h/C9h, no operand-size, address-size, FS or GS prefix; the only prefixes CS:, ES: and REP.
  PUSHF/POPF only save and restore IF around critical sections (no CPU-type test). So MAME's `i8086` (the 8088 is
  the same code on a narrower bus): the lowest core, and the PC the card was sold for. The core counts every opcode
  whose meaning differs on the 80186 and later (`i86_aliased`) and `pc86.py` stops on one: none in 625 million steps
  of `compare_i86_accent.py`.
- **The writes** (`compare_i86_accent.py`, full run): INIT 17, the demo dialogue 13,317, settings 2,823, seeded
  cancels 1,447, the add-on's snapshot path 687 -- every value AND every time identical, the audio identical, the
  registers and the host's log identical; INIT's snapshots (registers, chip writes, card and EMS state) identical;
  64- and 32-bit Python. Memory after INIT differs in 4 bytes: FLAGS images on the stack, bits 12-15 set on the
  8086 (F2h) and clear on Unicorn (02h, a 386 in real mode) -- never read back as data.
- **Timing, classified.** Both CPUs are driven the host's way, `cpu_ips` (5 million) instructions per second of chip
  time in slices, so a write's time can move only where the two count instructions differently and a slice ends in
  between. The known cases: (1) a REP string instruction with CX = n: Unicorn counts n + 1 (a last pass that finds
  CX = 0), MAME n. The driver's only REP is the REPE CMPSB of its EMS check, inside INIT's single unsliced call: no
  effect. (2) LOCK is its own step on MAME (unused by the driver). None occurred.
- **The coupling is a compatibility policy, not a clock** (Astra, Reply 104). The host is a virtual PC running the
  DOS driver, not a model of one particular physical PC; 5 million instructions a second is the setting Unicorn's
  host has always used, kept so that changing the CPU does not also change the scheduler. The core counts T-states
  beside it (`i86_cycles`, Intel's counts: CONTRACT.md 4) and steps (`i86_steps`), separately; the host reads only
  the steps, so the clock corrections move no write. The workload's 14-15 T-states an instruction do not make the
  policy a real 74 MHz 8086, and a 4.77 MHz PC's rate is not the policy either. Clock-based board timing is future
  work, a decision for Tomi and Astra, not made here.
- **complete_fuzz on the MAME core** (`COMPLETE_FUZZ_SYNTH=accent`, SIM_SPEED=10) reports 1-4 incomplete utterances
  per 150 steps where Unicorn reports none. Not the CPU: the driver's calls recorded from such a run and replayed on
  both CPUs give the live run's 29,518 writes exactly, on either; Unicorn slowed by 25% (a sleep after each run/say)
  fails the same way; with the fuzz's 0.5 s wait after "done" made 1.5 s, the MAME core passes every seed. The
  utterances finish, later than the check looks: `pc86.dll` is about 1.2 times slower than Unicorn's JIT on this
  driver, and the fuzz's margin is host-speed dependent. Not gated on the MAME core for that reason.
- **Clock counts from Intel's manual** (Astra, Reply 104; CONTRACT.md 4, each a named change with its control):
  INT n 51, INT 3 52, INTO 53, IRET 24 (MAME 0, 2, 2, 44), the same when the host intercepts one (the omitted
  handler's time is not modelled); INTR 61, NMI 50, the trap 50 (MAME 0); the divide error's entry 51 (a model);
  REP string forms 9 + n a repetition (MAME 2 + the plain instruction a pass); NOP 3, LOCK 2, ESC per the table;
  4 more per word transfer at an odd address. Every step now costs at least 2 T, so `i86_run` always returns:
  Astra's probe (an INT 21h vectored at itself, 0 T a step on MAME's counts) looped `i86_run(cpu, 1)` for ever.
  After the corrections the comparison is unchanged: every write identical in value and time, as the host counts
  steps.
- **Known limits (MAME's, kept):** the stack and word-port rows are Intel's 8086 figure plus the 8088's 4 a word
  transfer, and the returns match neither CPU (CONTRACT.md 4, open). The undefined flags are MAME's.
