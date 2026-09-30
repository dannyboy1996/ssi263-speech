// v40_mame.hpp -- what MAME's NEC core compiles against, without the MAME framework.
//
// MAME's NEC V20/V30/V33 core (nec.cpp and its .h/.hxx/.ipp parts, BSD-3-Clause, copyright-holders: Bryan McPhail)
// is copied by extract_v40_machine.py into v40_mame_machine.cpp, as the class `nec_common_device` built on the shim
// below.  This header replaces what the framework provided:
//
//   MAME                                        here
//   emu.h's u8 ... s32, offs_t, BIT()           the same names, plain
//   endianness.h's NATIVE_ENDIAN_VALUE_LE_BE    the little-endian value (a little-endian host is required)
//   address_space m_program (read_byte, ...)    v40_space: the cpu_bus callbacks.  The V40's bus is 8 bits wide, so a
//                                               word is two byte accesses, low byte first, at a and a + 1; addresses
//                                               are 20 bits (the V40's A0-A19), so a + 1 wraps at 1 MB
//   m_dr8 (the opcode cache)                    v40_opcodes: cpu_bus.fetch, or read without one
//   io_read_byte etc. (virtual, the v5x's       v40_shim: cpu_bus.in/out, 16-bit ports; a word is two byte
//   on-chip I/O first)                          accesses, low then high, at p and p + 1.  The V40's on-chip
//                                               peripherals are NOT here: the board models them (cpu.h: only external
//                                               I/O reaches in/out, and on this board the ICU/SCU/TCU are the board's)
//   standard_irq_callback(0, pc)                cpu_bus.irq_ack(ctx, V40_INT, 0): the vector NUMBER (-1: FFh, MAME's
//                                               default vector)
//   logerror(...)                               drv_logerror (v40_mame.cpp): an undefined or unimplemented opcode is
//                                               counted and its address kept (v40_regs.undefined, .undefined_at)
//   LOGMASKED, debugger_*_hook                  nothing
//
// Licence: this header is ours (MIT); the class it serves is MAME's (BSD-3-Clause; mame_nec/LICENSE-BSD-3-Clause.txt).
#ifndef SSI263_V40_MAME_HPP
#define SSI263_V40_MAME_HPP

#include <cstdint>
#include <cstring>

#include "cpu.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "MAME's register union (necbasicregs) and BREGS are laid out here for a little-endian host"
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint32_t offs_t;

template <typename T, typename U> constexpr T BIT(T x, U n) { return (x >> n) & T(1); }

#define NATIVE_ENDIAN_VALUE_LE_BE(le, be) (le)

enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { INPUT_LINE_NMI = 32 };               // as MAME's; the NEC core's INT is its input 0

#define logerror(...) drv_logerror(__VA_ARGS__)
#define LOGMASKED(...) ((void)0)
#define debugger_exception_hook(n) ((void)0)
#define debugger_instruction_hook(a) ((void)0)
#define debugger_wait_hook() ((void)0)

#define V40_ADDR_MASK 0xfffffu              // 20 address lines

// ---- the framework's stand-ins --------------------------------------------------------------------------------
struct v40_space {                          // m_program: memory, 8-bit bus, 20-bit addresses
    const cpu_bus *bus;
    u8 read_byte(offs_t a) const { return bus->read(bus->ctx, a & V40_ADDR_MASK); }
    u16 read_word_unaligned(offs_t a) const
    {
        u16 lo = read_byte(a);
        return (u16)(lo | (read_byte(a + 1) << 8));
    }
    void write_byte(offs_t a, u8 v) const { bus->write(bus->ctx, a & V40_ADDR_MASK, v); }
    void write_word_unaligned(offs_t a, u16 v) const
    {
        write_byte(a, (u8)v);
        write_byte(a + 1, (u8)(v >> 8));
    }
};

struct v40_opcodes {                        // m_dr8: opcode and operand fetches
    const cpu_bus *bus;
    u8 operator()(offs_t a) const
    {
        a &= V40_ADDR_MASK;
        return bus->fetch ? bus->fetch(bus->ctx, a) : bus->read(bus->ctx, a);
    }
};

// The base of MAME's class here, in place of cpu_device: the bus, the I/O and the interrupt acknowledge.
struct v40_shim {
    const cpu_bus *m_bus;
    v40_space m_space;

    explicit v40_shim(const cpu_bus *bus) : m_bus(bus), m_space{bus} {}

    u8 io_read_byte(offs_t a) { return m_bus->in(m_bus->ctx, (uint16_t)a); }
    u16 io_read_word(offs_t a)
    {
        u16 lo = io_read_byte(a);
        return (u16)(lo | (io_read_byte((a + 1) & 0xffff) << 8));
    }
    void io_write_byte(offs_t a, u8 v) { m_bus->out(m_bus->ctx, (uint16_t)a, v); }
    void io_write_word(offs_t a, u16 v)
    {
        io_write_byte(a, (u8)v);
        io_write_byte((a + 1) & 0xffff, (u8)(v >> 8));
    }

    // nec_interrupt's vector number for INT (MAME: standard_irq_callback(0, PC()))
    u32 standard_irq_callback(int, offs_t)
    {
        int v = m_bus->irq_ack ? m_bus->irq_ack(m_bus->ctx, V40_INT, 0) : -1;
        return v < 0 ? 0xff : (u32)(v & 0xff);
    }
};

#endif
