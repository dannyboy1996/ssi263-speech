"""Generate v40_mame_machine.cpp from MAME's NEC core (src/devices/cpu/nec): the V20/V40 instruction set this library
needs, copied from exact upstream line ranges of several files, with the framework replaced by named substitutions
(each listed below and marked 'CHANGED' in the output).  It refuses to run if a source is not the pinned revision or
an anchor line has moved.

    python src/csrc/cpu/extract_v40_machine.py <MAME checkout, or the folder holding nec.cpp>

The pinned sources: MAME c0d3677674339a489efbad881047012ce8c2cc96, src/devices/cpu/nec/ (sha256 of each file with
LF line ends below; mame_nec/PINNED.txt).  BSD-3-Clause, copyright-holders: Bryan McPhail; the generated file keeps
that notice.  The V40's parameters come from v5x.cpp (copyright-holders: Patrick Mackinlay), which is NOT copied:
v40_device is nec_common_device(8-bit bus, prefetch 4 bytes at 4 clocks, V20_TYPE, no DIV quirk) -- set in
v40_mame.cpp's constructor.  Left out: the device framework (constructors, address spaces, the state table, save
states, the disassembler), execute_run (our step driver in v40_mame.cpp follows it), the V33's memory translation map,
v5x.cpp's on-chip peripherals (the board models what its firmware uses), and the 8080 emulation mode (nec80inst.hxx
and its table): BRKEM entering it is an explicit fault of the core (v40_regs.fault), since no firmware here uses it.
"""
import hashlib
import os
import sys

REVISION = "c0d3677674339a489efbad881047012ce8c2cc96"
SHA256 = {   # of the file with LF line ends (as raw.githubusercontent.com serves it)
    "nec.cpp": "a2456982d4998ca5cdc724080f193f43d9b8ac1eb880d8669194e9e99b3da2a6",
    "nec.h": "d5352079f764d37b9a15c72ea51373697a74004ebb518c16dbee87151f905276",
    "necpriv.ipp": "36852ebd2d3087565d93268fa4dd66c878e2af83b32b1af715076c983452fd66",
    "necea.h": "d67d42551b5bced80727b69ac69041239b632f7a65c7d52d16f0db4d5f291559",
    "necmodrm.h": "541e971a01f429d43e87f8ce74be1dee049d2a0445d3d75e2fedcb2eec8cf6b3",
    "necmacro.h": "e42a9b19cd06437732c3ba102e4ba909ff8affc4abf13e86347152b1fc97b08d",
    "necinstr.h": "2f7b787026e4b533e2208ef3721d355f5f1648054c074feca4f1b6467cc53e4f",
    "necinstr.hxx": "f70cc2ae5064066fe324b3f6486ac29f2fc4c8373cf5a3b879ffb25a8b277b04",
}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "v40_mame_machine.cpp")

# The class's head, ours: in place of nec.h lines 25-68 (the framework's bases and overrides).  nec.h's members
# follow it, from line 69.
CLASS_HEAD = [
    "// ---- ours: the class's head, in place of nec.h lines 25-68 (the framework's bases and overrides) ---------------",
    "class nec_common_device : public v40_shim   // CHANGED: no cpu_device or disassembler bases: v40_mame.hpp's shim",
    "{",
    "public:",
    "\texplicit nec_common_device(const cpu_bus *bus);   // v40_mame.cpp: the V40's parameters (v5x.cpp's v40_device)",
    "",
    "\t// our step driver (v40_mame.cpp)",
    "\tint drv_accept();                         // phase A: an acceptance; returns its clocks",
    "\tint drv_instruction();                    // phase E: one instruction, one REP iteration, or a slot",
    "\tvoid drv_logerror(const char *fmt, ...);  // logerror: undefined and unimplemented opcodes counted",
    "\tu32 drv_undefined;                        // how many",
    "\tu32 drv_undefined_at;                     // the linear address of the last one",
    "\tbool drv_fault;                           // BRKEM entered the 8080 mode, which is not modelled",
    "\tu16 drv_psw();                            // the flags word (CompressFlags)",
    "\tu8 drv_ei_shadow;                         // EI's delay: INT (not NMI) held off through the next instruction",
    "",
    "\t// nec.h lines 33-64, the few kept (no longer virtual)",
    "\tvoid init_tables();                       // device_start()'s tables and first values (the generated file)",
    "\tvoid device_reset();",
    "\tvoid execute_set_input(int inputnum, int state);",
    "\tvoid set_int_line(int state);",
    "\tvoid set_nmi_line(int state);",
    "\tvoid set_poll_line(int state);",
    "",
    "\t// CHANGED: necmodrm.h's static Mod_RM, nec.cpp's static parity_table and necinstr.hxx's static nec_popa_tmp are",
    "\t// members: no global state, so two cores run side by side and on different threads (CONTRACT.md 9)",
    "\tstruct { struct { WREGS w[256]; BREGS b[256]; } reg; struct { WREGS w[256]; BREGS b[256]; } RM; } Mod_RM;",
    "\tuint8_t parity_table[256];",
    "\tunsigned nec_popa_tmp;",
    "",
]

# (file, first line, last line, the text that must open the range, what it is); lines as sed and grep count them
RANGES = [
    ("nec.cpp", 116, 119, "typedef uint8_t BOOLEAN;", "the type names"),
    ("nec.h", 11, 22, "#define NEC_INPUT_LINE_POLL 20", "the POLL input's number, the state numbers"),
    ("necpriv.ipp", 1, 136, "// license:BSD-3-Clause", "the enums, register and memory macros, the clock macros"),
    ("CLASS_HEAD", 0, 0, "", ""),
    ("nec.h", 69, 680, "private:", "the class's members"),
    ("nec.cpp", 197, 203, "offs_t nec_common_device::v33_translate(offs_t addr)",
     "the V33's translation (compiled, never used on the V40)"),
    ("nec.cpp", 221, 268, "void nec_common_device::prefetch()", "the prefetch queue, fetch"),
    ("necinstr.h", 1, 262, "// license:BSD-3-Clause", "the instruction table (the 8080 mode's table left out)"),
    ("necmacro.h", 1, 316, "// license:BSD-3-Clause", "the ALU and flag macros"),
    ("necea.h", 1, 59, "// license:BSD-3-Clause", "the effective addresses"),
    ("necmodrm.h", 1, 106, "// license:BSD-3-Clause", "ModRM"),
    ("nec.cpp", 275, 281, "static uint8_t parity_table[256];", "fetchop"),
    ("nec.cpp", 287, 383, "void nec_common_device::device_reset()", "reset, interrupts, BRK"),
    ("necinstr.hxx", 1, 838, "// license:BSD-3-Clause", "the instructions"),
    ("nec.cpp", 394, 437, "void nec_common_device::set_int_line(int state)", "the input lines"),
    ("nec.cpp", 439, 480, "void nec_common_device::device_start()", "device_start's tables and first values"),
]

AFTER = {
    ("nec.cpp", 439, 480): ["}   // CHANGED: closes init_tables(); upstream's device_start goes on with framework set-up"],
}

# (upstream text, replacement, why, how many times it must match) -- applied to the copied text only
SUBS = [
    ('#include "endianness.h"\n',
     "// CHANGED: endianness.h -> v40_mame.hpp's NATIVE_ENDIAN_VALUE_LE_BE\n", "endianness.h", 1),
    ("namespace {\n", "// CHANGED: not in an anonymous namespace (one translation unit; the class's members use these)\n",
     "anonymous namespace opened", 1),
    ("} // anonymous namespace\n", "// CHANGED: (the anonymous namespace ended here)\n", "anonymous namespace closed", 1),
    ("private:\n", "public:   // CHANGED: all public, for the step driver\n", "private", 2),
    ("protected:\n", "public:   // CHANGED: all public, for the step driver\n", "protected", 1),
    ("\taddress_space *m_program;\n\tmemory_access<24, 0, 0, ENDIANNESS_LITTLE>::cache m_cache8;\n"
     "\tmemory_access<24, 1, 0, ENDIANNESS_LITTLE>::cache m_cache16;\n\n\tstd::function<u8 (offs_t address)> m_dr8;\n"
     "\taddress_space *m_io;\n",
     "\tv40_space *m_program;   // CHANGED: the bus (v40_mame.hpp), not an address space and its caches\n"
     "\tv40_opcodes m_dr8;       // CHANGED: opcode fetches through the bus\n"
     "\t// CHANGED: no m_io: I/O goes through the shim's io_read_byte etc.\n",
     "the address spaces", 1),
    ("\toptional_shared_ptr<uint16_t> m_v33_transtable;\n",
     "\tuint16_t m_v33_transtable[64];   // CHANGED: the V33's map, never used on the V40 (compiled for read_mem_*)\n",
     "the V33's shared table", 1),
    ("static struct {\n\tstruct {\n\t\tWREGS w[256];\n\t\tBREGS b[256];\n\t} reg;\n\tstruct {\n\t\tWREGS w[256];\n"
     "\t\tBREGS b[256];\n\t} RM;\n} Mod_RM;\n",
     "// CHANGED: Mod_RM is a member (the class's head): no global state\n", "Mod_RM", 1),
    ("static uint8_t parity_table[256];\n",
     "// CHANGED: parity_table is a member (the class's head): no global state\n", "parity_table", 1),
    ("static unsigned nec_popa_tmp;\n",
     "// CHANGED: nec_popa_tmp is a member (the class's head): no global state\n", "nec_popa_tmp", 1),
    ("OP( 0xf4, i_hlt ) { m_halted=1; m_icount=0; }",
     "OP( 0xf4, i_hlt ) { m_halted=1; }   // CHANGED: no m_icount=0, the framework's slice end (a step ends anyway)",
     "HLT's slice end", 1),
    ("\t\tm_pending_irq |= INT_IRQ;\n\t\tm_halted = 0;\n",
     "\t\tm_pending_irq |= INT_IRQ;\n"
     "\t\t// CHANGED: HALT is released at the next step's A (v40_mame.cpp), not here at the line: by an acceptance\n"
     "\t\t// when IE = 1, and with IE = 0 without one, execution resuming after the HLT (NEC's V40 data book, 1990,\n"
     "\t\t// printed p.34).  Upstream releases it here, mid-step, so the instruction after the HLT ran first\n",
     "INT and HALT", 1),
    ("\t\tm_pending_irq |= NMI_IRQ;\n\t\tm_halted = 0;\n",
     "\t\tm_pending_irq |= NMI_IRQ;\n"
     "\t\t// CHANGED: HALT is left at the acceptance, the next step's A (v40_mame.cpp): upstream wakes it at the line,\n"
     "\t\t// so a step would run the instruction after the HLT before the NMI is taken\n",
     "NMI and HALT", 1),
    # NEC's interrupt deferrals (V40 data book, 1990, printed p.34, PDF p.159; Instruction Manual U11301EJ5V0UMJ1)
    ("OP( 0xfb, i_ei    ) { SetIF(1);         CLK(2); }",
     "OP( 0xfb, i_ei    ) { SetIF(1);         CLK(2); drv_ei_shadow=1; }   // CHANGED: EI delays INT, not NMI,"
     "\n\t// through the next instruction (data book p.34: \"EI instruction (maskable interrupts only)\"; instruction"
     "\n\t// manual p.80).  Its own counter: m_no_interrupt would hold off NMI too.  Upstream has no delay",
     "EI's delay", 1),
    ("\t\tdefault:   logerror(\"%06x: MOV Sreg - Invalid register\\n\",PC());\n\t}\n}\nOP( 0x8d, i_lea",
     "\t\tdefault:   logerror(\"%06x: MOV Sreg - Invalid register\\n\",PC());\n\t}\n"
     "\tm_no_interrupt=1;   // CHANGED: a move FROM a segment register defers NMI and INT through the next instruction"
     "\n\t// (data book p.34: \"Moves to/from segment registers\"; instruction manual p.98).  Upstream: only moves to\n"
     "}\nOP( 0x8d, i_lea",
     "MOV from a segment register", 1),
    ("OP( 0x07, i_pop_es   ) { POP(Sreg(DS1));    CLKS(12,8,5);   }",
     "OP( 0x07, i_pop_es   ) { POP(Sreg(DS1));    CLKS(12,8,5);   m_no_interrupt=1; }   // CHANGED: as POP SS"
     "\n\t// (instruction manual p.118, POP: \"When dst = sreg\", NMI and INT deferred).  Upstream: POP SS only",
     "POP DS1", 1),
    ("OP( 0x1f, i_pop_ds   ) { POP(Sreg(DS0));        CLKS(12,8,5);   }",
     "OP( 0x1f, i_pop_ds   ) { POP(Sreg(DS0));        CLKS(12,8,5);   m_no_interrupt=1; }   // CHANGED: as POP DS1",
     "POP DS0", 1),
    ("OP( 0x9b, i_wait      ) { if (!m_poll_state) m_ip--; CLK(5); }",
     "OP( 0x9b, i_wait      ) { if (!m_poll_state) m_ip--; else m_no_interrupt=1; CLK(5); }   // CHANGED: a"
     "\n\t// completed POLL defers NMI and INT through the next instruction (data book p.34).  A waiting POLL is left"
     "\n\t// as upstream has it (interruptible at each re-execution): no source says, and cpu.h has no POLL line",
     "POLL", 1),
    ("void nec_common_device::device_start()\n{",
     "void nec_common_device::init_tables()   // CHANGED: was device_start(); its tables and first values only\n{",
     "device_start", 1),
]


def source_dir(arg):
    for d in (arg, os.path.join(arg, "src", "devices", "cpu", "nec")):
        if os.path.isfile(os.path.join(d, "nec.cpp")):
            return d
    sys.exit("no nec.cpp in %s" % arg)


def read(d, name):
    raw = open(os.path.join(d, name), "rb").read().replace(b"\r\n", b"\n")
    if hashlib.sha256(raw).hexdigest() != SHA256[name]:
        sys.exit("%s is not the pinned revision %s (sha256 differs)" % (name, REVISION))
    return raw.decode("utf-8").split("\n")


def main():
    d = source_dir(sys.argv[1])
    files = {name: read(d, name) for name in SHA256}
    nec = files["nec.cpp"]
    assert nec[0] == "// license:BSD-3-Clause" and nec[1] == "// copyright-holders:Bryan McPhail", nec[:2]
    out = nec[0:2] + [
        "//",
        "// v40_mame_machine.cpp -- GENERATED by extract_v40_machine.py from MAME's src/devices/cpu/nec/ at",
        "// %s (mame_nec/PINNED.txt): nec.cpp, nec.h, necpriv.ipp, necea.h," % REVISION,
        "// necmodrm.h, necmacro.h, necinstr.h, necinstr.hxx, from exact line ranges; framework calls replaced as marked",
        "// 'CHANGED'.  Do not edit: change the script and regenerate.  The shim it compiles against: v40_mame.hpp.  Our",
        "// step driver and the cpu.h API: v40_mame.cpp, which includes this file (build that one, never this one alone).",
        "//",
        "// Upstream's header comment (nec.cpp), kept:",
    ] + nec[2:105] + [
        "",
        '#include "v40_mame.hpp"',
    ]
    for name, a, b, opener, what in RANGES:
        if name == "CLASS_HEAD":
            out.append("")
            out.extend(CLASS_HEAD)
            continue
        seg = files[name][a - 1:b]
        assert seg[0].strip().startswith(opener.strip()), "%s line %d moved: %r" % (name, a, seg[0])
        out.append("")
        out.append("// ---- upstream %s lines %d-%d: %s " % (name, a, b, what) + "-" * max(0, 50 - len(what)))
        out.extend(seg)
        out.extend(AFTER.get((name, a, b), []))
    text = "\n".join(out) + "\n"
    for old, new, why, n in SUBS:
        assert text.count(old) == n, "substitution anchor found %d time(s), not %d: %s" % (text.count(old), n, why)
        text = text.replace(old, new)
    open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote %s (%d lines from %d upstream ranges)" % (os.path.basename(OUT), text.count("\n"), len(RANGES) - 1))


if __name__ == "__main__":
    main()
