"""The Accent-mini on MAME's 8086 against Unicorn: the same scripted scenarios through src/hosts/accent.py on both CPUs,
in one process, and everything the migration promises compared -- and the run FAILS on any of it (Astra, Reply 104):

  - every SSI-263 write: its value AND its time (the same writes, in the same order, at the same chip time)
  - the audio (the PCM, hashed block by block)
  - the CPU registers at the end (accent.py's regs(): the general, segment and IP registers; FLAGS is not among them:
    bits 12-15 read 1 on the 8086 and 0 on Unicorn's 386, and the undefined flags are each core's own)
  - the host's log (DOS, EMS and the driver's stops, line by line)
  - init only: memory after INIT, byte for byte, except an explicit allow-list (ALLOWED_INIT_BYTES): the four stack
    bytes that are FLAGS images' high bytes, F-nibble on the 8086 and 0 on Unicorn, low nibble equal -- a listed byte
    must still match that pattern, and any other difference fails
  - state only: the two INIT snapshots (registers, INIT's chip writes, the host's card/EMS state)

    python src/csrc/cpu/compare_i86_accent.py [--quick] [--only SCENARIO[,SCENARIO]] [--control] [--dvc PATH]

Scenarios (deterministic: no wall clock, no threads):
  init      the driver's INIT from scratch (EMS set-up, "Accent ready" queued), then the machine state compared
  demo      boot, then the demo dialogue (tools/render_accent_demo.py) as the NVDA add-on sends it: each line in the
            background (say(..., background=True)), 30 ms run() blocks until not busy
  settings  rate / pitch / volume / voice commands between and inside utterances, capital pitch
  cancel    utterances cut by a cancel after a seeded number of blocks, with more text at once (the fuzz's shape)
  state     boot from a Unicorn-made INIT snapshot (the add-on's SPKEMS.state path) on both, and the MAME core's own
            snapshot against Unicorn's
--quick runs a shorter version of each (the run_tests gate); --only runs the named scenarios alone.  Needs pc86.dll
(src/csrc/blazie/build_board.py) and Unicorn's DLL (src/hosts/bin, as the add-ons use).  Exit 0 when every promised
invariant holds in every scenario run, 1 otherwise.

Must-fail controls, each perturbing exactly ONE thing in the MAME run's results (so the comparison must fail on it and
on nothing else, with its own mark): --control flips one write's value; I86_COMPARE_PERTURB=time moves one write's
time by 1 us, =pcm changes one PCM sample of the first audio block, =reg changes AX in the final registers, =log adds
a line to the host's log, =mem changes one byte of memory after INIT outside the allow-list, =allowed one inside it
(no longer a FLAGS image's byte), =snapshot changes the MAME snapshot's registers.  nvda/tools/run_tests.py runs each and checks its mark.

Timing: both cores are coupled to the chip the host's way -- cpu_ips instructions per chip second, in slices: a
compatibility policy kept from Unicorn, not a clock (src/csrc/cpu/README.md) -- so a write's time could differ only
where the two cores count instructions differently (a REP string instruction: Unicorn counts one more pass than MAME)
and a slice boundary fell between; none does.  MAME's own T-states (Intel's counts) are reported beside, and move no
write.
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
PERTURB = os.environ.get("I86_COMPARE_PERTURB", "")
PERTURBS = ("", "time", "pcm", "reg", "log", "mem", "allowed", "snapshot")
CORES = ("unicorn", "mame")
SCENARIOS = ("init", "demo", "settings", "cancel", "state")
BLOCK = 0.03

# The tolerated memory difference after INIT (README.md, "The MAME 8086 against Unicorn"): these stack bytes are the
# high bytes of FLAGS images the driver pushes (PUSHF, INT), bits 12-15 set on the 8086 (F-nibble), clear on
# Unicorn's 386 in real mode; never read back as data.  Each must still be exactly that: Unicorn's high nibble 0,
# MAME's F, the low nibble equal.
ALLOWED_INIT_BYTES = (0x9FFB5, 0x9FFC3, 0x9FFD7, 0x9FFF9)


def flags_image_byte(u, m):
    return (u & 0xF0) == 0x00 and (m & 0xF0) == 0xF0 and (u & 0x0F) == (m & 0x0F)


class Run:
    """One core through one scenario: what the chip was told, the audio, and the CPU."""

    def __init__(self, core, dvc):
        self.core = core
        self.a = Accent(dvc, chip=SSI263C(), core=core)
        self.a.keep_writes = True
        self.pcm = hashlib.sha256()
        self.blocks = 0
        self.t0 = time.perf_counter()

    def audio(self, y):
        pcm = self.a.chip.dsp.pcm16(y, 1.0)
        if PERTURB == "pcm" and self.core == "mame" and self.blocks == 0 and len(pcm) >= 2:
            pcm = bytes([pcm[0] ^ 0x01]) + bytes(pcm[1:])         # the control: one sample's low bit
        self.blocks += 1
        self.pcm.update(pcm)

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


def line(ok, name, text):
    print("%-4s %-9s %s" % ("ok" if ok else "FAIL", name, text))
    return ok


def compare(name, runs):
    """Every promised invariant of one scenario; returns True when all hold.  One line each, ok or FAIL."""
    u, m = runs["unicorn"], runs["mame"]
    wu, wm = list(u.a.writes), list(m.a.writes)
    regs_u, regs_m = u.a.regs(), m.a.regs()
    log_u, log_m = list(u.a.log), list(m.a.log)
    # the must-fail controls: exactly one thing perturbed, in the MAME run's results
    if CONTROL and wm:
        t, reg, v = wm[len(wm) // 2]
        wm[len(wm) // 2] = (t, reg, v ^ 0x01)
    if PERTURB == "time" and wm:
        t, reg, v = wm[len(wm) // 2]
        wm[len(wm) // 2] = (t + 1e-6, reg, v)
    if PERTURB == "reg":
        regs_m = regs_m.replace("ax=", "ax=1", 1) if "ax=" in regs_m else regs_m + " perturbed"
    if PERTURB == "log":
        log_m.append("(a line the control added)")
    ok = True
    # values
    vu, vm = [(reg, v) for _, reg, v in wu], [(reg, v) for _, reg, v in wm]
    if vu == vm:
        ok &= line(True, name, "%6d writes, values identical" % len(vu))
    else:
        k = next((i for i, (x, y) in enumerate(zip(vu, vm)) if x != y), min(len(vu), len(vm)))
        ok &= line(False, name, "write values DIFFER at write %d of %d/%d: unicorn %s, mame %s" % (
            k, len(vu), len(vm), wu[k] if k < len(wu) else None, wm[k] if k < len(wm) else None))
    # times (of the writes both have, and their number)
    dts = [(i, b[0] - a[0]) for i, (a, b) in enumerate(zip(wu, wm)) if a[0] != b[0]]
    if not dts and len(wu) == len(wm):
        ok &= line(True, name, "times identical")
    elif dts:
        big = max(dts, key=lambda x: abs(x[1]))
        ok &= line(False, name, "write times DIFFER: %d of %d, the largest %+.6f s at write %d; first at write %d "
                   "(%+.6f s)" % (len(dts), len(wu), big[1], big[0], dts[0][0], dts[0][1]))
    else:
        ok &= line(False, name, "write times DIFFER: %d writes against %d" % (len(wu), len(wm)))
    # audio, registers, host log
    ok &= line(u.pcm.digest() == m.pcm.digest(), name, "audio %s (%d / %d blocks)" % (
        "identical" if u.pcm.digest() == m.pcm.digest() else "DIFFERS", u.blocks, m.blocks))
    ok &= line(regs_u == regs_m, name, "CPU registers %s" % (
        "identical" if regs_u == regs_m else "DIFFER (%s | %s)" % (regs_u, regs_m)))
    if log_u == log_m:
        ok &= line(True, name, "host log identical (%d lines)" % len(log_u))
    else:
        k = next((i for i, (x, y) in enumerate(zip(log_u, log_m)) if x != y), min(len(log_u), len(log_m)))
        ok &= line(False, name, "host log DIFFERS at line %d of %d/%d: unicorn %r, mame %r" % (
            k, len(log_u), len(log_m), log_u[k] if k < len(log_u) else None, log_m[k] if k < len(log_m) else None))
    steps, cycles = m.a.uc.steps, m.a.uc.cycles
    print("     %-9s mame: %d steps, %d T-states (%.1f per step), chip time %.2f s, aliased opcodes %d; "
          "%.1f s / %.1f s of host time (unicorn / mame)" % (
              "", steps, cycles, cycles / max(steps, 1), m.a.chip.time, m.a.uc.aliased, u.elapsed, m.elapsed))
    return ok


def compare_init_memory(runs):
    mem_u = bytearray(runs["unicorn"].a.uc.mem_read(0, 0x100000))
    mem_m = bytearray(runs["mame"].a.uc.mem_read(0, 0x100000))
    if PERTURB == "mem":
        mem_m[0x500] ^= 0x01                           # the control: one byte, outside the allow-list
    if PERTURB == "allowed":
        mem_m[ALLOWED_INIT_BYTES[0]] ^= 0x01           # the control: an allowed byte no longer a FLAGS image's
    diff = [i for i in range(0x100000) if mem_u[i] != mem_m[i]]
    allowed = [i for i in diff if i in ALLOWED_INIT_BYTES and flags_image_byte(mem_u[i], mem_m[i])]
    other = [i for i in diff if i not in allowed]
    def show(xs):
        return ", ".join("%05X: %02X/%02X" % (i, mem_u[i], mem_m[i]) for i in xs[:6])

    if other:
        return line(False, "init", "memory after INIT DIFFERS in %d byte(s) outside the allowed FLAGS images (%s)" % (
            len(other), show(other)))
    return line(True, "init", "memory after INIT identical but for %d allowed FLAGS image byte(s)%s" % (
        len(allowed), " (%s)" % show(allowed) if allowed else ""))


def main():
    dvc = sys.argv[sys.argv.index("--dvc") + 1] if "--dvc" in sys.argv else DVC
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else list(SCENARIOS)
    if PERTURB not in PERTURBS or any(s not in SCENARIOS for s in only):
        sys.exit("unknown I86_COMPARE_PERTURB %r or scenario in %r" % (PERTURB, only))
    ok = True
    for name, fn in (("init", s_init), ("demo", s_demo), ("settings", s_settings), ("cancel", s_cancel)):
        if name not in only:
            continue
        runs = {}
        for core in CORES:
            r = Run(core, dvc)
            fn(r)
            r.elapsed = time.perf_counter() - r.t0
            runs[core] = r
        ok &= compare(name, runs)
        if name == "init":
            ok &= compare_init_memory(runs)
    if "state" in only:
        # the snapshot path: one state made on Unicorn (what the add-on ships), and one made on MAME
        states = {core: Accent(dvc, chip=SSI263C(), core=core).init_state() for core in CORES}
        runs = {}
        for core in CORES:
            r = Run(core, dvc)
            s_state(r, states["unicorn"])
            r.elapsed = time.perf_counter() - r.t0
            runs[core] = r
        ok &= compare("state", runs)
        snap_m = dict(states["mame"])
        if PERTURB == "snapshot":
            snap_m["regs"] = ("the control's registers", snap_m["regs"])
        parts = [k for k in ("regs", "chip_writes", "host") if states["unicorn"][k] != snap_m[k]]
        ok &= line(not parts, "state", "INIT snapshots: registers, INIT's chip writes and the host's card/EMS state %s"
                   % ("identical" if not parts else "DIFFER in %s" % ", ".join(parts)))
    print("compare_i86_accent: %s" % ("every promised invariant identical in every scenario run" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
