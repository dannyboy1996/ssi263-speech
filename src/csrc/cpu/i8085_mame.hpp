// i8085_mame.hpp -- the class MAME's 8085 code compiles against, without the MAME framework.
//
// MAME's machine code (i8085_mame_machine.cpp, generated from i8085.cpp by extract_i8085_machine.py) is written as
// members of `i8085a_cpu_device`.  This header declares that class with the member names it uses, taken from MAME's
// i8085.h at the revision in mame_i8085/PINNED.txt, and replaces what the framework provided:
//
//   MAME                                   here
//   memory_access<...> m_program etc.      i8085_mem / i8085_io: the cpu_bus callbacks, with its ctx; 16-bit
//                                          addresses and 8-bit ports (PAIR's upper bytes are never meaningful)
//   devcb_read8 m_in_inta_func             i8085_inta: the INTR acknowledge's bytes, cpu_bus.irq_ack(ctx,
//                                          I8085_INTR, n) with n = 0, 1, ... from each acceptance; -1 or none = FFh
//   devcb_read_line m_in_sid_func          cpu_bus.serial_pin(ctx, I8085_PIN_SID); none = 0
//   devcb_write_line m_out_sod_func        cpu_bus.serial_out_pin(ctx, I8085_PIN_SOD, level); none = nothing
//   m_out_status_func, m_out_inte_func     unconnected outputs
//   standard_irq_callback                  nothing: TRAP and RST 5.5-7.5 are internal RESTARTs, no bus read
//   LOG                                    discarded
//   device_start                           init_state(): its power-on values (then init_tables())
//
// The class is the 8085A only (is_8085() is true; the 8080 variants are not built).  i8085_mame.cpp holds our step
// driver and the cpu.h API.  Licence: this header is ours (MIT) but restates MAME's declarations (BSD-3-Clause,
// copyright-holders: Juergen Buchmueller, Roberto Fresca, Grull Osgo; see mame_i8085/LICENSE-BSD-3-Clause.txt).
#ifndef SSI263_I8085_MAME_HPP
#define SSI263_I8085_MAME_HPP

#include <cstdint>
#include <iterator>

#include "cpu.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "PAIR below is laid out for a little-endian host, as MAME's is on one"
#endif

// ---- MAME's basic types (emu/emucore.h, lib/util), little-endian layout -----------------------------------------
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

union PAIR {
    struct { u8 l, h, h2, h3; } b;
    struct { u16 l, h; } w;
    struct { s8 l, h, h2, h3; } sb;
    struct { s16 l, h; } sw;
    u32 d;
    s32 sd;
};

enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { INPUT_LINE_NMI = 32 };             // outside the 8085's own input lines (0-3), as in MAME

#define LOG(...) ((void)0)

// ---- i8085.h's constants ----------------------------------------------------------------------------------------
#define I8085_INTR_LINE     0
#define I8085_RST55_LINE    1
#define I8085_RST65_LINE    2
#define I8085_RST75_LINE    3
#define I8085_TRAP_LINE     INPUT_LINE_NMI

// ---- the framework's stand-ins ----------------------------------------------------------------------------------
struct i8085_mem {                        // m_cprogram, m_program: memory
    const cpu_bus *bus;
    u8 read_byte(u32 a) const { return bus->read(bus->ctx, a & 0xffff); }
    void write_byte(u32 a, u8 v) const { bus->write(bus->ctx, a & 0xffff, v); }
};

struct i8085_opcodes {                    // m_copcodes: opcode fetches
    const cpu_bus *bus;
    u8 read_byte(u32 a) const { return bus->fetch ? bus->fetch(bus->ctx, a & 0xffff) : bus->read(bus->ctx, a & 0xffff); }
};

struct i8085_io {                         // m_io: I/O ports (8-bit)
    const cpu_bus *bus;
    u8 read_byte(u32 p) const { return bus->in(bus->ctx, (uint16_t)(p & 0xff)); }
    void write_byte(u32 p, u8 v) const { bus->out(bus->ctx, (uint16_t)(p & 0xff), v); }
};

// An INTR acknowledge (CONTRACT.md 4): each read takes the next byte of the injected instruction from the bus.
struct i8085_inta {
    const cpu_bus *bus;
    int n;                                // the next byte's index; 0 at each acceptance
    bool isunset() const { return false; }
    u8 operator()(u32) { int v = bus->irq_ack ? bus->irq_ack(bus->ctx, I8085_INTR, n) : -1; n++; return v < 0 ? 0xff : (u8)v; }
};

struct i8085_sid {                        // m_in_sid_func
    const cpu_bus *bus;
    int operator()() const { return bus->serial_pin ? (bus->serial_pin(bus->ctx, I8085_PIN_SID) ? 1 : 0) : 0; }
};

struct i8085_sod {                        // m_out_sod_func
    const cpu_bus *bus;
    void operator()(int level) const { if (bus->serial_out_pin) bus->serial_out_pin(bus->ctx, I8085_PIN_SOD, level); }
};

struct i8085_unconnected {                // m_out_status_func, m_out_inte_func
    bool isunset() const { return true; }
    void operator()(int) const {}
};

// ---- the class --------------------------------------------------------------------------------------------------
// Everything is public: the step driver (i8085_mame.cpp) is part of the core and reads the registers directly.
class i8085a_cpu_device {
public:
    explicit i8085a_cpu_device(const cpu_bus *bus)
        : m_in_inta_func{bus, 0}, m_in_sid_func{bus}, m_out_sod_func{bus},
          m_cprogram{bus}, m_program{bus}, m_copcodes{bus}, m_io{bus}
    {
        m_inject_pending = false;
        m_icount = 0;
        m_status = 0;
        m_PC.d = m_SP.d = m_AF.d = m_BC.d = m_DE.d = m_HL.d = m_WZ.d = 0;
    }

    // ---- our step driver's pieces (i8085_mame.cpp) ----
    bool m_inject_pending;                // A accepted INTR; E runs the injected instruction
    int drv_accept();                     // phase A; returns the acceptance T-states
    int drv_instruction();                // phase E: one instruction, the injected one, or a HALT slot
    int drv_injected_instruction();

    // the framework's calls, answered
    u8 standard_irq_callback(int, u32) { return 0xff; }

    // ---- from i8085.h ----
    void init_state();                    // the generated file: device_start's power-on values, then init_tables()
    void device_reset();
    void execute_set_input(int irqline, int state);

    i8085_inta m_in_inta_func;
    i8085_unconnected m_out_status_func;
    i8085_unconnected m_out_inte_func;
    i8085_sid m_in_sid_func;
    i8085_sod m_out_sod_func;

    PAIR m_PC, m_SP, m_AF, m_BC, m_DE, m_HL, m_WZ;
    u8 m_halt;
    u8 m_im;             // interrupt mask (8085A only)
    u8 m_status;         // status word

    u8 m_after_ei;       // post-EI processing; starts at 2, check for ints at 0
    u8 m_nmi_state;      // raw NMI line state
    u8 m_irq_state[4];   // raw IRQ line states
    bool m_trap_pending; // TRAP interrupt latched?
    u8 m_trap_im_copy;   // copy of IM register when TRAP was taken
    u8 m_sod_state;      // state of the SOD line
    bool m_in_acknowledge;

    u8 m_ietemp;         // import/export temp space

    i8085_mem m_cprogram, m_program;
    i8085_opcodes m_copcodes;
    i8085_io m_io;
    int m_icount;

    // cycles lookup
    static const u8 lut_cycles_8080[256];
    static const u8 lut_cycles_8085[256];
    u8 lut_cycles[256];

    // flags lookup
    u8 lut_zs[256];
    u8 lut_zsp[256];

    int ret_taken() { return 6; }
    int jmp_taken() { return 3; }
    int call_taken() { return 9; }
    bool is_8085() { return true; }

    void set_sod(int state);
    void set_inte(int state);
    void set_status(u8 status);
    u8 get_rim_value();
    void break_halt_for_interrupt();
    u8 read_op();
    u8 read_inta();
    u8 read_arg();
    PAIR read_arg16();
    u8 read_mem(u32 a);
    void write_mem(u32 a, u8 v);
    void op_push(PAIR p);
    PAIR op_pop();
    void check_for_interrupts();
    void execute_one(int opcode);
    void init_tables();

    void op_ora(u8 v);
    void op_xra(u8 v);
    void op_ana(u8 v);
    u8 op_inr(u8 v);
    u8 op_dcr(u8 v);
    void op_add(u8 v);
    void op_adc(u8 v);
    void op_sub(u8 v);
    void op_sbb(u8 v);
    void op_cmp(u8 v);
    void op_dad(u16 v);
    void op_jmp(int cond);
    void op_call(int cond);
    void op_ret(int cond);
    void op_rst(u8 v);
};

#endif
