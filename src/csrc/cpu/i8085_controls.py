"""Must-fail controls for test_i8085_contract.c: each variant undoes one rule of the MAME 8085 core -- our step driver
(i8085_mame.cpp), a named change of the extraction, or an upstream rule the contract relies on
(i8085_mame_machine.cpp) -- in a temporary copy, and exactly that rule's tests must then fail.  Proves the tests see
what they claim to (the repo's rule for any fix claim).  The runner guard (exit code, summary line, the full test
inventory) is contract_controls.py's.  Not in run_tests: a dozen C++ builds take a minute or so.

    python src/csrc/cpu/i8085_controls.py
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

DRV, GEN = "i8085_mame.cpp", "i8085_mame_machine.cpp"

# a rule -> (its file, its code, the same code with the rule undone, the tests that must then fail -- and no others)
VARIANTS = {
    # the step driver
    "TRAP not delayed by EI": (
        DRV, "    if (m_trap_pending || m_after_ei == 0)", "    if (m_after_ei == 0)", ["trap_ei"]),
    "EI shadow": (
        DRV, "    if (m_trap_pending || m_after_ei == 0)", "    if (1)", ["ei_shadow", "reset", "rst_levels"]),
    "EI shadow ends at C": (
        DRV, "    d.m_after_ei = 0;                     // C\n", "",
        ["reset", "ei_shadow", "halt_edge", "accept_pc", "priority", "sim_reveal", "rst_levels", "intr_rst",
         "intr_call", "intr_nop", "intr_jcc", "intr_twice", "intr_halt"]),
    "HALT slot 4 T": (
        DRV, "        return 4;                         // a HALT slot (CONTRACT.md 6)", "        return 5;", ["halt_edge"]),
    "acceptance PC": (
        DRV, "    c->pc = (uint32_t)((d.m_PC.w.l + (d.m_halt ? 1 : 0)) & 0xffff);\n", "", ["accept_pc"]),
    "acknowledge index from 0": (
        DRV, "    m_in_inta_func.n = 0;\n", "", ["intr_twice"]),
    # the extraction's named changes
    "acceptance 12 T (Intel)": (
        GEN, "constexpr int I8085_ACCEPT_T = 12;", "constexpr int I8085_ACCEPT_T = 11;",
        ["trap_ei", "halt_edge", "sim_reveal"]),
    "INTR injected at E": (
        GEN, "\t\tset_inte(0);\n\t\tm_inject_pending = true;",
        "\t\tset_inte(0);\n\t\tm_in_inta_func.n = 0;\n\t\texecute_one(read_inta());",
        ["intr_rst", "intr_call", "intr_nop", "intr_jcc", "intr_halt"]),
    "no acceptance inside SIM": (
        GEN, "at the end of each instruction\n", "at the end of each instruction\n\t\t\t\tcheck_for_interrupts();\n",
        ["sim_reveal"]),
    "branch not taken in an acknowledge": (
        GEN, "\t\tm_icount -= jmp_taken();\n\t}\n\telse\n\t{\n\t\tif (!m_in_acknowledge)",
        "\t\tm_icount -= jmp_taken();\n\t}\n\telse\n\t{\n\t\tif (1)", ["intr_jcc"]),
    # upstream rules the contract states
    "TRAP: the line dropping cancels": (
        GEN, "\t\telse if (!newstate)\n\t\t\tm_trap_pending = false;\n", "", ["trap_cancel"]),
    "RST 7.5 latched while masked": (
        GEN, "\t\tif (!m_irq_state[I8085_RST75_LINE] && newstate)",
        "\t\tif (!m_irq_state[I8085_RST75_LINE] && newstate && !(m_im & IM_M75))",
        ["ei_shadow", "priority", "rst75_latch", "sim_reveal"]),   # the first two latch 7.5 while masked
    "RIM after TRAP": (
        GEN, "\t\t\t\tif (m_trap_im_copy & 0x80)", "\t\t\t\tif (0)", ["rim_after_trap"]),
    "reset drops a pending TRAP": (
        GEN, "\tm_trap_pending = false;\n\tm_trap_im_copy = 0;\n\tset_inte(0);", "\tm_trap_im_copy = 0;\n\tset_inte(0);",
        ["reset"]),
}

CXX = ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti"]


def build(src_dir, out_exe, env, bindir):
    inc = ["-I" + src_dir]
    t_o, c_o = os.path.join(src_dir, "t.o"), os.path.join(src_dir, "c.o")
    subprocess.run([os.path.join(bindir, "gcc"), "-O2", "-std=gnu89"] + inc
                   + ["-c", os.path.join(src_dir, "test_i8085_contract.c"), "-o", t_o], env=env, check=True)
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
            open(p, "w", encoding="utf-8").write(t.replace(old, new))
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
