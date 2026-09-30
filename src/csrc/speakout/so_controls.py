"""Must-fail controls for test_so_board.c: each variant undoes one rule of the Speak-Out board (so_icu.c, so_scu.c,
so_board.c) in a temporary copy, and exactly that rule's tests must then fail.  The runner guard (exit code, summary
line, the full test inventory) is ../cpu/contract_controls.py's.  Not in run_tests: a dozen builds.

    python src/csrc/speakout/so_controls.py

Without a control: ICW2 being skipped (the firmware's OCW1 follows it, so no test sees the difference).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CPU = os.path.join(os.path.dirname(HERE), "cpu")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
sys.path.insert(0, CPU)
from tools import repo_paths  # noqa: E402
from contract_controls import RunError, run_tests  # noqa: E402

UC = "    b->debt = done - n;"
VARIANTS = {
    "EOI clears the highest in service": (
        "so_icu.c", "            c->isr &= (uint8_t)(c->isr - 1);", "            c->isr = 0;", ["icu_priority"]),
    "in service blocks itself and lower": (
        "so_icu.c", "(c->isr & ((2 << ir) - 1))", "(c->isr & ((1 << ir) - 1))", ["icu_priority"]),
    "the mask": (
        "so_icu.c", "(c->imr >> ir & 1) || ", "", ["icu_masked"]),
    "an offer with IE = 0 dropped": (
        "so_board.c", "so_icu_offer(&b->icu, ir, (r.psw >> 9) & 1)", "so_icu_offer(&b->icu, ir, 1)", ["icu_masked"]),
    "reading port 0 loads the next": (
        "so_scu.c", "        s->loaded = pop(s);\n        return v;", "        s->loaded = -1;\n        return v;",
        ["scu_bytes"]),
    "the chip's request first": (
        "so_board.c", "    if (chip_request)\n        ir = SO_IRQ_CHIP;",
        "    if (chip_request && !so_scu_pending(&b->scu))\n        ir = SO_IRQ_CHIP;", ["offer_order"]),
    "power-on restores FFFF0h": (
        "so_board.c", "    memcpy(b->mem + RESET_VECTOR, keep, 5);\n", "", ["power_on"]),
    "the chip's window is R0-R4": (
        "so_board.c", "a <= CHIP_BASE + 4)", "a <= CHIP_BASE + 7)", ["chip_window"]),
    "Unicorn's count for a REP's exit": (
        "so_board.c", "            if (!z_ended)\n                done++;", "", ["unicorn_counts"]),
    "no exit count when Z ends it": (
        "so_board.c", "int z_ended = compares && (b->rep == 0xF3 ? !zf : zf);", "int z_ended = 0; (void)zf;",
        ["unicorn_counts"]),
    "a count carried into the next call": (
        "so_board.c", UC, "    b->debt = 0;", ["unicorn_counts"]),
}


def build(d, exe, env, bindir):
    objs = []
    for src, tool, flags in [(os.path.join(d, "cpu", "v40_mame.cpp"), "g++",
                              ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wno-sign-compare"])] + [
            (os.path.join(d, "speakout", f), "gcc", ["-O2", "-std=gnu89"])
            for f in ("so_board.c", "so_icu.c", "so_scu.c", "so_hex.c", "test_so_board.c")]:
        o = src + ".o"
        subprocess.run([os.path.join(bindir, tool)] + flags + ["-I" + os.path.join(d, "cpu"),
                                                               "-I" + os.path.join(d, "speakout"), "-c", src, "-o", o],
                       env=env, check=True)
        objs.append(o)
    subprocess.run([os.path.join(bindir, "g++"), "-o", exe] + objs, env=env, check=True)


def copy(tmp, name):
    d = os.path.join(tmp, name)
    shutil.copytree(CPU, os.path.join(d, "cpu"))
    shutil.copytree(HERE, os.path.join(d, "speakout"))
    return d


def main():
    bindir = repo_paths.bin_dir("W64DEVKIT") if os.name == "nt" else "/usr/bin"
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        d = copy(tmp, "base")
        exe = os.path.join(d, "tb")
        build(d, exe, env, bindir)
        try:
            results, f = run_tests(exe, suffixes=("",))
        except RunError as e:
            print("BAD  the board as it is: %s" % e)
            sys.exit(1)
        inventory = sorted(results)
        print("%-4s the board as it is -> %d tests, failing: %s" % ("ok" if not f else "BAD", len(inventory),
                                                                    ", ".join(f) or "none"))
        bad += bool(f)
        for k, (rule, (fname, old, new, tests)) in enumerate(VARIANTS.items()):
            d = copy(tmp, "v%d" % k)
            p = os.path.join(d, "speakout", fname)
            t = open(p, encoding="utf-8").read()
            assert t.count(old) == 1, "the rule's code moved: %s" % rule
            open(p, "w", encoding="utf-8", newline="\n").write(t.replace(old, new))
            exe = os.path.join(d, "tb")
            build(d, exe, env, bindir)
            try:
                _results, f = run_tests(exe, inventory, suffixes=("",))
            except RunError as e:
                bad += 1
                print("BAD  undone: %-36s -> the run itself failed: %s" % (rule, e))
                continue
            ok = f == sorted(tests)
            bad += not ok
            print("%-4s undone: %-36s -> failing: %s" % ("ok" if ok else "BAD", rule, ", ".join(f) or "none"))
    print("controls: %s" % ("all as expected" if not bad else "%d NOT as expected" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
