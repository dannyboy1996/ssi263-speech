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
| A. Acceptance | At most one pending interrupt (priorities in 4): the return address is pushed, `irq_ack` is read if the mode needs a vector, the PC is set. A halted core leaves HALT. | the step's start (not yet charged) |
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
claim otherwise. Synthetic tests pin each exception (Astra's in `investigation/section95-review/`), because the
Braille Lite goldens may never exercise them.

## 4. Interrupts

Lines are set with `*_set_irq(line, asserted)` and sampled at A.

**Z180 (chip, except where marked):**
- TRAP, raised by an undefined opcode, comes first. Then NMI, on an edge.
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
- **INTR**: a level, subject to IE only (not the SIM masks). It is an injected instruction: `irq_ack(ctx, line, n)`
  supplies byte *n* of it (opcode, then any operand bytes). It runs as the step's instruction, at E, with its own
  T-states charged at F.
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

**SLP** (Z180) on the corrected path is HALT with the on-chip peripherals clocked by every slot **(chip)**. On the
legacy path it keeps today's slice-ending quirk (3).

## 7. What callbacks see, by phase

- **A**: stack writes, and `irq_ack`. `*_cycles()` is the step's start. `*_pc()` is the interrupted instruction's
  address. There has been no boundary for the vector's first instruction yet.
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

- **The z180emu adapter (legacy path)**: the Braille Lite goldens, English and Spanish, bit for bit, through
  `bl_board.c` rewritten onto `cpu.h`; plus Astra's synthetic tests for the legacy exceptions (3).
- **MAME's Z180 extracted (corrected path)**:
  - the Z80 instruction exercisers, and Astra's synthetic phase tests (NMI at every boundary, SLP clocking, burst
    DMA chunks, interrupt arrival at slot edges);
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
- MAME's Z180 and 8085 files keep their BSD-3-Clause notices. Builds without z180emu or Unicorn can then be MIT/BSD
  throughout, which is what the App Store requires (iOS imports the firmware from Files and ships none).
