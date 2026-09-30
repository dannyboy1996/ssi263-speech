"""Must-fail controls for test_v40_contract.c: each variant undoes one rule of the MAME V40 core -- our step driver
(v40_mame.cpp), the shim (v40_mame.hpp), a named change of the extraction or an upstream rule the contract states
(v40_mame_machine.cpp) -- in a temporary copy, and exactly that rule's tests must then fail.  Proves the tests see
what they claim to (the repo's rule for any fix claim).  The runner guard (exit code, summary line, the full test
inventory) is contract_controls.py's.  Not in run_tests: 29 C++ builds take a few minutes.

    python src/csrc/cpu/v40_controls.py

No control is possible for the statics made members (Mod_RM, parity_table, nec_popa_tmp): the tables are the same in
every instance, so two cores agree with them shared too; two_cores checks the instances stay apart.  Nor for reset
clearing EI's delay (drv_ei_shadow): reset clears IE, and the delay is spent by the first instruction after it.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)
from tools import repo_paths  # noqa: E402
from contract_controls import RunError, run_tests  # noqa: E402

DRV, SHIM, GEN = "v40_mame.cpp", "v40_mame.hpp", "v40_mame_machine.cpp"
REPS = ["rep_steps", "rep_irq", "rep_seg_irq", "rep_seg_steps"]

# a rule -> (its file, its code, the same code with the rule undone, the tests that must then fail -- and no others)
VARIANTS = {
    # the step driver
    "one REP iteration per step": (
        DRV, "static const int V40_STEP_ICOUNT = 1;", "static const int V40_STEP_ICOUNT = 100000;", REPS),
    "a continued REP not charged again": (
        DRV, "(cont ? V40_REP_PREFIX : 0)", "0", ["rep_steps"]),
    "HALT slot 2 clocks": (
        DRV, "static const int V40_HALT_SLOT = 2;", "static const int V40_HALT_SLOT = 3;",
        ["halt_int", "mode8080_fault"]),
    "an acceptance ends HALT": (
        DRV, "            m_halted = 0;                    // an acceptance ends HALT\n", "",
        ["ei_halt", "halt_int", "halt_nmi"]),
    "no acceptance in a shadow": (
        DRV, "    if (m_pending_irq && !m_no_interrupt) {", "    if (m_pending_irq) {",
        ["pop_sreg_shadow", "poll_shadow", "sreg_from_shadow", "sreg_shadow"]),
    "the shadow runs down at C": (
        DRV, "    if (d.m_no_interrupt)                    // C\n        d.m_no_interrupt--;\n", "",
        ["pop_sreg_shadow", "poll_shadow", "prefix_atomic", "rep_seg_irq", "sreg_from_shadow", "sreg_shadow"]),
    # NEC's rules (after Astra, Reply 103: V40 data book 1990 p.34; instruction manual pp.80, 98, 118)
    "EI's delay lets NMI through (driver)": (
        DRV, "        if (nmi || (m_IF && !drv_ei_shadow)) {", "        if ((nmi && !drv_ei_shadow) || (m_IF && !drv_ei_shadow)) {",
        ["ei_nmi"]),
    "a masked INT releases HALT": (
        DRV, "    if (m_halted && (m_pending_irq & INT_IRQ))\n        m_halted = 0;", "",
        ["halt_masked_wake"]),
    "the release keeps the INT pending": (
        DRV, "        m_halted = 0;                        // a masked INT releases HALT:",
        "        { m_halted = 0; m_pending_irq &= ~INT_IRQ; }   // dropped:", ["halt_masked_wake"]),
    "the release is no acceptance": (
        DRV, "        if (nmi || (m_IF && !drv_ei_shadow)) {", "        if (nmi || m_halted || (m_IF && !drv_ei_shadow)) {",
        ["halt_masked_wake"]),
    "acceptance PC": (
        DRV, "    c->pc = linear(d, d.m_ip);               // A's callbacks see the interrupted address\n", "",
        ["halt_int", "int_accept"]),
    "reset keeps a held INT": (
        DRV, "    if (int_held != CLEAR_LINE)\n        d.set_int_line(ASSERT_LINE);\n", "", ["reset"]),
    "undefined opcodes counted": (
        DRV, '"ndefined", "nvalid", "nknown", "nimplemented mod",', '"-",', ["undefined"]),
    "the FPO escape is not undefined": (
        DRV, '"nimplemented mod",', '"nimplemented",', ["undefined"]),
    "the 8080 mode faults": (
        DRV, "    if (drv_fault || !m_MF) {", "    if (drv_fault) {", ["mode8080_fault"]),
    # the shim
    "20 address lines": (
        SHIM, "#define V40_ADDR_MASK 0xfffffu", "#define V40_ADDR_MASK 0xffffffu", ["wrap20"]),
    "a word's low byte first": (
        SHIM, "        write_byte(a, (u8)v);\n        write_byte(a + 1, (u8)(v >> 8));",
        "        write_byte(a + 1, (u8)(v >> 8));\n        write_byte(a, (u8)v);", ["word_order"]),
    # the extraction's named changes
    "INT releases HALT at A, not at the line": (
        GEN, "\t\tm_pending_irq |= INT_IRQ;\n", "\t\tm_pending_irq |= INT_IRQ;\n\t\tm_halted = 0;\n",
        ["halt_int", "halt_masked_wake"]),   # upstream wakes at the line: the instruction after HLT runs first
    "NMI ends HALT at its acceptance": (
        GEN, "\t\tm_pending_irq |= NMI_IRQ;\n", "\t\tm_pending_irq |= NMI_IRQ;\n\t\tm_halted = 0;\n", ["halt_nmi"]),
    "EI delays INT": (
        GEN, "CLK(2); drv_ei_shadow=1; }", "CLK(2); }", ["ei_halt", "ei_shadow", "halt_masked_wake", "reset"]),
    "EI's delay is its own, not m_no_interrupt": (   # Astra: a shadow that holds NMI off too is wrong for EI
        GEN, "CLK(2); drv_ei_shadow=1; }", "CLK(2); m_no_interrupt=1; }", ["ei_nmi"]),
    "a move FROM a segment register defers": (
        GEN, "\tm_no_interrupt=1;   // CHANGED: a move FROM", "\t// a move FROM", ["sreg_from_shadow"]),
    "POP DS1 defers": (
        GEN, "CLKS(12,8,5);   m_no_interrupt=1; }   // CHANGED: as POP SS", "CLKS(12,8,5);   }   // as POP SS",
        ["pop_sreg_shadow"]),
    "POP DS0 defers": (
        GEN, "CLKS(12,8,5);   m_no_interrupt=1; }   // CHANGED: as POP DS1", "CLKS(12,8,5);   }   // as POP DS1",
        ["pop_sreg_shadow"]),
    "a completed POLL defers": (
        GEN, "m_ip--; else m_no_interrupt=1; CLK(5); }", "m_ip--; CLK(5); }", ["poll_shadow"]),
    # upstream rules the contract states
    "reset drops a pending NMI": (
        GEN, "\tm_pending_irq = 0;\n", "", ["reset"]),
    "POP SS shadow": (
        GEN, "POP(Sreg(SS));     CLKS(12,8,5);   m_no_interrupt=1; }", "POP(Sreg(SS));     CLKS(12,8,5); }",
        ["sreg_shadow"]),
    "the acceptance consumes INT": (
        GEN, "\t\tm_irq_state = CLEAR_LINE;\n\t\tm_pending_irq &= ~INT_IRQ;\n", "\t\tm_irq_state = CLEAR_LINE;\n",
        ["ei_halt", "ei_shadow", "halt_int", "halt_masked_wake", "int_accept", "poll_shadow", "pop_sreg_shadow",
         "rep_irq", "rep_seg_irq", "reset", "sreg_from_shadow", "sreg_shadow"]),   # a held line taken again
}

CXX = ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wno-sign-compare"]


def build(src_dir, out_exe, env, bindir):
    inc = ["-I" + src_dir]
    t_o, c_o = os.path.join(src_dir, "t.o"), os.path.join(src_dir, "c.o")
    subprocess.run([os.path.join(bindir, "gcc"), "-O2", "-std=gnu89"] + inc
                   + ["-c", os.path.join(src_dir, "test_v40_contract.c"), "-o", t_o], env=env, check=True)
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
        for k, (rule, (fname, old, new, tests)) in enumerate(VARIANTS.items()):
            d = os.path.join(tmp, "v%d" % k)
            shutil.copytree(HERE, d)
            p = os.path.join(d, fname)
            t = open(p, encoding="utf-8").read()
            assert t.count(old) == 1, "the rule's code moved: %s" % rule
            open(p, "w", encoding="utf-8", newline="\n").write(t.replace(old, new))
            exe = os.path.join(d, "tc")
            build(d, exe, env, bindir)
            try:
                _results, f = run_tests(exe, inventory, suffixes=("",))
            except RunError as e:
                bad += 1
                print("BAD  undone: %-34s -> the run itself failed: %s" % (rule, e))
                continue
            ok = f == sorted(tests)
            bad += not ok
            print("%-4s undone: %-34s -> failing: %s" % ("ok" if ok else "BAD", rule, ", ".join(f) or "none"))
    print("controls: %s" % ("all as expected" if not bad else "%d NOT as expected" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
