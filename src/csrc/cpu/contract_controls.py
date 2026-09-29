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
        ["im0_nop", "im0_rst", "im0_call"]),
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
        ["trap_dd_nop", "trap_ixh", "trap_fd_exdehl"]),
    "TRAP: DDCB/FDCB rule": (
        "z180_mame.cpp", "        if (!z180_trap::xycb_defined(b4))", "        if (0)", ["trap_ddcb_reg", "trap_ddcb_sll"]),
    "TRAP: UFO's stacked PC": (
        "z180_mame.cpp", "    _PCD = (_PCD - (ufo ? 2 : 1)) & 0xffff;", "    _PCD = (_PCD - 1) & 0xffff;",
        ["trap_ddcb_reg", "trap_ddcb_sll"]),
    "TRAP: ITC cannot be set": (
        "z180_mame_machine.cpp", "(m_itc & data & Z180_ITC_TRAP)", "(data & Z180_ITC_TRAP)",
        ["trap_dd_nop", "trap_ixh", "trap_fd_exdehl", "trap_ed_dup", "trap_ed_in_c", "trap_cb_sll", "trap_ddcb_reg",
         "trap_ddcb_sll"]),
    # MAME's e0deaf3898b, each hunk reverted in the generated machine file (Astra, Reply 92)
    "e0deaf: timer from RLDR": (
        "z180_mame_machine.cpp", "Z180_TCR_TDE0))\n\t\t\t\tm_tmdr_value[0] = m_rldr[0].w;",
        "Z180_TCR_TDE0))\n\t\t\t\tm_tmdr_value[0] = 0;", ["timer_start", "iostop"]),
    "e0deaf: TMDR1H shift": (
        "z180_mame_machine.cpp", "m_tmdr_value[1] = (m_tmdr_value[1] & 0x00ff) | (m_tmdr[1].b.h << 8);",
        "m_tmdr_value[1] = (m_tmdr_value[1] & 0x00ff) | m_tmdr[1].b.h;", ["tmdr1h"]),
    "e0deaf: DMA0 IRQ kept": (
        "z180_mame_machine.cpp", "\t\tif (m_dstat & Z180_DSTAT_DIE0)\n", "\t\tif (m_dstat & Z180_DSTAT_DIE0 && m_IFF1)\n",
        ["dma_done_di"]),
    "e0deaf: DMA1 edge = DMS1": (
        "z180_mame_machine.cpp", "\tif (m_dcntl & Z180_DCNTL_DMS1)\n", "\tif (m_dcntl & Z180_DCNTL_DIM1)\n",
        ["dma1_level"]),
    "e0deaf: internal IRQ gating": (
        "z180_mame_machine.cpp",
        "\t\tm_int_pending[Z180_INT_ASCI1] = m_asci[1]->check_interrupt();\n\n\t\tfor (int i = 0;",
        "\t\tm_int_pending[Z180_INT_ASCI1] = m_asci[1]->check_interrupt();\n\t}\n\t{\n\t\tfor (int i = 0;",
        ["dma_done_di"]),
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


def failing(exe):
    f = []
    for e in (exe, exe + "_wb"):
        out = subprocess.run([e], capture_output=True, text=True).stdout
        f += [ln.split()[1] for ln in out.splitlines() if ln.startswith("FAIL ")]
    return f


def main():
    bindir = repo_paths.bin_dir("W64DEVKIT") if os.name == "nt" else "/usr/bin"
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "base")
        shutil.copytree(HERE, base)
        exe = os.path.join(base, "tc")
        build(base, exe, env, bindir)
        f = failing(exe)
        print("%-4s the core as it is -> failing: %s" % ("ok" if not f else "BAD", ", ".join(f) or "none"))
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
            f = failing(exe)
            ok = sorted(f) == sorted(tests)
            bad += not ok
            print("%-4s undone: %-26s -> failing: %s" % ("ok" if ok else "BAD", rule, ", ".join(f) or "none"))
    print("controls: %s" % ("all as expected" if not bad else "%d NOT as expected" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
