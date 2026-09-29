"""Must-fail controls for test_z180_contract.c: each variant undoes one rule of the MAME core's step driver
(z180_mame.cpp) in a temporary copy, and exactly that rule's test must then fail.  Proves the tests see what they
claim to (the repo's rule for any fix claim).  Not in run_tests: four C++ builds take about a minute.

    python src/csrc/cpu/contract_controls.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

# a rule -> (its file, its code, the same code with the rule undone, the tests that must then fail -- and no others)
VARIANTS = {
    "burst chunk before G": (
        "z180_mame.cpp",
        "    if (!burst && d.m_HALT != 2 && (d.m_dstat & Z180_DSTAT_DME) && !d.drv_burst()) {",
        "    if (!burst && d.m_HALT != 2 && (d.m_dstat & Z180_DSTAT_DME)) {", ["burst_dma"]),
    "SLP wake without service": (
        "z180_mame.cpp", "    if (t == 0 && m_HALT == 2 && !m_IFF1 && drv_sleep_wake_request())", "    if (0)",
        ["slp_wake"]),
    "IOSTOP": (
        "z180_mame.cpp", "    if (!(c->dev->m_iocr & Z180_IOCR_IOSTP))\n        c->dev->handle_io_timers(t);",
        "    c->dev->handle_io_timers(t);", ["iostop"]),
    "NMI at every step": (
        "z180_mame.cpp", "    if (m_nmi_pending) {                  // execute_run's NMI entry, as upstream",
        "    if (m_nmi_pending && 0) {", ["nmi_per_step", "nmi_stops_dma", "accept_pc"]),
    "IM0 injected": (
        "z180_mame.cpp", "    if (m_IM == 0 && m_IFF1 && !m_after_EI", "    if (0 && m_IM == 0 && m_IFF1 && !m_after_EI",
        ["im0_nop", "im0_rst", "im0_call", "im0_rst_nowait", "im0_nop_nowait", "im0_prefixed",
         "im0_undefined"]),
    "no DMA in sleep": (
        "z180_mame.cpp", "    if (!burst && d.m_HALT != 2 && (d.m_dstat",
        "    if (!burst && (d.m_dstat", ["sleep_dma"]),
    "acceptance PC": (
        "z180_mame.cpp", "    c->pc = (uint32_t)((d.m_PC.w.l + (d.m_HALT == 2 ? 2 : d.m_HALT ? 1 : 0)) & 0xffff);",
        "", ["accept_pc"]),
    "EFR = 0 clears": (
        "z180_asci.cpp", "    if (!(data & CNTLA_EFR))", "    if (data & CNTLA_EFR)", ["asci_efr"]),
    # TRAP (z180_trap.hpp, drv_trap): each classification, the stacked PC and ITC's set-protection
    "TRAP: CB map": (
        "z180_mame.cpp", "        if (!z180_trap::cb_defined(b2))", "        if (0)", ["trap_cb_sll"]),
    "TRAP: ED map": (
        "z180_mame.cpp", "        if (!z180_trap::ed_defined(b2))", "        if (0)", ["trap_ed_dup", "trap_ed_in_c"]),
    "TRAP: DD/FD rule": (
        "z180_mame.cpp", "        if (!z180_trap::xy_defined(b2))", "        if (0)",
        ["trap_dd_nop", "trap_ixh", "trap_fd_exdehl", "trapbus_2nd", "im0_undefined"]),
    "TRAP: DDCB/FDCB rule": (
        "z180_mame.cpp", "        if (!z180_trap::xycb_defined(b4))", "        if (0)", ["trap_ddcb_reg", "trap_ddcb_sll", "trapbus_3rd"]),
    "TRAP: UFO's stacked PC": (
        "z180_mame.cpp", "(uint16_t)((_PCD - (ufo ? 2 : 1)) & 0xffff)", "(uint16_t)((_PCD - 1) & 0xffff)",
        ["trap_ddcb_reg", "trap_ddcb_sll"]),
    "TRAP: ITC cannot be set": (
        "z180_mame_machine.cpp", "(m_itc & data & Z180_ITC_TRAP)", "(data & Z180_ITC_TRAP)",
        ["trap_dd_nop", "trap_ixh", "trap_fd_exdehl", "trap_ed_dup", "trap_ed_in_c", "trap_cb_sll", "trap_ddcb_reg",
         "trap_ddcb_sll"]),
    # after Astra, Replies 93/94
    "PRT requests from TIF/TIE": (
        "z180_mame_machine.cpp",
        "\t\tm_int_pending[Z180_INT_PRT0] = (m_tcr & Z180_TCR_TIE0) && (m_tcr & Z180_TCR_TIF0);\n"
        "\t\tm_int_pending[Z180_INT_PRT1] = (m_tcr & Z180_TCR_TIE1) && (m_tcr & Z180_TCR_TIF1);\n", "",
        ["prt_priority"]),
    "IM0 acknowledge cycle 5 T": (
        "z180_mame.cpp", "    t += 5 - 3;", "    t += 0;",
        ["im0_nop", "im0_rst", "im0_call", "im0_rst_nowait", "im0_nop_nowait"]),
    "IM0 acknowledge bytes no waits": (
        "z180_mame.cpp", "    t -= (m_inject.n - 1) * memory_wait_states();", "", ["im0_call"]),
    "IM0 acknowledge is an M1": (
        "z180_mame.cpp", "    m_R++;                                // the acknowledge cycle is an M1 cycle", "",
        ["im0_rst_nowait", "im0_nop_nowait", "im0_prefixed", "im0_undefined"]),
    "DD/FD R counted once each": (
        "z180_mame.cpp", "            m_R--;\n", "", ["im0_prefixed"]),
    "TRAP: the read at IX+d": (
        "z180_mame.cpp", "            RM(m_ea);", "", ["trapbus_3rd"]),
    "TRAP: Figure 32's 18 T": (
        "z180_mame.cpp", "3 * 2 + 6 + 6", "3 * 2 + 3 + 6", ["trapbus_2nd"]),
    "TRAP: PCH to SP-1 first": (
        "z180_mame.cpp",
        "    WM((_SPD + 1) & 0xffff, (uint8_t)(stacked >> 8));\n    WM(_SPD, (uint8_t)stacked);",
        "    WM(_SPD, (uint8_t)stacked);\n    WM((_SPD + 1) & 0xffff, (uint8_t)(stacked >> 8));",
        ["trapbus_2nd", "trapbus_3rd"]),
    # MAME's e0deaf3898b, each hunk reverted in the generated machine file (Astra, Reply 92)
    "e0deaf: timer from RLDR": (
        "z180_mame_machine.cpp", "Z180_TCR_TDE0))\n\t\t\t\tm_tmdr_value[0] = m_rldr[0].w;",
        "Z180_TCR_TDE0))\n\t\t\t\tm_tmdr_value[0] = 0;", ["timer_start", "iostop"]),
    "e0deaf: TMDR1H shift": (
        "z180_mame_machine.cpp", "m_tmdr_value[1] = (m_tmdr_value[1] & 0x00ff) | (m_tmdr[1].b.h << 8);",
        "m_tmdr_value[1] = (m_tmdr_value[1] & 0x00ff) | m_tmdr[1].b.h;", ["tmdr1h"]),
    "e0deaf: DMA0 IRQ kept": (
        "z180_mame_machine.cpp", "\t\tif (m_dstat & Z180_DSTAT_DIE0)\n", "\t\tif (m_dstat & Z180_DSTAT_DIE0 && m_IFF1)\n",
        ["dma_done_di", "prt_stale"]),
    "e0deaf: DMA1 edge = DMS1": (
        "z180_mame_machine.cpp", "\tif (m_dcntl & Z180_DCNTL_DMS1)\n", "\tif (m_dcntl & Z180_DCNTL_DIM1)\n",
        ["dma1_level"]),
    "e0deaf: internal IRQ gating": (
        "z180_mame_machine.cpp",
        "\t\tm_int_pending[Z180_INT_ASCI1] = m_asci[1]->check_interrupt();\n\n\t\tfor (int i = 0;",
        "\t\tm_int_pending[Z180_INT_ASCI1] = m_asci[1]->check_interrupt();\n\t}\n\t{\n\t\tfor (int i = 0;",
        ["dma_done_di", "prt_priority"]),
}


def build(src_dir, out_exe, env, bindir):
    inc = ["-I" + src_dir, "-I" + os.path.dirname(HERE)]
    objs = []
    for name, cc, flags in (("test_z180_contract.c", "gcc", ["-O2", "-std=gnu89"]),
                            ("z180_mame.cpp", "g++", ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti"]),
                            ("z180_asci.cpp", "g++", ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti"])):
        o = os.path.join(src_dir, name + ".o")
        subprocess.run([os.path.join(bindir, cc)] + flags + inc + ["-c", os.path.join(src_dir, name), "-o", o],
                       env=env, check=True)
        objs.append(o)
    subprocess.run([os.path.join(bindir, "g++"), "-o", out_exe] + objs, env=env, check=True)
    # the white-box tests include the core themselves
    subprocess.run([os.path.join(bindir, "g++"), "-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti"] + inc
                   + ["-o", out_exe + "_wb", os.path.join(src_dir, "test_z180_whitebox.cpp"),
                      os.path.join(src_dir, "z180_asci.cpp")], env=env, check=True)


class RunError(Exception):
    """A test program that did not complete as a test program must: crashed, timed out, printed no summary, lost
    tests from the inventory, or exited with a code that does not match its report."""


def run_tests(exe, inventory=None):
    """Both test programs; returns (every test's result {name: ok}, the failing names).  The guard (after Astra,
    Reply 93, whose child exiting 42 without output passed the old stdout-only check): each program must end with
    its summary line ("all passed" / "FAILED"), exit 0 with "all passed" and 1 with "FAILED", and nothing else;
    and, given the baseline's inventory, report exactly those tests, each once."""
    results = {}
    for e in (exe, exe + "_wb"):
        try:
            p = subprocess.run([e], capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            raise RunError("%s timed out" % os.path.basename(e))
        lines = p.stdout.splitlines()
        summary = lines[-1].strip() if lines else ""
        if summary not in ("all passed", "FAILED"):
            raise RunError("%s: no summary line (exit %d, stderr %r)" % (os.path.basename(e), p.returncode,
                                                                       p.stderr[-200:]))
        if p.returncode != (0 if summary == "all passed" else 1):
            raise RunError("%s: exit %d with summary %r" % (os.path.basename(e), p.returncode, summary))
        mine = {}
        for ln in lines[:-1]:
            parts = ln.split()
            if len(parts) >= 2 and parts[0] in ("ok", "FAIL"):
                if parts[1] in results or parts[1] in mine:
                    raise RunError("%s: test %s reported twice" % (os.path.basename(e), parts[1]))
                mine[parts[1]] = parts[0] == "ok"
        results.update(mine)
        if not mine or (summary == "all passed") != all(mine.values()):
            raise RunError("%s: the summary %r contradicts the tests' lines" % (os.path.basename(e), summary))
    if inventory is not None and set(results) != set(inventory):
        raise RunError("the tests reported differ from the baseline's: missing %s, extra %s" % (
            sorted(set(inventory) - set(results)), sorted(set(results) - set(inventory))))
    return results, sorted(n for n, ok in results.items() if not ok)


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
            results, f = run_tests(exe)
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
                _results, f = run_tests(exe, inventory)
            except RunError as e:
                bad += 1
                print("BAD  undone: %-26s -> the run itself failed: %s" % (rule, e))
                continue
            ok = f == sorted(tests)
            bad += not ok
            print("%-4s undone: %-26s -> failing: %s" % ("ok" if ok else "BAD", rule, ", ".join(f) or "none"))
    print("controls: %s" % ("all as expected" if not bad else "%d NOT as expected" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
