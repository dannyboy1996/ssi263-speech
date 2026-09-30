"""Must-fail controls for test_i86_contract.c: each variant undoes one rule of the MAME 8086 core -- our step driver
(i86_mame.cpp), a named change of the extraction, or an upstream rule the contract relies on (i86_mame_machine.cpp)
-- in a temporary copy, and exactly that rule's tests must then fail.  Proves the tests see what they claim to (the
repo's rule for any fix claim).  The runner guard (exit code, summary line, the full test inventory) is
contract_controls.py's.  Not in run_tests: some seventy-five C++ builds (in parallel) take a few minutes.

    python src/csrc/cpu/i86_controls.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)
from tools import repo_paths  # noqa: E402
from contract_controls import RunError, run_tests  # noqa: E402

DRV, GEN, HPP = "i86_mame.cpp", "i86_mame_machine.cpp", "i86_mame.hpp"
TRAP_TEST = "        if ((m_fire_trap >= 2) && (m_no_interrupt == 0)) {"

# a rule -> (its file, its code, the same code with the rule undone, the tests that must then fail -- and no others)
VARIANTS = {
    # the step driver
    "shadow ends at C": (
        DRV, "    d.drv_shadow();                       // C\n", "",
        ["accept_costs", "halt", "intr_masked", "intr_vector", "reset", "ss_shadow", "sti_shadow", "trap_ss"]),
    "HALT slot 2 T": (
        DRV, "enum { I86_HALT_SLOT_T = 2 };", "enum { I86_HALT_SLOT_T = 3 };", ["halt"]),
    "acceptance PC": (
        DRV, "    c->pc = i86_linear(d);                // A's callbacks see where execution resumes\n", "",
        ["intr_vector"]),
    "a prefix and its instruction are one step": (
        DRV, "        if (!m_seg_prefix_next)\n            break;", "        break;",
        ["prefix_atomic", "rep_iteration", "wait_prefixed"]),
    "the seam only for the instruction's": (
        DRV, "    if (!m_in_instruction || !m_bus->intercept || int_num < 0)",
        "    if (!m_bus->intercept || int_num < 0)", ["hw_not_offered", "trap"]),
    "IF gates INTR": (
        DRV, "        } else if (m_IF) {", "        } else if (1) {",
        ["halt_masked", "intr_masked", "intr_vector", "reset"]),
    "the trap after one instruction": (
        DRV, TRAP_TEST, TRAP_TEST.replace(">= 2", ">= 1"), ["accept_costs", "trap", "trap_ss"]),
    "no trap in the shadow": (
        DRV, TRAP_TEST, "        if (m_fire_trap >= 2) {", ["trap_ss"]),
    "aliases counted": (
        DRV, "        if (i86_alias(op)) {", "        if (0) {", ["aliased"]),
    # the extraction's named changes
    "the INT seam": (
        GEN, "\tif (drv_intercept(int_num, trap))\n\t\treturn;\n", "",
        ["divide_error", "int_costs", "intercept", "intercept_kinds"]),
    "HLT's T-states": (
        GEN, "\t\t\tCLK(HLT);   // CHANGED", "\t\t\tm_icount = 0;   // (undone)", ["halt", "step_cost_floor"]),
    # WAIT, Intel's 3 + 5n (Astra, Reply 106)
    "WAIT: 3 on entry, 5 a recheck (the substitution, upstream's code back)": (
        GEN, "\t\t\twait_clk();   // CHANGED: 3 T on entry, 5 for each recheck (Intel's 3 + 5n; upstream charged 3, "
             "or ended the slice)\n\t\t\tif (m_test_state == 0)\n\t\t\t\twait_hold();   // CHANGED: TEST inactive: IP "
             "back on the WAIT, the next step rechecks (upstream ended the slice)",
        "\t\t\tif (m_test_state == 0)\n\t\t\t{\n\t\t\t\tm_icount = 0;\n\t\t\t\tm_ip--;\n\t\t\t}\n\t\t\telse\n"
        "\t\t\t\tCLK(WAIT);", ["wait", "wait_interrupt", "wait_prefixed"]),
    "WAIT's entry 3 (not a flat 5)": (
        HPP, "        m_icount -= m_wait_continue ? (int)I86_WAIT_RECHECK_T : m_timing[WAIT];",
        "        m_icount -= (int)I86_WAIT_RECHECK_T;", ["wait", "wait_interrupt", "wait_prefixed"]),
    "WAIT's recheck 5 (not the entry's 3 again)": (
        HPP, "        m_icount -= m_wait_continue ? (int)I86_WAIT_RECHECK_T : m_timing[WAIT];",
        "        m_icount -= m_timing[WAIT];", ["wait", "wait_interrupt", "wait_prefixed"]),
    "WAIT holds IP while TEST is inactive": (
        HPP, "        m_ip--;                           // upstream's: IP back on the WAIT -- on its 9Bh byte, after "
             "any prefix: a\n", "",
        ["wait", "wait_interrupt", "wait_prefixed"]),
    "a recheck recognised": (
        DRV, "    m_wait_continue = m_wait_pending && start == m_wait_at;", "    m_wait_continue = false;",
        ["wait", "wait_interrupt", "wait_prefixed"]),
    "an interrupted WAIT is entered again": (
        DRV, "    m_wait_pending = false;\n", "", ["wait_interrupt", "wait_prefixed"]),
    # upstream rules the contract states
    "MOV sreg shadow": (
        GEN, "\t\t\tm_no_interrupt = 1; // Disable IRQ after load segment register.", "", ["ss_shadow", "trap_ss"]),
    "POP SS shadow": (
        GEN, "\t\tcase 0x17: // i_pop_ss\n\t\t\tm_sregs[SS] = POP();\n\t\t\tCLK(POP_SEG);\n\t\t\tm_no_interrupt = 1;",
        "\t\tcase 0x17: // i_pop_ss\n\t\t\tm_sregs[SS] = POP();\n\t\t\tCLK(POP_SEG);", ["ss_shadow"]),
    "STI shadow": (
        GEN, "\t\t\tm_IF = 1;\n\t\t\tm_no_interrupt = 1;", "\t\t\tm_IF = 1;",
        ["intr_level", "intr_masked", "reset", "sti_shadow"]),
    "REP puts IP back": (
        GEN, "\t\t\t\t\tif(!(!ZF && ((next & 6) == 6)))\n\t\t\t\t\t\tm_ip = m_prev_ip;",
        "\t\t\t\t\tif (0)\n\t\t\t\t\t\tm_ip = m_prev_ip;", ["rep_counts", "rep_iteration"]),
    "NMI on an edge": (
        GEN, "\t\tif (!m_nmi_state && state && total_cycles())", "\t\tif (state && total_cycles())", ["nmi_edge"]),
    "reset drops a pending NMI": (
        GEN, "\tm_pending_irq &= INT_IRQ;", "", ["reset"]),
    # the clock counts from Intel's manual and forward progress (Astra, Reply 104): each undone alone, the number put
    # back to upstream's
    "INT n 51": (GEN, "\t\t52,51, 4,53, /* INTs */", "\t\t52, 0, 4,53, /* INTs */",
                 ["forward_progress", "int_costs", "int_iret", "odd_word", "step_cost_floor"]),
    "INT 3 52": (GEN, "\t\t52,51, 4,53, /* INTs */", "\t\t 2,51, 4,53, /* INTs */", ["int_costs"]),
    "INTO taken 53": (GEN, "\t\t52,51, 4,53, /* INTs */", "\t\t52,51, 4, 2, /* INTs */", ["int_costs"]),
    "IRET 24": (GEN, "\t51,24,          /* exception, IRET */", "\t51,32,          /* exception, IRET */",
                ["int_iret", "odd_word", "rep_iteration", "wait_interrupt", "wait_prefixed"]),
    "IRET charges no POPF": (GEN, "\t\t\tm_icount += m_timing[POPF];   // CHANGED", "\t\t\t//",
                             ["int_iret", "odd_word", "rep_iteration", "wait_interrupt", "wait_prefixed"]),
    "NOP 3": (GEN, "\t\t2,16, 2, 3, 3,11,   /* misc */", "\t\t2,16, 2, 2, 3,11,   /* misc */",
              ["accept_costs", "rep_counts", "tstates", "wait", "wait_interrupt"]),
    "LOCK 2": (GEN, "\t\t\tCLK(OVERRIDE);   // CHANGED: LOCK", "\t\t\tCLK(NOP);   // (undone) LOCK", ["tstates"]),
    "ESC 2 or 8 + EA": (GEN, "\t\t\t\tm_icount -= (m_modrm < 0xc0) ? 8 : 2;   // CHANGED: ESC",
                        "\t\t\t\tCLK(NOP);   // (undone) ESC", ["tstates"]),
    "REP CMPS 22": (GEN, "\t22, 9,22,       /* CMPS 8-bit */", "\t22, 9,21,       /* CMPS 8-bit */", ["rep_counts"]),
    "REP SCAS 15": (GEN, "\t15, 9,15,       /* SCAS 8-bit */", "\t15, 9,14,       /* SCAS 8-bit */", ["rep_counts"]),
    "REP LODS 13": (GEN, "\t12, 9,13,       /* LODS 8-bit */", "\t12, 9,11,       /* LODS 8-bit */", ["rep_counts"]),
    "a REP before a non-string instruction 2": (
        GEN, "\t\t\t\t\tCLK(OVERRIDE);   // CHANGED: the REP prefix's own 2 T", "\t\t\t\t\t//",
        ["forward_progress", "rep_counts", "step_cost_floor"], 2),
    "the divide error's entry": (GEN, "\tif (m_in_instruction && trap)   // CHANGED", "\tif (0)   // (undone) CHANGED",
                                 ["divide_error", "forward_progress", "step_cost_floor"]),
    "the REP's 9, once": (HPP, "            m_icount -= m_timing[base];", "            m_icount -= 2;   // upstream's",
                          ["rep_counts", "rep_iteration"]),
    "a continuing pass pays no prefixes": (
        HPP, "            m_icount = 0;                 // this step's re-fetched prefixes",
        "            ;   // (undone)", ["rep_iteration"]),
    "a repetition's count": (HPP, "        m_icount += m_timing[one];", "        //",
                             ["rep_counts", "rep_iteration"]),
    "the continuation recognised": (DRV, "    if (m_rep_ran && m_ip == m_prev_ip) {", "    if (0) {",
                                    ["rep_counts", "rep_iteration"]),
    "INTR 61": (DRV, "            m_icount -= I86_INTR_T;\n", "",
                ["accept_costs", "intr_vector", "rep_iteration", "wait_interrupt", "wait_prefixed"]),
    "NMI 50": (DRV, "            m_icount -= I86_NMI_T;\n", "", ["accept_costs"]),
    "the trap 50": (DRV, "            m_icount -= I86_TRAP_T;\n", "", ["accept_costs"]),
    "odd word 4": (HPP, "if (addr & 1) m_icount -= I86_ODD_WORD_T;", "(void)addr;",
                   ["odd_word", "port_counts", "return_counts", "stack_counts", "test_imm_counts",
                    "word_memory_counts"]),
    # the 8086's own rows (Astra, Reply 106): each number put back to upstream's 8088-flavoured one alone
    "PUSH r16 11": (GEN, "\t11,16,10,10,    /* pushes */", "\t15,16,10,10,    /* pushes */", ["stack_counts"]),
    "PUSH mem 16": (GEN, "\t11,16,10,10,    /* pushes */", "\t11,24,10,10,    /* pushes */", ["stack_counts"]),
    "PUSH sreg 10": (GEN, "\t11,16,10,10,    /* pushes */", "\t11,16,14,10,    /* pushes */", ["stack_counts"]),
    "PUSHF 10": (GEN, "\t11,16,10,10,    /* pushes */", "\t11,16,10,14,    /* pushes */", ["stack_counts"]),
    "POP r16 8": (GEN, "\t 8,17, 8, 8,    /* pops */", "\t12,17, 8, 8,    /* pops */", ["stack_counts"]),
    "POP mem 17": (GEN, "\t 8,17, 8, 8,    /* pops */", "\t 8,25, 8, 8,    /* pops */", ["stack_counts"]),
    "POP sreg 8": (GEN, "\t 8,17, 8, 8,    /* pops */", "\t 8,17,12, 8,    /* pops */", ["stack_counts"]),
    "POPF 8": (GEN, "\t 8,17, 8, 8,    /* pops */", "\t 8,17, 8,12,    /* pops */", ["stack_counts"]),
    "IN AX,imm8 10": (GEN, "\t10,10, 8, 8,    /* port reads */", "\t10,14, 8, 8,    /* port reads */",
                      ["port_counts"]),
    "IN AX,DX 8": (GEN, "\t10,10, 8, 8,    /* port reads */", "\t10,10, 8,12,    /* port reads */",
                   ["port_counts"]),
    "OUT imm8,AX 10": (GEN, "\t10,10, 8, 8,    /* port writes */", "\t10,14, 8, 8,    /* port writes */",
                       ["port_counts"]),
    "OUT DX,AX 8": (GEN, "\t10,10, 8, 8,    /* port writes */", "\t10,10, 8,12,    /* port writes */",
                    ["odd_word", "port_counts"]),
    "RET 8": (GEN, "\t 8,18,12,17,    /* returns */", "\t20,18,12,17,    /* returns */", ["return_counts"]),
    "RETF 18": (GEN, "\t 8,18,12,17,    /* returns */", "\t 8,32,12,17,    /* returns */", ["return_counts"]),
    "RET n 12": (GEN, "\t 8,18,12,17,    /* returns */", "\t 8,18,24,17,    /* returns */", ["return_counts"]),
    "RETF n 17": (GEN, "\t 8,18,12,17,    /* returns */", "\t 8,18,12,31,    /* returns */", ["return_counts"]),
    "LDS/LES 16": (GEN, "\t\t2,16, 2, 3, 3,11,   /* misc */", "\t\t2,24, 2, 3, 3,11,   /* misc */",
                   ["word_memory_counts"]),
    "MUL m16 124": (GEN, "\t70,118,76,124,  /* MUL */", "\t70,118,76,128,  /* MUL */", ["word_memory_counts"]),
    "IMUL m16 134": (GEN, "\t80,128,86,134,  /* IMUL */", "\t80,128,86,138,  /* IMUL */", ["word_memory_counts"]),
    "DIV m16 150": (GEN, "\t80,144,86,150,  /* DIV */", "\t80,144,86,154,  /* DIV */", ["word_memory_counts"]),
    "IDIV m16 171": (GEN, "\t101,165,107,171,/* IDIV */", "\t101,165,107,175,/* IDIV */", ["word_memory_counts"]),
    # Astra's Reply 110: three more rows, each substitution undone alone (upstream's charge back), and the rows they
    # must NOT move -- TEST AL/AX,imm (A8/A9) keeps its own 4, so a blanket 4 -> 5 fails
    "LOOPNE taken 19": (GEN, "\t\t\t\t\tm_icount -= 19;   // CHANGED: LOOPNE taken 19",
                        "\t\t\t\t\tCLK(LOOP_T);   // (undone) LOOPNE taken 19", ["loopne_counts"]),
    "TEST r8,imm 5": (GEN, "\t\t\t\t\t\tm_icount -= 5;   // CHANGED: TEST r8,imm 5",
                      "\t\t\t\t\t\tCLK(ALU_RI8);   // (undone) TEST r8,imm 5", ["test_imm_counts"]),
    "TEST r16,imm 5": (GEN, "\t\t\t\t\t\tm_icount -= 5;   // CHANGED: TEST r16,imm 5",
                       "\t\t\t\t\t\tCLK(ALU_RI16);   // (undone) TEST r16,imm 5", ["test_imm_counts"]),
    "TEST m8,imm 11 + EA": (GEN, "\t\t\t\t\t\tm_icount -= 11;   // CHANGED: TEST m8,imm 11 + EA",
                            "\t\t\t\t\t\tCLK(ALU_MI8_RO);   // (undone) TEST m8,imm 11 + EA", ["test_imm_counts"]),
    "TEST m16,imm 11 + EA": (GEN, "\t\t\t\t\t\tm_icount -= 11;   // CHANGED: TEST m16,imm 11 + EA",
                             "\t\t\t\t\t\tCLK(ALU_MI16_RO);   // (undone) TEST m16,imm 11 + EA", ["test_imm_counts"]),
    "TEST AL,imm (A8) keeps its own 4": (GEN, "\t\t\tDEF_ald8();\n\t\t\tANDB();\n\t\t\tCLK(ALU_RI8);",
                                         "\t\t\tDEF_ald8();\n\t\t\tANDB();\n\t\t\tm_icount -= 5;   // (a blanket 5)",
                                         ["test_imm_counts"]),
    "TEST AX,imm (A9) keeps its own 4": (GEN, "\t\t\tDEF_axd16();\n\t\t\tANDX();\n\t\t\tCLK(ALU_RI16);",
                                         "\t\t\tDEF_axd16();\n\t\t\tANDX();\n\t\t\tm_icount -= 5;   // (a blanket 5)",
                                         ["test_imm_counts"]),
    # IMUL's CF and OF (Reply 110): each width's substitution undone alone, upstream's MUL-style test back
    "IMUL r/m8's CF and OF": (GEN, "m_CarryVal = m_OverVal = (result < -128 || result > 127) ? 1 : 0;   // CHANGED",
                              "m_CarryVal = m_OverVal = (m_regs.b[AH]!=0) ? 1 : 0;   // (undone) CHANGED",
                              ["imul_flags_byte"]),
    "IMUL r/m16's CF and OF": (GEN,
                               "m_CarryVal = m_OverVal = (result < -32768 || result > 32767) ? 1 : 0;   // CHANGED",
                               "m_CarryVal = m_OverVal = (m_regs.w[DX] != 0) ? 1 : 0;   // (undone) CHANGED",
                               ["imul_flags_word"]),
    # the prefixed WAIT's reproducer (Reply 110) sees where a waiting WAIT restarts: a variant that puts IP back on
    # the first prefix instead (NOT a proposed fix -- the 8086's own retention is not established) fails it alone
    "(reproducer) a prefixed WAIT restarts at its WAIT byte": (
        HPP, "        m_ip--;                           // upstream's: IP back on the WAIT",
        "        m_ip = m_prev_ip;                 // (variant) back on the first prefix", ["wait_prefixed"]),
}

CXX = ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti"]


def build(src_dir, out_exe, env, bindir):
    inc = ["-I" + src_dir]
    t_o, c_o = os.path.join(src_dir, "t.o"), os.path.join(src_dir, "c.o")
    subprocess.run([os.path.join(bindir, "gcc"), "-O2", "-std=gnu89"] + inc
                   + ["-c", os.path.join(src_dir, "test_i86_contract.c"), "-o", t_o], env=env, check=True)
    subprocess.run([os.path.join(bindir, "g++")] + CXX + inc + ["-c", os.path.join(src_dir, DRV), "-o", c_o],
                   env=env, check=True)
    subprocess.run([os.path.join(bindir, "g++"), "-o", out_exe, t_o, c_o], env=env, check=True)


def main():
    bindir = repo_paths.bin_dir("W64DEVKIT") if os.name == "nt" else "/usr/bin"
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "base")
        shutil.copytree(HERE, base)
        exe = os.path.join(base, "tc")
        build(base, exe, env, bindir)
        try:
            results, f = run_tests(exe, suffixes=("",))
        except RunError as e:
            print("BAD  the core as it is: %s" % e)
            sys.exit(1)
        inventory = sorted(results)
        print("%-4s the core as it is -> %d tests, failing: %s" % ("ok" if not f else "BAD", len(inventory),
                                                                   ", ".join(f) or "none"))
        bad += bool(f)

        def one(k, rule, spec):
            """Build and run one variant; returns (as expected, its report line)."""
            fname, old, new, tests = spec[:4]
            n = spec[4] if len(spec) > 4 else 1        # the rule's code, where it occurs more than once
            d = os.path.join(tmp, "v%d" % k)
            shutil.copytree(HERE, d)
            p = os.path.join(d, fname)
            t = open(p, encoding="utf-8").read()
            assert t.count(old) == n, "the rule's code moved: %s" % rule
            open(p, "w", encoding="utf-8").write(t.replace(old, new))
            exe = os.path.join(d, "tc")
            try:
                build(d, exe, env, bindir)
            except subprocess.CalledProcessError as e:
                return False, "BAD  undone: %-42s -> the variant does not build: %s" % (rule, e)
            try:
                _results, f = run_tests(exe, inventory, suffixes=("",))
            except RunError as e:
                return False, "BAD  undone: %-42s -> the run itself failed: %s" % (rule, e)
            ok = f == sorted(tests)
            return ok, "%-4s undone: %-42s -> failing: %s" % ("ok" if ok else "BAD", rule, ", ".join(f) or "none")

        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            jobs = [pool.submit(one, k, rule, spec) for k, (rule, spec) in enumerate(VARIANTS.items())]
            for job in jobs:                           # in VARIANTS' order
                ok, line = job.result()
                bad += not ok
                print(line)
    print("controls: %s" % ("all as expected" if not bad else "%d NOT as expected" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
