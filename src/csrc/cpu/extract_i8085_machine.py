"""Generate i8085_mame_machine.cpp from MAME's i8085.cpp: the 8085's machine code this library needs, copied from exact
upstream line ranges, with the framework calls replaced by named substitutions (each listed below and marked
'CHANGED' in the output).  It refuses to run if the source is not the pinned revision or an anchor line has moved.

    python src/csrc/cpu/extract_i8085_machine.py <folder holding i8085.cpp, or a MAME checkout>

The pinned source: MAME 1c924ea79551fdfc3960bfa8ceb93fc1928c661b, src/devices/cpu/i8085/i8085.cpp (sha256 below;
mame_i8085/PINNED.txt).  It is BSD-3-Clause (copyright-holders: Juergen Buchmueller, Roberto Fresca, Grull Osgo); the
generated file keeps that notice.  What is left out: the MAME device framework (constructors, address-space
configuration, the state table and save states, the disassembler, execute_run -- replaced by our own step driver in
i8085_mame.cpp, see CONTRACT.md).  The class it compiles against is i8085_mame.hpp.
"""
import hashlib
import os
import sys

REVISION = "1c924ea79551fdfc3960bfa8ceb93fc1928c661b"
SHA256 = "d2f2f84466ee7f434a76de4a96b4f1110620bf7c5611b4ec00f46ca62270dc0e"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "i8085_mame_machine.cpp")

# (first line, last line, the text that must open the range, what it is); line numbers as sed and grep count them
RANGES = [
    (129, 150, "constexpr u8 SF", "flag bits, RIM/SIM bits, the RESTART addresses"),
    (157, 205, "/* cycles lookup */", "the cycle tables"),
    (275, 292, "void i8085a_cpu_device::init_tables()", "the flag tables"),
    (294, 315, "void i8085a_cpu_device::device_start()", "the power-on values (device_start's first part)"),
    (378, 389, "void i8085a_cpu_device::device_reset()", "reset"),
    (486, 611, "void i8085a_cpu_device::execute_set_input(int irqline, int state)",
     "the input lines, leaving HALT, the interrupt check"),
    (618, 863, "void i8085a_cpu_device::set_sod(int state)", "SOD, INTE, status, RIM, the bus and ALU helpers"),
    (892, 1663, "void i8085a_cpu_device::execute_one(int opcode)", "the instructions"),
]

# after these ranges, lines of our own
AFTER = {
    (129, 150): [
        "",
        "// CHANGED (ours): the T-states of a TRAP or RST 5.5/6.5/7.5 acceptance.  Upstream charges 11, the 8080's RST.",
        "// Intel, MCS-80/85 Family User's Manual (Jan 1983): RST is 'States: 12 (8085), 11 (8080)' (printed page 5-15",
        "// ff., the RST listing), and the hardware RESTART is that instruction generated internally -- 'it executes an",
        "// OF machine cycle without issuing RD, generating the RESTART opcode instead' (section 2.3.5, printed page",
        "// 2-15, Figure 2-19: M1 (BI) T1-T6, then two MW cycles).",
        "constexpr int I8085_ACCEPT_T = 12;",
    ],
    (294, 315): ["}   // CHANGED: closes init_state(); upstream's device_start goes on with framework set-up"],
}

# (upstream text, replacement, why, how many times it must match) -- applied to the copied text only
SUBS = [
    ("std::popcount(i)", "__builtin_popcount(i)   /* CHANGED: std::popcount is C++20; we build C++17 */",
     "popcount", 1),
    ("void i8085a_cpu_device::device_start()\n{",
     "void i8085a_cpu_device::init_state()   // CHANGED: was device_start(); only its power-on values are kept\n{",
     "device_start", 1),
    ("\t\tm_icount -= 11;", "\t\tm_icount -= I8085_ACCEPT_T;   // CHANGED: 12 (Intel), not 11", "acceptance T-states", 4),
    ("\t\tu8 vector = read_inta();\n\n\t\t// use the resulting vector as an opcode to execute\n\t\tset_inte(0);\n"
     "\t\tLOG(\"i8085 take int $%02x\\n\", vector);\n\t\texecute_one(vector);",
     "\t\t// CHANGED: acceptance only (CONTRACT.md 4): nothing is read here.  The step's instruction at E is the\n"
     "\t\t// injected one, its bytes from the acknowledge (i8085_mame.cpp, drv_injected_instruction)\n"
     "\t\tset_inte(0);\n\t\tm_inject_pending = true;",
     "INTR injected at E", 1),
    ("\t\t\t\t// check for revealed interrupts\n\t\t\t\tcheck_for_interrupts();",
     "\t\t\t\t// CHANGED: no acceptance inside an instruction (CONTRACT.md 1): an interrupt SIM unmasks is\n"
     "\t\t\t\t// sampled at the next step's A, as Intel samples at the end of each instruction",
     "SIM's interrupt check", 1),
    ("\t\tm_PC.w.l += 2;",
     "\t\tif (!m_in_acknowledge)   // CHANGED: an injected instruction's PC is held (Intel: INA inhibits the PC)\n"
     "\t\t\tm_PC.w.l += 2;",
     "a branch not taken in an acknowledge", 2),
]


def source_path(arg):
    for p in (os.path.join(arg, "i8085.cpp"), os.path.join(arg, "src", "devices", "cpu", "i8085", "i8085.cpp")):
        if os.path.isfile(p):
            return p
    sys.exit("no i8085.cpp in %s" % arg)


def main():
    src = source_path(sys.argv[1])
    raw = open(src, "rb").read()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        sys.exit("%s is not the pinned revision %s (sha256 differs)" % (src, REVISION))
    # split on newlines only, as sed and grep count lines
    lines = [l.rstrip("\r") for l in raw.decode("utf-8").split("\n")]
    assert lines[0] == "// license:BSD-3-Clause", lines[0]
    out = lines[0:3] + [
        "//",
        "// i8085_mame_machine.cpp -- GENERATED by extract_i8085_machine.py from MAME's src/devices/cpu/i8085/i8085.cpp",
        "// at %s (mame_i8085/PINNED.txt).  The 8085's machine code, copied" % REVISION,
        "// from exact line ranges; framework calls replaced as marked 'CHANGED'.  Do not edit: change the script and",
        "// regenerate.  The class it compiles against: i8085_mame.hpp.  Our step driver and the cpu.h API:",
        "// i8085_mame.cpp, which includes this file (build that one, never this one alone).",
        "//",
        "// Upstream's header comment, kept:",
    ] + lines[3:114] + [
        "",
        '#include "i8085_mame.hpp"',
    ]
    for a, b, opener, what in RANGES:
        seg = lines[a - 1:b]
        assert seg[0].strip().startswith(opener.strip()), "line %d moved: %r" % (a, seg[0])
        out.append("")
        out.append("// ---- upstream i8085.cpp lines %d-%d: %s " % (a, b, what) + "-" * max(0, 60 - len(what)))
        out.extend(seg)
        out.extend(AFTER.get((a, b), []))
    text = "\n".join(out) + "\n"
    for old, new, why, n in SUBS:
        assert text.count(old) == n, "substitution anchor not found %d time(s): %s" % (n, why)
        text = text.replace(old, new)
    open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote %s (%d lines from %d upstream ranges)" % (os.path.basename(OUT), text.count("\n"), len(RANGES)))


if __name__ == "__main__":
    main()
