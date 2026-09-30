"""The Accent-mini on MAME's 8086 against Unicorn: the same scripted scenarios through src/hosts/accent.py on both CPUs,
in one process, and every SSI-263 write compared -- values (must be identical), then times, the audio and the CPU state.

    python src/csrc/cpu/compare_i86_accent.py [--quick] [--control] [--dvc PATH]

Scenarios (deterministic: no wall clock, no threads):
  init      the driver's INIT from scratch (EMS set-up, "Accent ready" queued), then the machine state compared
  demo      boot, then the demo dialogue (tools/render_accent_demo.py) as the NVDA add-on sends it: each line in the
            background (say(..., background=True)), 30 ms run() blocks until not busy
  settings  rate / pitch / volume / voice commands between and inside utterances, capital pitch
  cancel    utterances cut by a cancel after a seeded number of blocks, with more text at once (the fuzz's shape)
  state     boot from a Unicorn-made INIT snapshot (the add-on's SPKEMS.state path) on both, and the MAME core's own
            snapshot against Unicorn's
--quick runs a shorter version of each (the run_tests gate); --control flips one value of the MAME core's writes,
and the comparison must then say DIFFER (its must-fail control).  Needs pc86.dll (src/csrc/blazie/build_board.py) and
Unicorn's DLL (src/hosts/bin, as the add-ons use).  Exit 0 when every scenario's write values are identical.

Timing: both cores are coupled to the chip the host's way (cpu_ips instructions per chip second, in slices), so a
write's time can differ only where the two cores count instructions differently (a REP string instruction: Unicorn
counts one more pass than MAME) and a slice boundary falls between; each difference is listed.  MAME's own T-states
are reported beside: what a cycle-true coupling would mean is a separate decision (src/csrc/cpu/README.md).
"""
import hashlib
import os
import random
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "src"))
sys.path.insert(0, REPO)
from hosts.accent import Accent  # noqa: E402
from ssi263.native import SSI263C  # noqa: E402
from tools.render_accent_demo import LINES  # noqa: E402

DVC = os.path.join(REPO, "firmware", "aicom-accent-mini", "SPKEMS.DVC")
QUICK = "--quick" in sys.argv
CONTROL = "--control" in sys.argv
CORES = ("unicorn", "mame")
BLOCK = 0.03


class Run:
    """One core through one scenario: what the chip was told, the audio, and the CPU."""

    def __init__(self, core, dvc):
        self.a = Accent(dvc, chip=SSI263C(), core=core)
        self.a.keep_writes = True
        self.pcm = hashlib.sha256()
        self.t0 = time.perf_counter()

    def audio(self, y):
        self.pcm.update(self.a.chip.dsp.pcm16(y, 1.0))

    def speak(self, text, max_blocks=2000):
        a = self.a
        a.say(text + "\r", background=True)
        for _ in range(max_blocks):
            self.audio(a.run(BLOCK))
            if not a.busy():
                return
        raise RuntimeError("never done: %r" % text[:40])

    def settings(self, cmd):
        self.a.say(cmd, speech=False)


def s_init(r):
    r.a.init()


def s_demo(r):
    r.a.boot()
    for line in LINES[:3] if QUICK else LINES:
        r.speak(line)


def s_settings(r):
    r.a.boot()
    r.settings("\x1bR7\x1bP6\x1bV8")
    r.speak("Faster, higher and louder than before.")
    r.settings("\x1bR2\x1bP2\x1bM1")
    r.speak("And now slow, low, and another voice.")
    if not QUICK:
        r.settings("\x1bP9")
        r.speak("A")                                   # a capital, as the add-on raises pitch for one
        r.settings("\x1bP2")
        r.speak("1234 dollars and 5.67 cents; the 3rd of May, 1999!")
        r.settings("\x1bR5\x1bP5\x1bV5\x1bM0")
        r.speak("Back to the defaults, e-mail a@b.com (maybe).")


def s_cancel(r):
    rng = random.Random(7)
    words = ("the quick brown fox jumps over a lazy dog while Accent reads the mail from Tomi about "
             "speech synthesis on the SSI two sixty three chip one two three").split()
    r.a.boot()
    for _ in range(4 if QUICK else 14):
        text = " ".join(rng.choice(words) for _ in range(rng.randint(3, 25)))
        r.a.say(text + "\r", background=True)
        for _ in range(rng.randint(1, 25)):
            r.audio(r.a.run(BLOCK))
        r.a.cancel()
    r.speak("After all those cancels, one whole sentence.")


def s_state(r, state):
    r.a.boot(state=state)
    r.speak("From the saved machine." if QUICK else LINES[0])


def compare(name, runs):
    """Values must be identical; times are listed; returns True when the values are."""
    u, m = runs["unicorn"], runs["mame"]
    wu, wm = list(u.a.writes), list(m.a.writes)
    if CONTROL and wm:
        t, reg, v = wm[len(wm) // 2]
        wm[len(wm) // 2] = (t, reg, v ^ 0x01)
    vu, vm = [(reg, v) for _, reg, v in wu], [(reg, v) for _, reg, v in wm]
    same = vu == vm
    if same:
        print("ok   %-9s %6d writes, values identical" % (name, len(vu)))
    else:
        k = next((i for i, (x, y) in enumerate(zip(vu, vm)) if x != y), min(len(vu), len(vm)))
        print("FAIL %-9s write values DIFFER at write %d of %d/%d: unicorn %s, mame %s" % (
            name, k, len(vu), len(vm), wu[k] if k < len(wu) else None, wm[k] if k < len(wm) else None))
    dts = [(i, b[0] - a[0]) for i, (a, b) in enumerate(zip(wu, wm)) if a[0] != b[0]]
    if dts:
        big = max(dts, key=lambda x: abs(x[1]))
        print("     %-9s times: %d of %d differ, the largest %+.6f s at write %d; first at write %d (%+.6f s)" % (
            "", len(dts), len(wu), big[1], big[0], dts[0][0], dts[0][1]))
    else:
        print("     %-9s times: all identical" % "")
    print("     %-9s audio %s; CPU %s; host log %s" % (
        "", "identical" if u.pcm.digest() == m.pcm.digest() else "DIFFERS",
        "identical" if u.a.regs() == m.a.regs() else "DIFFERS (%s | %s)" % (u.a.regs(), m.a.regs()),
        "identical" if u.a.log == m.a.log else "DIFFERS"))
    steps, cycles = m.a.uc.steps, m.a.uc.cycles
    print("     %-9s mame: %d steps, %d T-states (%.1f per step), chip time %.2f s, aliased opcodes %d; "
          "%.1f s / %.1f s of host time (unicorn / mame)" % (
              "", steps, cycles, cycles / max(steps, 1), m.a.chip.time, m.a.uc.aliased, u.elapsed, m.elapsed))
    return same


def main():
    dvc = sys.argv[sys.argv.index("--dvc") + 1] if "--dvc" in sys.argv else DVC
    ok = True
    for name, fn in (("init", s_init), ("demo", s_demo), ("settings", s_settings), ("cancel", s_cancel)):
        runs = {}
        for core in CORES:
            r = Run(core, dvc)
            fn(r)
            r.elapsed = time.perf_counter() - r.t0
            runs[core] = r
        ok &= compare(name, runs)
        if name == "init":
            mem_u = bytes(runs["unicorn"].a.uc.mem_read(0, 0x100000))
            mem_m = bytes(runs["mame"].a.uc.mem_read(0, 0x100000))
            diff = [i for i in range(0x100000) if mem_u[i] != mem_m[i]]
            print("     %-9s memory after INIT: %d bytes differ%s" % ("", len(diff), (
                " (%s)" % ", ".join("%05X: %02X/%02X" % (i, mem_u[i], mem_m[i]) for i in diff[:6])) if diff else ""))
    # the snapshot path: one state made on Unicorn (what the add-on ships), and one made on MAME
    states = {core: Accent(dvc, chip=SSI263C(), core=core).init_state() for core in CORES}
    runs = {}
    for core in CORES:
        r = Run(core, dvc)
        s_state(r, states["unicorn"])
        r.elapsed = time.perf_counter() - r.t0
        runs[core] = r
    ok &= compare("state", runs)
    same_state = states["unicorn"]["regs"] == states["mame"]["regs"] and \
        states["unicorn"]["chip_writes"] == states["mame"]["chip_writes"] and \
        states["unicorn"]["host"] == states["mame"]["host"]
    print("     %-9s INIT snapshots: registers, INIT's chip writes and the host's card/EMS state %s" % (
        "", "identical" if same_state else "DIFFER"))
    ok &= same_state
    print("compare_i86_accent: %s" % ("write values identical in every scenario" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
