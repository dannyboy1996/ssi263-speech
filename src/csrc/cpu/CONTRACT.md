# The CPU contract, version 2 (draft for review)

`cpu.h` declares the interface; this defines its behaviour. Every core in libssi263speech implements it: the Z180
(the Braille Lite), the 8085 (the Accent SA), and later an x86 real-mode core. A board drives a core only through
`cpu.h`.

Version 2 follows Astra's review (Reply 78). The main change: **two execution paths**, kept apart.

- **The corrected path** (`*_step`, `*_run`): the semantics this contract defines, the target for every new core.
- **The legacy compatibility path** (`z180_run_legacy`): today's z180emu run slice, exceptions included, so that the
  current Braille Lite goldens hold while the board moves onto `cpu.h`. It is not a model of the chip. It is a
  fixed reference, retired once the Z180 comes from MAME (Tomi's decision: MAME's BSD-3 Z180 extracted behind this
  interface) and new goldens are accepted.

Sources are labelled throughout: **(chip)** the manufacturer's documented behaviour; **(model)** a modelling choice;
**(legacy)** observed in today's core, kept only for compatibility.

## 1. A step on the corrected path

`*_step()` performs exactly one step, in these phases, and returns its T-states:

| Phase | What happens | `*_cycles()` during it |
|---|---|---|
| A. Acceptance | At most one pending interrupt (priorities in 4), **by type**. *Vectored* (Z180 TRAP, NMI, IM1, IM2; 8085 TRAP and the RSTs): the return address is pushed, any vector byte is read (`irq_ack`, byte 0), the PC is set. *Injected* (8085 INTR, Z180 IM0): nothing is pushed and nothing is read here; the step's instruction at E comes from the acknowledge instead (4). Either way, a halted core leaves HALT. | the step's start (not yet charged) |
| B. Acceptance charge | The acceptance T-states are added, and the on-chip timers are clocked by them. | start + acceptance |
| C. EI shadow ends | (see 5) | start + acceptance |
| D. Boundary | `steps` increments; the on-chip serial port catches up to the current count and refreshes its interrupt level; then `bus->boundary(ctx, pc)`. | start + acceptance |
| E. Instruction | One instruction, or one HALT or SLP slot (6). Memory and I/O callbacks run here. | start + acceptance |
| F. Instruction charge | Its T-states are added; the on-chip timers are clocked by them. | start + acceptance + instruction |
| G. DMA | Z180: a cycle-stolen DMA transfer, charged and clocked. | as F, then + DMA |

Consequences worth stating:
- Anything the board raises in D (a key, the A/R request) is sampled at the **next** step's A **(legacy, kept as
  model)**.
- The serial port's interrupt level is refreshed at D, before the board's work. Moving that to F would make its
  request visible one boundary later **(legacy, kept as model)**.
- Burst-mode DMA (Z180) is its own step: a chunk of at most `Z180_DMA_CHUNK` bytes, with its own boundary at D, its
  T-states charged at F, and no instruction. The chunk size is fixed, so a step never depends on a budget.

## 2. Running a budget (corrected path)

`*_run(budget)` performs whole steps while the T-states run so far are below `budget`, and returns them: `>= budget`,
with an overrun under one step. `*_run(0)` returns 0 and has no side effects. A halted core keeps stepping in slots:
`*_run` never skips to the budget's end.

## 3. The legacy compatibility path (`z180_run_legacy`)

`z180_run_legacy(budget)` reproduces today's `cpu_execute_z180(budget)` exactly, **its exceptions included**:

- **NMI is sampled at slice entry only**, not at each boundary (Astra, Reply 78: ten NOPs in one 30-cycle call
  finish with an NMI raised from the first hook still pending; one-cycle calls take it on the next call).
- **A burst DMA chunk takes the rest of the slice's budget**, with a single boundary callback.
- **SLP ends the slice**: the remaining budget is dropped while the timers are clocked for that one step only (608
  cycles reported against 8 clocked, Reply 76).

A sequence of `*_step()` calls, or of 1-cycle legacy calls, is **not** equivalent to one legacy call. Nothing may
claim otherwise. Each exception needs a synthetic test, because the Braille Lite goldens may never exercise them.
Which tests already exist and which are still to write is listed in 10.

## 4. Interrupts

Lines are set with `*_set_irq(line, asserted)` and sampled at A.

**Z180 (chip, except where marked):**
- TRAP, raised by an undefined opcode, comes first. It is not an acceptance at A: it is found at E, at the
  undefined byte's fetch, and is that step's instruction (its fetches, the read at IX+d for a DDCB/FDCB form, the
  stacking of PCH to SP-1 then PCL to SP-2, and the jump to 0000h), charged at F (printed pages 70-72, Figures
  32-33: 18 T for a 2nd op code, 26 T for a 3rd, plus programmed waits). Then NMI, on an edge.
- R counts op code fetches (M1 cycles, printed page 177), a trapped one and an IM0 acknowledge included.
- The on-chip requests are taken before the priority choice from their sources: PRT0/PRT1 as levels of TIF and
  TIE (a cleared TIF drops its request), the ASCI and CSIO likewise; the DMA completions are latched.
- IM0 timing: an injected instruction's opcode fetch is the 5-T acknowledge cycle (T1 T2 TW* TW* T3, two
  automatic waits: printed page 76, Figure 36), so an injected RST is 13 T without programmed waits; the
  acknowledge bytes take no programmed memory waits. An undefined injected form TRAPs, stacking the PC the
  interrupt found **(model)**.
- **(model)** Stack writes other than TRAP's (CALL, RST, the interrupt pushes) come from MAME's PUSH, which writes
  the low byte first: the stacked bytes are right, their bus order is not.
- Then the maskable sources, when IFF1 is set and no EI shadow is active: INT0, INT1, INT2 (levels, each gated by
  its ITC enable), then the on-chip sources in the Z180's order (PRT0, PRT1, DMA0, DMA1, CSIO, ASCI0, ASCI1).
- The ASCI request is a level that follows its status bits (Astra, Reply 15).
- **NMI acceptance** copies IFF1 into IFF2 and clears IFF1, so RETN restores the enable. **Maskable acceptance**
  clears both. There is no blanket "acceptance clears both" rule.

**8085 (chip, per Intel's documentation, checked against the pinned MAME source):**
- **TRAP** is non-maskable: neither IE nor the SIM masks affect it. It is edge and level sensitive: pending after a
  rising edge *while the line stays high*, so dropping the line before acceptance cancels it (unlike the RST 7.5
  latch). Acceptance saves IE for RIM.
- **RST 7.5**: an edge latch, cleared by acceptance or by SIM's R7.5 bit. It is subject to IE and its SIM mask.
- **RST 6.5, RST 5.5**: levels, subject to IE and their SIM masks.
- **INTR**: a level, subject to IE only (not the SIM masks). It is an **injected instruction**, and all of its
  acknowledge reads happen at E: the byte index *n* restarts at 0 for each acceptance, and `irq_ack(ctx, line, n)`
  supplies the opcode (*n* = 0) and then any operands, in place of memory fetches. It is the step's one instruction,
  executed exactly once, with no ordinary instruction after it in the same step. It does its own control transfer:
  an RST or CALL pushes and jumps as that instruction does, and an injected NOP pushes nothing. Its T-states are
  charged at F. The Z180's IM0 works the same way.
- **The Python core (`src/hosts/i8085.py`) is not an oracle.** It delays TRAP through the instruction after EI, and
  charges 12 T-states for an accepted interrupt where the pinned MAME source charges 11 (Reply 78). Each such
  difference is decided from Intel's documentation and recorded; neither implementation is copied blindly.

## 5. EI

After EI, the next step's acceptance skips the **maskable** interrupts, so the instruction after EI runs first
**(chip)**. TRAP and NMI are not delayed.

## 6. HALT and SLP

A halted core does one **slot** per step, separate from the HLT instruction's own execution cost:
- Z180: 3 T-states **(legacy, kept as model)**.
- 8085: 4 T-states **(model, proposed)**.

Interrupt arrival is tested at both slot edges.

**SLP** (Z180) is **not** simply HALT with the peripherals running. Per Zilog's manual (Z8018x user manual, printed
pages 33–35) **(chip)**:
- **Wake without service.** An interrupt request that is *individually* enabled ends SLEEP even when IEF1 = 0. The
  CPU then continues at the instruction after SLP without vectoring. With IEF1 = 1 it is accepted as usual. HALT
  differs: a request masked by IEF1 = 0 leaves the CPU halted.
- **What stops.** Normal SLEEP stops DMA and refresh; the IOSTOP bit (ICR) changes which on-chip peripherals keep
  running.

On the corrected path, sleep is polled in the same fixed slots as HALT **(model)**, and each slot's T-states clock
the peripherals that the manual leaves running in that state. Required tests: wake-without-service, and IOSTOP
(10). The narrow clock-accounting correction measured so far (Reply 76) is part of this, not all of it. On the
legacy path, SLP keeps today's slice-ending quirk (3).

## 7. What callbacks see, by phase

- **A** (vectored acceptance only): the stack writes and any vector read (`irq_ack`, byte 0). `*_cycles()` is the
  step's start. `*_pc()` is the interrupted instruction's address: where execution resumes, the address acceptance
  pushes. For a halted core that is the address after its HALT or SLP **(model)**; during the slot itself (D, E)
  `*_pc()` stays on the HALT, or on SLP's first byte (MAME's convention; the legacy core reports SLP's second). There has been no boundary for the vector's first
  instruction yet. An injected instruction makes no bus access at A.
- **E** for an injected instruction: its bytes come from `irq_ack` (*n* = 0, 1, …), and any stack writes are the
  instruction's own.
- **D**: `serial_tx` (the serial catch-up), then `boundary`. `*_cycles()` is start + acceptance.
- **E**: memory, fetch and I/O. `*_cycles()` is start + acceptance. The instruction's own T-states come at F.
- **F**: timer-driven serial or interrupt effects happen inside the core and reach the board at the next D.
- **G**: DMA memory accesses. `*_cycles()` is start + acceptance + instruction.

Two PCs are distinct. `*_pc()` is the **saved instruction-start PC** of the current (or next) instruction.
`*_regs_get()` reports the **architectural** registers, including the PC as execution has moved it.

Getters have no side effects. In particular they never perform a RIM, and never consume the 8085's saved
post-TRAP IE.

Memory addresses are physical (after the Z180's MMU). Only external I/O reaches `in`/`out`. Cores are not
bus-cycle accurate: an access inside an instruction is not placed at its own T-state, and the Z180's wait states
(DCNTL) are applied as the chosen core applies them, documented per core.

## 8. The 8085's I/O timestamps

Start-stamped I/O (E: start + acceptance) is the one documented convention for every core **(model)**. Today's
Python core charges before executing, so its timestamps sit later by an amount that **varies per instruction** (its
opcode fetch already happens before the charge), not by a constant. Before migrating:
- capture today's Accent SA ordered writes and timestamps as a reference;
- compare architectural state and events at explicitly normalised phases;
- check the host's speech and interrupt timing separately.

## 9. API obligations

- **Ownership.** `*_create` copies the `cpu_bus`. `ctx` must outlive the core.
  - Required callbacks: `read`, `write`, `in`, `out`.
  - Optional (NULL allowed): `fetch` (defaults to `read`), `irq_ack` (defaults to FFh), `serial_*`, `boundary`.
- **Reset** restores the architectural state and the chip's reset values, clears pending edges and latches,
  re-samples level lines on the next A, and zeroes `cycles` and `steps`.
- **`steps()`** increments at D, before `boundary` runs. So the board's scheduled work sees the new count, as
  today's board increments its counter before its key schedule.
- **Serial divisors** come from the chip's own registers. 16 is only the Braille Lite's configuration.
- **Instances.** No global mutable state. Distinct instances may run on distinct threads, and two instances
  interleaved in one thread must behave as each alone (a required test; `test_bl_board.c` does it today for the
  board).
- **Re-entry.** A callback may call `*_cycles`, `*_steps`, `*_pc`, `*_regs_get` and `*_set_irq`, but not `*_step`,
  `*_run`, `*_reset` or `*_destroy`.
- **`*_destroy`** frees everything the core allocated.

## 10. How a core is accepted

**Tests that already exist** (Astra's, on copies of today's core):
- `investigation/section93-review/`: the SLP clock accounting (608 reported against 8 clocked; 602 and 602 once
  fixed) and the timer start from RLDR.
- `investigation/section95-review/`: NMI sampled at slice entry against per boundary, and the Python 8085's EI/TRAP
  behaviour.

**Written for the MAME Z180 core** (`test_z180_contract.c`, in run_tests and the Linux gate; its must-fail
controls, each undoing one driver rule, are `contract_controls.py`):
- a zero budget; reset (counts zeroed, a pending NMI edge dropped, a held INT0 re-sampled); two instances
  interleaved;
- NMI taken at the step after the boundary that raised it (IFF1 into IFF2); the EI shadow;
- an interrupt raised at a HALT slot's boundary, accepted at the next step (the slot is 3 T);
- SLP wake-without-service with IEF1 = 0, and HALT staying halted in the same program;
- IOSTOP stopping the PRT;
- burst DMA in chunks of 16 bytes, each its own step with a boundary and no instruction;
- (after Astra, Reply 92) IM0 injected instructions: a NOP (nothing pushed, no instruction after it in the step),
  an RST and a CALL (their own pushes of the interrupted PC; the CALL's operands from acknowledge bytes 1 and 2),
  the byte index from 0; DMA stopped by SLEEP, including in the SLP's own step, with HALT's DMA as the positive
  control; an NMI waking a HALT and disabling DMA; the acceptance-phase `*_pc()`, from HALT too; the ASCI's
  error flags cleared by EFR = 0 and not by EFR = 1.

**Tests still to write:**
- refresh stopped in sleep (the core doesn't model refresh), and IOSTOP stopping the ASCI;
- the 8085's INTR injected instructions (with its core);
- the bus order of the other stack writes (see 4), if a board ever depends on it.

**After Astra, Replies 93/94:** `im0_rst_nowait` (13 T, R + 1), `im0_nop_nowait` (5 T), `im0_prefixed` (LD IX,nn
from the acknowledge: the PC held, R + 2), `im0_undefined` (DD 00h from the acknowledge TRAPs, stacking the
interrupted PC); the first three IM0 tests now assert their T-states (5, 19, 24 with reset's wait states);
`trapbus_2nd` (DD 24h: 18 T, R + 2, PCH written first), `trapbus_3rd` (DD CB 05 00: 26 T, R + 3, one read of
IX+5, PCH first), `trapbus_legal_ddcb` (the control: RLC (IX+5) reads IX+5); `prt_priority` (a waiting PRT0
overflow beats a waiting DMA0 completion in all 20 timer-clock phases; Astra's fixture had 3 of 20) and
`prt_stale` (with TIF0 cleared first, DMA0 is taken). `contract_controls.py` checks each program's exit code,
summary line and the full test inventory (a crash, a timeout or a missing test fails the run), and has 27 controls
(29 after Reply 95).

**After Astra, Reply 95:** an injected prefixed instruction keeps the PC it set only if it transfers control (JP
(IX)/(IY), RETN, RETI), decided from the acknowledge bytes, not from the final PC: `im0_jpix_collision`/`_ordinary`,
`im0_retn_collision`/`_ordinary`, `im0_wrap`; an injected LDIR runs one iteration with the PC put back
(`im0_ldir_once`, a model choice).

**The legacy path's exceptions** (`test_z180_legacy.c`, on z180emu): `legacy_nmi_entry` (an NMI raised at a slice's
first boundary stays pending through that 30-cycle call and is taken at the next call's entry; one-cycle calls take
it at the next call), `legacy_burst` (a 100-cycle call from the DMA start: one boundary, 17 bytes, 102 cycles),
`legacy_slp_slice` (one 5000-cycle call: SLP reports the budget with no PRT0 wake; the HALT control wakes four
times).  Writing them found that `z180_set_irq(Z180_NMI)` never reached z180emu's NMI (line 3 is an IRQ slot there);
fixed in the adapter, and the test fails with the mapping undone.

**TRAP** (`z180_trap.hpp`, from the manual's op code maps): undefined second bytes after DD/FD (DD 00h, INC IXH,
EX DE,HL), ED (the Z80's NEG duplicate, IN (C)) and CB (SLL) trap with UFO = 0 and the stacked PC at the
instruction's start + 1; undefined DDCB/FDCB fourth bytes trap with UFO = 1 and start + 2; IEF1 unaffected;
software clears ITC.TRAP and cannot set it; legal prefixed instructions (LD IX, an (IX+d) CB form, NEG, MLT,
SRL, JP (IY)) are the negative controls.

**MAME's e0deaf3898b, one test per hunk** (each fails with its hunk reverted in `contract_controls.py`):
`timer_start` (enabling starts from RLDR), `tmdr1h` (TMDR1H is the high byte), `dma_done_di` (a DMA0 completion
while IFF1 = 0 is kept, and taken only after EI and its shadow: the DMA hunk and the internal-IRQ gating hunk), and
`dma1_level` in `test_z180_whitebox.cpp` (/DREQ1's edge or level sense is DMS1; cpu.h has no DREQ lines).

Acceptance per core:
- **The z180emu adapter (legacy path)**: the Braille Lite goldens, English and Spanish, bit for bit, through
  `bl_board.c` rewritten onto `cpu.h`; plus the legacy-exception tests above (`test_z180_legacy.c`).
- **MAME's Z180 extracted (corrected path)**:
  - the Z80 instruction exercisers, and the phase tests above;
  - then the Braille Lite firmware, compared with the legacy adapter. Every difference is explained before new
    goldens are accepted, including the serial protocol (the ordered host sends, cancels and completions), not
    just the spoken register values;
  - then a listen.
- **MAME's 8085 (corrected path)**: per-step traces against the Python core at normalised phases, each difference
  decided from Intel's documentation (4); the 8080/8085 exercisers; the interrupt, flag and timing tests; the
  captured Accent SA reference (8).

The behavioural specification comes from the manufacturers' manuals. Observations of today's cores are labelled
**(legacy)** and are kept as regression facts, not as the definition of correct.

## 11. Licences

- `cpu.h` and this contract: MIT.
- The z180emu adapter links a GPL-2.0-or-later core, so any build containing it is GPL.
- MAME's Z180 and 8085 files keep their BSD-3-Clause notices, each extracted dependency with its own actual notice
  and the upstream revision pinned.
- **Our dependency policy** for store builds (iOS): no GPL components, so neither z180emu nor Unicorn; MIT and BSD
  components with their notices kept; no firmware shipped (it is imported from Files). This is our policy. Apple's
  review guidelines don't require any particular licence, and choosing these licences doesn't by itself make an app
  acceptable.
