"""The Accent-mini on MAME's 8086 against Unicorn: the same scripted scenarios through src/hosts/accent.py on both CPUs,
in one process, and everything the migration promises compared -- and the run FAILS on any of it (Astra, Reply 104):

  - every SSI-263 write: its value AND its time (the same writes, in the same order, at the same chip time)
  - the audio (the PCM, hashed block by block)
  - the CPU registers at the end (accent.py's regs(): the general, segment and IP registers)
  - FLAGS, by an explicit policy (Astra, Reply 106; FLAGS_POLICY below), sampled at every software interrupt the
    driver raises (the host's seam: INT 67h, 21h, 17h), at every OUT, and at the end
  - the host's log (DOS, EMS and the driver's stops, line by line)
  - init only: memory after INIT, byte for byte, except an explicit allow-list (ALLOWED_INIT_BYTES): the four stack
    bytes that are FLAGS images' high bytes, F-nibble on the 8086 and 0 on Unicorn, low nibble equal -- a listed byte
    must still match that pattern, and any other difference fails
  - state only: the two INIT snapshots (registers, FLAGS by the same policy, INIT's chip writes, the host's card/EMS
    state)

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
(no longer a FLAGS image's byte), =snapshot changes the MAME snapshot's registers; for FLAGS (Reply 106): =flags flips
IF in the middle sample of MAME's, =carry flips CF in MAME's final FLAGS (an arithmetic flag its last writer
defines), =flagsmask clears bit 12 in MAME's middle sample (the masked bits must still be the recorded values),
=snapflags flips IF in the MAME snapshot's FLAGS.  nvda/tools/run_tests.py runs each and checks its mark.

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
from hosts.ucmini import UC_X86_REG_EFLAGS  # noqa: E402  (pc86.py answers it too: the 8086's FLAGS)
from ssi263.native import SSI263C  # noqa: E402
from tools.render_accent_demo import LINES  # noqa: E402

DVC = os.path.join(REPO, "firmware", "aicom-accent-mini", "SPKEMS.DVC")
QUICK = "--quick" in sys.argv
CONTROL = "--control" in sys.argv
PERTURB = os.environ.get("I86_COMPARE_PERTURB", "")
PERTURBS = ("", "time", "pcm", "reg", "log", "mem", "allowed", "snapshot", "flags", "carry", "flagsmask", "snapflags")
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


# FLAGS_POLICY (Astra, Reply 106: FLAGS is compared, not left out because bits 12-15 differ).
#   - Required everywhere: IF, DF and TF (bits 9, 10, 8), and the fixed bits 1, 3 and 5.
#   - The arithmetic flags (CF, PF, AF, ZF, SF, OF) are required where their last writer defines them.  None of the
#     samples below has shown an arithmetic difference in any scenario (the 8086's and Unicorn's results agree even
#     where Intel's table says "U"), so none is masked: ARITH_UNDEFINED is 0 and all of bits 0-11 must be equal.  A
#     difference found later fails the run; masking it needs a named entry here that shows the sample's last flag
#     writer and the Table 2-21 column marking that flag undefined.
#   - Masked from the equality, and only these: bits 12-15, reserved on the 8086, which reads them as 1 (F000h), and
#     IOPL/NT/reserved on Unicorn's 386 in real mode, which reads 0.  Masked is not ignored: every sample must show
#     exactly those recorded values, FLAGS_HIGH, or it fails.
# The samples: every software interrupt the driver raises, at the host's seam, before the host services it (INT
# 67h, 21h, 17h: the flags the driver's code left), every OUT the driver makes, and the FLAGS at the end (the idle
# loop's).  A word OUT would be two samples on MAME (two byte accesses) and one on Unicorn: the driver has none, and
# a different number of samples fails.  The INIT snapshots' FLAGS are held to the same policy.
FLAGS_REQUIRED = 0x0700 | 0x002A                  # TF, IF, DF; the fixed bits 1, 3, 5
FLAGS_ARITH = 0x08D5                              # OF, SF, ZF, AF, PF, CF
ARITH_UNDEFINED = 0x0000                          # none needed (see above)
FLAGS_COMPARED = (FLAGS_REQUIRED | FLAGS_ARITH) & ~ARITH_UNDEFINED
FLAGS_MASKED = 0xF000
FLAGS_HIGH = {"unicorn": 0x0000, "mame": 0xF000}   # the masked bits' recorded values
assert FLAGS_COMPARED | FLAGS_MASKED == 0xFFFF
FLAG_NAMES = ((0x0001, "CF"), (0x0004, "PF"), (0x0010, "AF"), (0x0040, "ZF"), (0x0080, "SF"), (0x0100, "TF"),
              (0x0200, "IF"), (0x0400, "DF"), (0x0800, "OF"))


def _sample_flags(method, kind):
    """Wrap the host's hook `method` (bound when an Accent is built, so this runs first): a sample of FLAGS per call
    on an Accent that has flag_samples, before the host acts."""
    orig = getattr(Accent, method)

    def hook(self, uc, *args):
        samples = getattr(self, "flag_samples", None)
        if samples is not None:
            samples.append((kind, args[0], uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF))
        return orig(self, uc, *args)
    setattr(Accent, method, hook)


_sample_flags("_int", "INT")
_sample_flags("_out", "OUT")


def flags_site(s):
    return "the end" if s[0] == "end" else "%s %02Xh" % (s[0], s[1]) if s[0] == "INT" else "%s %03Xh" % (s[0], s[1])


def flags_bits(x):
    return "%04X (%s)" % (x, " ".join(n for b, n in FLAG_NAMES if x & b) or "fixed bits")


def flags_problems(fu, fm):
    """FLAGS_POLICY over two sample lists [(kind, number, flags)]; returns the problems, as text (none: [])."""
    if len(fu) != len(fm) or any(a[:2] != b[:2] for a, b in zip(fu, fm)):
        k = next((i for i, (a, b) in enumerate(zip(fu, fm)) if a[:2] != b[:2]), min(len(fu), len(fm)))
        return ["samples differ in number or site (%d / %d; first at %d)" % (len(fu), len(fm), k)]
    out = []
    low = [i for i, (a, b) in enumerate(zip(fu, fm)) if (a[2] ^ b[2]) & FLAGS_COMPARED]
    if low:
        i = low[0]
        out.append("DIFFER at %d of %d samples: first %d (%s): unicorn %04X, mame %04X, bits %s" % (
            len(low), len(fu), i, flags_site(fu[i]), fu[i][2], fm[i][2], flags_bits((fu[i][2] ^ fm[i][2]) & 0x0FFF)))
    high = [i for i, (a, b) in enumerate(zip(fu, fm))
            if a[2] & FLAGS_MASKED != FLAGS_HIGH["unicorn"] or b[2] & FLAGS_MASKED != FLAGS_HIGH["mame"]]
    if high:
        i = high[0]
        out.append("bits 12-15 not the recorded %04X / %04X at %d of %d samples: first %d (%s): unicorn %04X, mame "
                   "%04X" % (FLAGS_HIGH["unicorn"], FLAGS_HIGH["mame"], len(high), len(fu), i, flags_site(fu[i]),
                             fu[i][2], fm[i][2]))
    return out


class Run:
    """One core through one scenario: what the chip was told, the audio, and the CPU."""

    def __init__(self, core, dvc):
        self.core = core
        self.a = Accent(dvc, chip=SSI263C(), core=core)
        self.a.keep_writes = True
        self.a.flag_samples = []                       # FLAGS_POLICY's samples (the hooks wrapped above)
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
    fu = list(u.a.flag_samples) + [("end", 0, u.a.uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF)]
    fm = list(m.a.flag_samples) + [("end", 0, m.a.uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF)]
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
    if PERTURB in ("flags", "flagsmask"):
        k, n, f = fm[len(fm) // 2]
        fm[len(fm) // 2] = (k, n, f ^ 0x0200 if PERTURB == "flags" else f & ~0x1000)   # IF; or bit 12 (masked)
    if PERTURB == "carry":
        k, n, f = fm[-1]
        fm[-1] = (k, n, f ^ 0x0001)                    # CF at the end
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
    bad = flags_problems(fu, fm)
    n_int = sum(1 for s in fu if s[0] == "INT")
    ok &= line(not bad, name, "FLAGS identical in bits 0-11 at %d samples (%d INTs, %d OUTs, the end); bits 12-15 "
               "masked, %04X / %04X (unicorn / mame) at every one" % (len(fu), n_int, len(fu) - 1 - n_int,
                                                                      FLAGS_HIGH["unicorn"], FLAGS_HIGH["mame"])
               if not bad else "FLAGS " + "; ".join(bad))
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
        if PERTURB == "snapflags":
            snap_m["eflags"] ^= 0x0200                 # IF
        parts = [k for k in ("regs", "chip_writes", "host") if states["unicorn"][k] != snap_m[k]]
        if flags_problems([("end", 0, states["unicorn"]["eflags"] & 0xFFFF)], [("end", 0, snap_m["eflags"] & 0xFFFF)]):
            parts.append("eflags")
        ok &= line(not parts, "state", "INIT snapshots: registers, FLAGS (FLAGS_POLICY: %04X / %04X), INIT's chip "
                   "writes and the host's card/EMS state %s" % (
                       states["unicorn"]["eflags"] & 0xFFFF, snap_m["eflags"] & 0xFFFF,
                       "identical" if not parts else "DIFFER in %s" % ", ".join(parts)))
    print("compare_i86_accent: %s" % ("every promised invariant identical in every scenario run" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
