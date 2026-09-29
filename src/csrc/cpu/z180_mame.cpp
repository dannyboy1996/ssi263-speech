// z180_mame.cpp -- cpu.h's Z180 on MAME's Z180 core: the CORRECTED path (CONTRACT.md 1-2, 4-7, 9).
//
// One translation unit: MAME's code (z180_mame_machine.cpp, which includes the vendored instruction files) is
// included here, because its register macros (_PCD, PUSH, ...) and inline helpers (ROP, RM, ...) must be seen by
// the step driver and should inline into it.  Build this file and z180_asci.cpp; never z180_mame_machine.cpp alone.
//
// The driver is ours.  It follows MAME's execute_run (z180.cpp, the revision in mame_z180/PINNED.txt) piece by
// piece, one step at a time, in the contract's phases:
//   A  NMI (edge) first, as execute_run's entry check but at every step; else the maskable sources through MAME's
//      check_interrupts (IFF1 and the EI shadow gate them).  A burst-DMA step accepts only the NMI, which stops DMA.
//   B  the acceptance T-states, and the on-chip timers clocked by them (handle_io_timers).
//   C  the EI shadow ends.
//   D  steps++, the saved PC, the ASCI catch-up, bus->boundary.
//   E  one instruction, or one HALT/SLP slot of 3 T-states (as execute_run), or one burst-DMA chunk.
//   F  its T-states, and the timers clocked by them.
//   G  cycle-stolen DMA as execute_run does it after each instruction (channel 0, one transfer, then channel 1),
//      only while DME is set, and none in SLEEP (Zilog: SLEEP stops the DMAC).
//
// Differences from MAME, each deliberate:
//   - NMI is sampled at every step (MAME: at execute_run's entry only; the legacy core keeps that quirk).
//   - Burst DMA runs in fixed chunks of Z180_DMA_CHUNK bytes, one chunk per step (MAME: until the slice's budget
//     runs out).  z180_dma0(0) moves exactly one byte: its loop stops once the cycles spent exceed the budget.
//   - SLP: an individually enabled interrupt request ends SLEEP even with IEF1 = 0, and the CPU continues after SLP
//     without vectoring (Zilog Z8018x UM, SLEEP).  MAME keeps the CPU asleep until a request is accepted.  HALT is
//     unchanged: a request masked by IEF1 leaves it halted.
//   - IOSTOP (ICR bit 5) stops the PRT/FRC and the ASCI (Zilog UM); MAME clocks them regardless.
//   - The ASCI is ours (z180_asci.hpp); its interrupt request is a level.
// Known limits: MAME has no TRAP (an undefined opcode is logged and skipped); nor has the legacy core.  Memory and
// I/O wait states are MAME's: DCNTL's MWI/IWI are charged on every access (the legacy core charged them only in
// DMA).  Every one of these goes into the comparison with the legacy core before new goldens.
#include "z180_mame_machine.cpp"

#include <new>

// ---- the driver's pieces, as members (MAME's macros name members) ----------------------------------------------

bool z180_device::drv_burst() const
{
    return (m_dstat & Z180_DSTAT_DME) && (m_dstat & Z180_DSTAT_DE0)
           && (m_dmode & Z180_DMODE_MMOD) == Z180_DMODE_MMOD;
}

bool z180_device::drv_sleep_wake_request()
{
    if (m_irq_state[0] != CLEAR_LINE && (m_itc & Z180_ITC_ITE0)) return true;
    if (m_irq_state[1] != CLEAR_LINE && (m_itc & Z180_ITC_ITE1)) return true;
    if (m_irq_state[2] != CLEAR_LINE && (m_itc & Z180_ITC_ITE2)) return true;
    if ((m_tcr & Z180_TCR_TIE0) && (m_tcr & Z180_TCR_TIF0)) return true;
    if ((m_tcr & Z180_TCR_TIE1) && (m_tcr & Z180_TCR_TIF1)) return true;
    if (m_int_pending[Z180_INT_DMA0] || m_int_pending[Z180_INT_DMA1]) return true;
    return m_csio->check_interrupt() || m_asci[0]->check_interrupt() || m_asci[1]->check_interrupt();
}

int z180_device::drv_accept(bool burst)
{
    if (m_nmi_pending) {                  // execute_run's NMI entry, as upstream
        LEAVE_HALT();
        m_dstat &= ~Z180_DSTAT_DME;       // NMI disables DMA transfers
        m_IFF2 = m_IFF1;
        m_IFF1 = 0;
        PUSH( PC );
        _PCD = 0x0066;
        m_nmi_pending = 0;
        return 11;
    }
    if (burst)
        return 0;                         // the DMAC has the bus
    int t = check_interrupts();
    if (t == 0 && m_HALT == 2 && !m_IFF1 && drv_sleep_wake_request())
        LEAVE_HALT();                     // CHANGED: SLEEP ends without service; on after SLP
    return t;
}

int z180_device::drv_instruction()
{
    if (m_HALT)
        return 3;                         // a HALT or SLP slot (execute_run: 3 T-states)
    _PPC = _PCD;
    m_R++;
    m_extra_cycles = 0;
    int t = exec_op(ROP());
    return t + m_extra_cycles;
}

int z180_device::drv_burst_chunk()
{
    int t = 0;
    for (int n = 0; n < Z180_DMA_CHUNK && drv_burst(); n++)
        t += z180_dma0(0);                // one byte per call
    return t;
}

// ---- cpu.h ------------------------------------------------------------------------------------------------------

struct z180 {
    cpu_bus bus;
    z8s180_device *dev;
    uint64_t cycles, steps;
    uint32_t pc;
    int line[4];                          // the external lines as last set (Z180_INT0..Z180_NMI), kept over reset
};

namespace {

inline void charge(z180 *c, int t)       // T-states pass: counted, and the PRT/FRC clocked unless IOSTOP
{
    if (t <= 0)
        return;
    c->cycles += (uint64_t)t;
    if (!(c->dev->m_iocr & Z180_IOCR_IOSTP))
        c->dev->handle_io_timers(t);
}

void apply_lines(z180 *c)                // after a reset: level lines re-sampled, the NMI edge not re-armed
{
    for (int k = 0; k < 3; k++)
        c->dev->execute_set_input(Z180_INPUT_LINE_IRQ0 + k, c->line[k] ? ASSERT_LINE : CLEAR_LINE);
    c->dev->m_nmi_state = c->line[Z180_NMI] ? ASSERT_LINE : CLEAR_LINE;
    c->dev->m_nmi_pending = 0;
}

}  // namespace

extern "C" {

z180 *z180_create(const cpu_bus *bus, double clock_hz)
{
    (void)clock_hz;                       // the core counts T-states; the board turns them into time
    z180 *c = new (std::nothrow) z180();
    if (!c)
        return nullptr;
    c->bus = *bus;
    c->dev = new (std::nothrow) z8s180_device(&c->bus);
    if (!c->dev) {
        delete c;
        return nullptr;
    }
    c->dev->init_tables();                // allocates and fills this instance's flag tables
    z180_reset(c);
    return c;
}

void z180_destroy(z180 *c)
{
    if (!c)
        return;
    delete c->dev;
    delete c;
}

void z180_reset(z180 *c)
{
    c->dev->device_reset();
    c->dev->m_asci_0.reset();
    c->dev->m_asci_1.reset();
    c->dev->m_csio_0.reset();
    c->cycles = c->steps = 0;
    c->pc = 0;
    apply_lines(c);
}

int z180_step(z180 *c)
{
    z180_device &d = *c->dev;
    bool burst = d.drv_burst();

    int acc = d.drv_accept(burst);        // A
    charge(c, acc);                       // B
    d.m_after_EI = 0;                     // C
    if (acc && burst)
        burst = d.drv_burst();            // an NMI stopped the DMA: this step runs an instruction

    c->steps++;                           // D
    c->pc = d.m_PC.w.l;
    if (d.m_iocr & Z180_IOCR_IOSTP) {
        d.m_asci_0.hold(c->cycles);
        d.m_asci_1.hold(c->cycles);
    } else {
        d.m_asci_0.catch_up(c->cycles);
        d.m_asci_1.catch_up(c->cycles);
    }
    if (c->bus.boundary)
        c->bus.boundary(c->bus.ctx, c->pc);

    bool sleeping = d.m_HALT == 2;
    int ins = burst ? d.drv_burst_chunk() : d.drv_instruction();   // E
    charge(c, ins);                       // F

    int dma = 0;                          // G -- not when burst mode is on now (execute_run re-checks it first:
    if (!burst && !sleeping && (d.m_dstat & Z180_DSTAT_DME) && !d.drv_burst()) {   // the next step is a chunk)
        int t = d.z180_dma0(6);
        charge(c, t);
        dma += t;
        t = d.z180_dma1();
        charge(c, t);
        dma += t;
    }
    return acc + ins + dma;
}

uint64_t z180_run(z180 *c, uint64_t budget)
{
    uint64_t done = 0;
    while (done < budget)
        done += (uint64_t)z180_step(c);
    return done;
}

void z180_set_irq(z180 *c, int line, int asserted)
{
    if (line < Z180_INT0 || line > Z180_NMI)
        return;
    c->line[line] = asserted ? 1 : 0;
    // cpu.h's lines are not MAME's input numbers: Z180_NMI (3) is MAME's DREQ0
    c->dev->execute_set_input(line == Z180_NMI ? INPUT_LINE_NMI : Z180_INPUT_LINE_IRQ0 + line,
                              asserted ? ASSERT_LINE : CLEAR_LINE);
}

uint64_t z180_cycles(const z180 *c) { return c->cycles; }
uint64_t z180_steps(const z180 *c) { return c->steps; }
uint32_t z180_pc(const z180 *c) { return c->pc; }

void z180_regs_get(const z180 *c, z180_regs *out)
{
    const z180_device &d = *c->dev;
    out->af = d.m_AF.w.l; out->bc = d.m_BC.w.l; out->de = d.m_DE.w.l; out->hl = d.m_HL.w.l;
    out->af2 = d.m_AF2.w.l; out->bc2 = d.m_BC2.w.l; out->de2 = d.m_DE2.w.l; out->hl2 = d.m_HL2.w.l;
    out->ix = d.m_IX.w.l; out->iy = d.m_IY.w.l; out->sp = d.m_SP.w.l; out->pc = d.m_PC.w.l;
    out->i = d.m_I; out->r = (uint8_t)((d.m_R & 0x7f) | (d.m_R2 & 0x80)); out->im = d.m_IM;
    out->iff1 = d.m_IFF1; out->iff2 = d.m_IFF2; out->halted = d.m_HALT ? 1 : 0; out->sleeping = d.m_HALT == 2;
}

}  // extern "C"
