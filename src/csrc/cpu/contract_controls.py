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

# test name -> (the rule's code in z180_mame.cpp, the same code with the rule undone)
VARIANTS = {
    "burst_dma": ("    if (!burst && !sleeping && (d.m_dstat & Z180_DSTAT_DME) && !d.drv_burst()) {",
                  "    if (!burst && !sleeping && (d.m_dstat & Z180_DSTAT_DME)) {"),
    "slp_wake": ("    if (t == 0 && m_HALT == 2 && !m_IFF1 && drv_sleep_wake_request())",
                 "    if (0)"),
    "iostop": ("    if (!(c->dev->m_iocr & Z180_IOCR_IOSTP))\n        c->dev->handle_io_timers(t);",
               "    c->dev->handle_io_timers(t);"),
    "nmi_per_step": ("    if (m_nmi_pending) {                  // execute_run's NMI entry, as upstream",
                     "    if (m_nmi_pending && 0) {"),
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


def failing(exe):
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    return [ln.split()[1] for ln in out.splitlines() if ln.startswith("FAIL ")]


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
        for test, (old, new) in VARIANTS.items():
            d = os.path.join(tmp, test)
            shutil.copytree(HERE, d)
            p = os.path.join(d, "z180_mame.cpp")
            t = open(p, encoding="utf-8").read()
            assert t.count(old) == 1, "the rule's code moved: %s" % test
            open(p, "w", encoding="utf-8").write(t.replace(old, new))
            exe = os.path.join(d, "tc")
            build(d, exe, env, bindir)
            f = failing(exe)
            ok = f == [test]
            bad += not ok
            print("%-4s rule undone for %-13s -> failing: %s" % ("ok" if ok else "BAD", test, ", ".join(f) or "none"))
    print("controls: %s" % ("all as expected" if not bad else "%d NOT as expected" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
