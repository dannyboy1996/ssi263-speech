"""Must-fail controls for test_i86_contract.c: each variant undoes one rule of the MAME 8086 core -- our step driver
(i86_mame.cpp), a named change of the extraction, or an upstream rule the contract relies on (i86_mame_machine.cpp)
-- in a temporary copy, and exactly that rule's tests must then fail.  Proves the tests see what they claim to (the
repo's rule for any fix claim).  The runner guard (exit code, summary line, the full test inventory) is
contract_controls.py's.  Not in run_tests: some forty C++ builds (in parallel) take a minute or two.

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
        DRV, "        if (!m_seg_prefix_next)\n            break;", "        break;", ["prefix_atomic", "rep_iteration"]),
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
    "WAIT's slot": (
        GEN, "\t\t\t\tCLK(WAIT);   // CHANGED", "\t\t\t\tm_icount = 0;   // (undone)", ["wait"]),
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
                ["int_iret", "odd_word", "rep_iteration"]),
    "IRET charges no POPF": (GEN, "\t\t\tm_icount += m_timing[POPF];   // CHANGED", "\t\t\t//",
                             ["int_iret", "odd_word", "rep_iteration"]),
    "NOP 3": (GEN, "\t\t2,24, 2, 3, 3,11,   /* misc */", "\t\t2,24, 2, 2, 3,11,   /* misc */",
              ["accept_costs", "rep_counts", "tstates"]),
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
    "INTR 61": (DRV, "            m_icount -= I86_INTR_T;\n", "", ["accept_costs", "intr_vector", "rep_iteration"]),
    "NMI 50": (DRV, "            m_icount -= I86_NMI_T;\n", "", ["accept_costs"]),
    "the trap 50": (DRV, "            m_icount -= I86_TRAP_T;\n", "", ["accept_costs"]),
    "odd word 4": (HPP, "if (addr & 1) m_icount -= I86_ODD_WORD_T;", "(void)addr;", ["odd_word"]),
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
