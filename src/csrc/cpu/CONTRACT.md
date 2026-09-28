# The CPU contract (draft for review)

`cpu.h` declares the interface; this defines its behaviour. Every core in libssi263speech implements it the same
way: the Z180 (the Braille Lite), the 8085 (the Accent SA), and later an x86 real-mode core. A board drives a core
only through `cpu.h`. Where a choice is a model decision and not the chip's documented behaviour, it says so.

The first implementation will be an adapter around today's z180emu core, and it must reproduce the golden vectors
(`nvda/tools/golden`) bit for bit. That requirement fixes several choices below; they are marked **(golden)**.

## 1. A step

A step is the unit of execution. `*_step()` performs exactly one, in this order, and returns its T-states:

1. **DMA** (Z180 only): if DMA channel 0 runs in burst mode, the step is one burst chunk and nothing else.
2. **Interrupt acceptance**: at most one pending interrupt is accepted (priorities in 3). Its acknowledge and
   vector cycles are charged to this step, and a halted core leaves HALT.
3. **The EI shadow ends** (see 4).
4. **`bus->boundary(ctx, pc)`**, with `pc` the address of the instruction about to run **(golden)**. The board's
   per-instruction work happens here, *after* acceptance, so an interrupt line it raises is sampled at the *next*
   step's acceptance. (Before acceptance, a scheduled key would be taken one instruction earlier and the goldens
   would change.)
5. **One instruction**, or, when halted or asleep, **one HALT slot** (5).
6. **Charging**: the instruction's (or slot's) T-states are added to the core's cycle count, and the on-chip
   peripherals (timers, serial, DMA) are clocked by exactly the T-states charged in this step, acceptance
   included.
7. **Cycle stealing** (Z180): a non-burst DMA transfer after the instruction, charged to this step.

`*_steps()` counts steps, HALT slots included. The Braille Lite's boot schedule (keys at instruction counts) is
defined in steps **(golden)**.

## 2. Running a budget

`*_run(budget)` performs whole steps while the T-states run so far are below `budget`, and returns the T-states
actually run. So the result is `>= budget`, and the overrun is less than one step. A step is never split. A halted
core keeps stepping in HALT slots: it never skips to the end of the budget, because the boundary callback must fire
at every slot **(golden)**.

## 3. Interrupts

Lines are set with `*_set_irq(line, asserted)` and sampled only at step 2. Level-sensitive lines are accepted while
asserted and enabled. Edge-sensitive ones latch on a rising edge and are cleared by acceptance.

- **Z180**: TRAP (an undefined opcode) first, then NMI (edge), then the maskable sources, when IFF1 is set and the
  EI shadow has ended: INT0, INT1, INT2 (levels, each gated by its ITC enable), then the on-chip sources in the
  Z180's fixed order (PRT0, PRT1, DMA0, DMA1, CSIO, ASCI0, ASCI1). The ASCI request is a *level* that follows its
  status bits: it stays pending while the condition holds (Astra, Reply 15). Today the board keeps that level
  itself; it moves inside the core.
- **8085**: TRAP (edge and level), then RST 7.5 (edge latch), RST 6.5, RST 5.5 (levels), then INTR, each subject
  to IE and to the SIM masks, as the 8085's data sheet orders them. RIM reports the pending bits, the masks, IE,
  and after a TRAP the IE it interrupted. INTR's vector comes from `bus->irq_ack`.

## 4. EI

After EI, the next step's acceptance skips the *maskable* interrupts, so the instruction after EI always runs first
(the standard shadow). TRAP and NMI are not delayed. An accepted interrupt clears IE (IFF1 and IFF2 on the Z180,
per the interrupt mode).

## 5. HALT and SLP

A halted core does one **HALT slot** per step. **Z180: 3 T-states (golden; a model choice, not a documented bus
figure). 8085: 4 T-states (a model choice, proposed).** An accepted interrupt ends HALT at step 2.

**SLP** (Z180) is HALT with the on-chip peripherals still clocked by every slot's T-states. **This differs from
today's z180emu**, whose SLP ends the whole run slice while its peripherals are clocked for only that one step:
Astra measured 608 T-states reported against 8 clocked (Reply 76). The first adapter keeps today's behaviour so that
the goldens still hold. The corrected behaviour becomes the contract's once the isolated test (Reply 76) is
explained, with a new golden baseline.

## 6. What a callback sees

During a step's memory, I/O and serial callbacks:

- `*_cycles()` is the count at the step's start **plus** the acceptance T-states of this step. The instruction's own
  T-states are charged only when it completes (1.6) **(golden)**: the Braille Lite timestamps every SSI-263 write
  from this.
- `*_pc()` is the address of the current instruction (the value `boundary` was given).
- Memory addresses are **physical** (after the Z180's MMU). Only **external** I/O reaches `in`/`out`: the Z180's
  internal registers (at ICR's base) never leave the core.

Cores are **not bus-cycle accurate**. A read inside an instruction is not placed at its own T-state, and wait states
(the Z180's DCNTL) are not modelled. This is a known limit (Reply 76), not an oversight.

**Note for the 8085**: today's Python core (`src/hosts/i8085.py`) charges an instruction's T-states *before*
running it, so its callbacks see the end count. Under this contract its successor sees the start count, the same
as the Z180. The Accent SA has no golden vectors yet, so nothing is lost. Its I/O timestamps would move earlier by
one instruction's T-states (a few microseconds), which the review should confirm is acceptable.

## 7. On-chip peripherals

They belong to the core and are clocked by charged T-states (1.6):
- **Z180**: PRT0/1, the FRC, ASCI0/1, CSIO, DMA0/1, the MMU, the refresh and wait-state registers.
- **8085**: RIM/SIM, and SID/SOD through `serial_pin` / `serial_out_pin`.

The Z180's ASCI works at the byte level: `serial_rx` is asked for the next byte when the receiver can take one, and
`serial_tx` is given a byte when the transmitter sends it. Its baud clock is advanced at step boundaries by the
T-states elapsed, one tick per divisor period (16 at the Braille Lite's setting) **(golden)**. Today the board does
this through the core's internals; it moves inside.

Undefined opcodes: the Z180 takes TRAP. The 8085's undocumented opcodes and flags follow the reused MAME core
*and* explicit tests (Reply 76: upstream documents some as uncertain).

## 8. Instances, threads, determinism

No global or static mutable state: every call names its instance, and every callback gets `ctx`. Distinct instances
may run on distinct threads. A callback may call `*_cycles`, `*_steps`, `*_pc` and `*_set_irq`; it must not call
`*_step` or `*_run`. The same inputs give the same outputs: no wall clock, and no allocation-dependent behaviour.
`*_destroy` frees everything (today's z180emu has no destructor).

## 9. How a core is accepted

- **The z180emu adapter**: the Braille Lite goldens, English and Spanish, bit for bit, through `bl_board.c` rewritten
  onto `cpu.h`.
- **A new Z180** (a clean-room MIT core, written from Zilog's manual by an author who has not read z180emu or MAME's
  Z180): lockstep against the adapter on the Braille Lite firmware, comparing registers, `cycles`, `steps` and every
  bus event per step, plus the goldens, plus Z80 instruction exercisers for what the firmware never runs. Any
  difference is explained, as a quirk kept deliberately or a bug fixed deliberately, before anything is accepted.
- **The 8085** (MAME's BSD-3 core behind `cpu.h`, its notices kept): per-step traces against the Python core on the
  Accent SA firmware, 8080/8085 exercisers, and specific interrupt, flag and timing tests.

## 10. Licences

- `cpu.h` and this contract: MIT.
- The z180emu adapter links a GPL-2.0-or-later core, so any build containing it is GPL.
- MAME's 8085 files keep their BSD-3-Clause notices.
- A clean-room Z180 would be MIT.
