"""Generate i86_mame_machine.cpp from MAME's i86.cpp and i86inline.h: the 8086's machine code this library needs, copied
from exact upstream line ranges, with the framework calls replaced by named substitutions (each listed below and marked
'CHANGED' in the output).  It refuses to run if a source is not the pinned revision or an anchor line has moved.

    python src/csrc/cpu/extract_i86_machine.py <folder holding i86.cpp and i86inline.h, or a MAME checkout>

The pinned sources: MAME c0d3677674339a489efbad881047012ce8c2cc96, src/devices/cpu/i86/ (sha256 below, of the files as
upstream stores them, LF line ends: a Windows checkout's CRLF is normalised first; mame_i86/PINNED.txt).  They are
BSD-3-Clause (copyright-holders: Carl); the generated file keeps that notice.  What is left out: the MAME device framework
(constructors, address spaces, the state table and save states, the disassembler), the memory and port accessors (ours,
in i86_mame.hpp: the cpu_bus callbacks) and execute_run -- replaced by our own step driver in i86_mame.cpp, see
CONTRACT.md; execute_run's own opcodes (POP CS, the shifts by CL, ESC) are kept, as execute_op().  The 80186 and 80286
(i186.cpp, i286.cpp) are not taken: the Accent-mini's driver needs only the 8086 (README.md).  The class it compiles
against is i86_mame.hpp.
"""
import hashlib
import os
import sys

REVISION = "c0d3677674339a489efbad881047012ce8c2cc96"
SHA256 = {
    "i86.cpp": "22138681b88981a49721944952f1eba4f060d0e0c924c194994847a46e062a7a",
    "i86inline.h": "8c060f5f465811da6c9a8640c2ca9181d30fc386060f3521859245bb476187ec",
    "i86.h": "274ebbde7817417c3add1534dce94d1ecaea8ddbc1c5f09a7c091828fb26c81a",   # restated in i86_mame.hpp
}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "i86_mame_machine.cpp")

# (file, first line, last line, the text that must open the range, what it is); line numbers as sed and grep count them
RANGES = [
    ("i86inline.h", 5, 968, "#define CF", "the flag macros and every inline helper (fetch_word, get_ea, the ALU, "
                                            "the string ops, PUSH/POP)"),
    ("i86.cpp", 32, 111, "#define I8086_NMI_INT_VECTOR 2", "the NMI vector, the cycle tables"),
    ("i86.cpp", 407, 433, "static const BREGS reg_name[8]", "the constructor's tables (parity, ModRM)"),
    ("i86.cpp", 560, 596, "void i8086_common_cpu_device::device_reset()", "reset"),
    ("i86.cpp", 599, 616, "void i8086_common_cpu_device::interrupt(int int_num, int trap)", "an interrupt's entry"),
    ("i86.cpp", 619, 639, "void i8086_common_cpu_device::execute_set_input( int inptnum, int state )", "the input lines"),
    ("i86.cpp", 699, 711, "uint32_t i8086_common_cpu_device::calc_addr(", "segment:offset to an address"),
    ("i86.cpp", 283, 369, "switch(op)", "execute_run's own opcodes: POP CS, the shifts by CL, ESC"),
    ("i86.cpp", 713, 2620, "bool i8086_common_cpu_device::common_op(uint8_t op)", "every other instruction"),
]

# our lines around a range
BEFORE = {
    ("i86.cpp", 407): ["void i8086_common_cpu_device::init_tables()   // CHANGED: the constructor's body; its initialiser "
                       "list is our constructor's", "{"],
    ("i86.cpp", 283): ["// CHANGED (ours): execute_run's own opcodes, as a function the step driver (i86_mame.cpp) calls with",
                       "// the fetched opcode.  The lines are execute_run's switch; the loop, the interrupt dispatch and the",
                       "// trap around it are the step driver's.",
                       "void i8086_common_cpu_device::execute_op(uint8_t op)", "{"],
}
AFTER = {
    ("i86.cpp", 407): ["}"],
    ("i86.cpp", 283): ["}"],
}

# (upstream text, replacement, why, how many times it must match) -- applied to the copied text only
SUBS = [
    ("void i8086_common_cpu_device::interrupt(int int_num, int trap)\n{\n",
     "void i8086_common_cpu_device::interrupt(int int_num, int trap)\n{\n"
     "\t// CHANGED: the host's seam (CONTRACT.md 4, cpu_bus.intercept): an INT n, INT 3, INTO or divide error raised by\n"
     "\t// the instruction may be serviced by the host instead -- nothing pushed, the instruction ends here\n"
     "\tif (drv_intercept(int_num, trap))\n\t\treturn;\n",
     "the INT seam", 1),
    ("\t\t\t//logerror(\"%s: %06x: HALT\\n\", tag(), m_pc);\n\t\t\tm_icount = 0;\n\t\t\tm_halt = true;",
     "\t\t\t//logerror(\"%s: %06x: HALT\\n\", tag(), m_pc);\n"
     "\t\t\tCLK(HLT);   // CHANGED: HLT's own 2 T-states (the table's HLT); upstream ends the slice (m_icount = 0)\n"
     "\t\t\tm_halt = true;",
     "HLT's T-states", 1),
    ("\t\t\t{\n\t\t\t\tm_icount = 0;\n\t\t\t\tm_ip--;\n\t\t\t}",
     "\t\t\t{\n"
     "\t\t\t\tCLK(WAIT);   // CHANGED: a WAIT that waits is a slot of WAIT's T-states per step; upstream ends the slice\n"
     "\t\t\t\tm_ip--;\n\t\t\t}",
     "WAIT's slot", 1),
]


def source_path(arg, name):
    for p in (os.path.join(arg, name), os.path.join(arg, "src", "devices", "cpu", "i86", name)):
        if os.path.isfile(p):
            return p
    sys.exit("no %s in %s" % (name, arg))


def load(arg, name):
    raw = open(source_path(arg, name), "rb").read().replace(b"\r\n", b"\n")
    if hashlib.sha256(raw).hexdigest() != SHA256[name]:
        sys.exit("%s is not the pinned revision %s (sha256 differs)" % (name, REVISION))
    return raw.decode("utf-8").split("\n")


def main():
    src = {name: load(sys.argv[1], name) for name in SHA256}
    cpp = src["i86.cpp"]
    assert cpp[0] == "// license:BSD-3-Clause" and cpp[1] == "// copyright-holders:Carl", cpp[:2]
    assert src["i86inline.h"][0:2] == cpp[0:2], "i86inline.h's notice differs"
    head_end = cpp.index('#include "emu.h"')
    out = cpp[0:2] + [
        "//",
        "// i86_mame_machine.cpp -- GENERATED by extract_i86_machine.py from MAME's src/devices/cpu/i86/i86.cpp and",
        "// i86inline.h at %s (mame_i86/PINNED.txt).  The 8086's machine code," % REVISION,
        "// copied from exact line ranges; framework calls replaced as marked 'CHANGED'.  Do not edit: change the script",
        "// and regenerate.  The class it compiles against: i86_mame.hpp.  Our step driver and the cpu.h API:",
        "// i86_mame.cpp, which includes this file (build that one, never this one alone).",
        "//",
        "// Upstream's header comment, kept:",
    ] + cpp[2:head_end] + [
        '#include "i86_mame.hpp"',
    ]
    for name, a, b, opener, what in RANGES:
        seg = src[name][a - 1:b]
        assert seg[0].strip().startswith(opener.strip()), "%s line %d moved: %r" % (name, a, seg[0])
        out.append("")
        out.append("// ---- upstream %s lines %d-%d: %s " % (name, a, b, what) + "-" * max(0, 50 - len(what)))
        out.extend(BEFORE.get((name, a), []))
        out.extend(seg)
        out.extend(AFTER.get((name, a), []))
    text = "\n".join(out) + "\n"
    for old, new, why, n in SUBS:
        assert text.count(old) == n, "substitution anchor not found %d time(s): %s" % (n, why)
        text = text.replace(old, new)
    open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote %s (%d lines from %d upstream ranges)" % (os.path.basename(OUT), text.count("\n"), len(RANGES)))


if __name__ == "__main__":
    main()
