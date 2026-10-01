"""Run ahead: a cancel, then at once a new say (NVDA's flow on every keypress) -- the new utterance must begin with its
own phonemes, never the cancelled text's.  Through bl.dll with the driver's boot, in parallel.

The finding (2026-09-30): the next utterance could begin with 1-2 phonemes of the cancelled text, or with the whole
rest of it.  The mechanism, traced with watchpoints (a trace build of bl.dll; the retained trace is in the private
investigation folder): the capture stops the unit's CPU wherever its bound falls -- just after an acknowledgement,
which wakes it -- so ^X met the firmware mid-routine, copying the next line's text into its line buffer, and that text
survived the ^X (awake at 97% of the capture's stops, 15% of the lockstep's time).  And the chip's request, given to the
unit at once after the cancel, had it load a phoneme of the dropped script and resume that text work before handling
its ^X.  The fix (run_ahead.h ra_settle, bl_host.c bh_cancel): before ^X the unit runs on, nothing more acknowledged,
until its CPU waits for an interrupt (at most RA_SETTLE_S, 0.1 s of CPU), and its A/R stays not requesting over the first ^X slice.

Each sweep: cancel times over a say, in run ahead (--lockstep: the same in the lockstep too, for reference -- it has
its own, rarer race at line boundaries, 1 of 25 on the first sweep: reported, not gated).  The respoken utterance's
spoken phonemes must equal the reference -- the respoken text said alone after the same history -- exactly.

    python run_ahead_cancel.py [--es] [--quick] [--lockstep]
    RUN_AHEAD_CANCEL_BREAK=settle   run_ahead.h's RA_BRK_SETTLE: the cancel meets the unit where the capture stopped
                                    it, the chip's request at once (must fail, English)
Spanish: the same sweeps pass with the control too -- its firmware kept no cancelled text in them (checked, 7 x 281
cancel times) -- so they guard against a regression but cannot show the fix.
"""
import os
import sys
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [REPO, os.path.join(REPO, "src")]
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", "x64" if sys.maxsize > 2 ** 32 else "x86", "bl.dll")
SPANISH = "--es" in sys.argv
QUICK = "--quick" in sys.argv
LOCKSTEP = "--lockstep" in sys.argv             # the lockstep's own counts too, for reference (twice the work)
BREAK = {"": 0, "settle": 6}[os.environ.get("RUN_AHEAD_CANCEL_BREAK", "")]   # run_ahead.h's RA_BRK_* order
RATE = 22050

if SPANISH:
    FW, STATE, ENC = "BL2SPA.BNS", "bl2spa_fresh.state", "cp850"
    FIRST = ["Hola, esta es la primera línea.", "Y la segunda la sigue."]
    FASTER = "Ahora la velocidad es mayor."
    CUT = ["Una línea que se corta pronto.", "Otra que nunca llega.", "Y una tercera."]
    RESAY = "Dicho otra vez."
else:
    FW, STATE, ENC = "BL2ENG.BNS", "bl2_2003_warm.state", "latin-1"
    FIRST = ["Hello, this is the first line.", "And a second line follows it."]
    FASTER = "The rate is faster now."
    CUT = ["One line to be cut short, early on.", "Another line it never reaches.", "And a third one."]
    RESAY = "Respoken at once."
# (name, history's unit rate or None, cancel after: seconds, or ("blocks", n) of 0.03 s as the driver's worker runs)
GRID = [0.2 + 0.05 * k for k in range(25)]
SWEEPS = [("state history, rate 14", 14, "times", GRID),
          ("no history, factory rate", None, "times", GRID),
          ("the driver's blocks, rate 14", 14, "blocks", list(range(4, 40)))]
if QUICK:
    SWEEPS = SWEEPS[:1]


def spoken(ws):
    r3, out = 0, []
    for _, r, v in ws:
        if r == 3:
            r3 = v
        if r == 0 and not r3 & 0x80 and v & 0x3F:
            out.append(v & 0x3F)
    return out


def done(u):
    for _ in range(400):
        u.run(0.03)
        if not u.busy():
            return
    raise RuntimeError("never done")


def one(args):
    ahead, rate, kind, at = args
    from ssi263.native import SSI263C
    from hosts.native_blazie import NativeBlazie
    chip = SSI263C(params={"closure_noise_lead_ms": 10.0}, out_rate=RATE)
    log = []
    u = NativeBlazie(DLL, os.path.join(ENG, FW), os.path.join(ENG, STATE), chip=chip, out_rate=RATE,
                     menu=("punct_none", "numbers_toggle"), key_start=3000000, key_gap=1500000, board_lowpass_hz=5000.0,
                     on_write=lambda t, r, v: log.append((t, r, v)))
    u.encoding = ENC
    u.send(b"\x18")
    u.send(b"\r\x06")
    u.run(0.3)
    u.send(b"\x056V")
    u.run(0.05)
    u.turbo_between_lines = True
    u.run_ahead = ahead
    u.run_ahead_break = BREAK if ahead else 0
    try:
        if rate:
            u.say(FIRST)
            done(u)
            u.run(0.5)
            u.send(b"\x05%dE" % rate)
            u.run(0.52)
            u.say(FASTER)
            done(u)
            u.run(0.5)
        if at is None:                   # the reference: the respoken text alone after the same history
            n0 = len(log)
            u.say(RESAY)
            done(u)
            return spoken(log[n0:])
        if kind == "blocks":             # as the driver: its lines packed two to a say, 0.03 s blocks, then cancel
            u.say([CUT[0], " ".join(CUT[1:])])
            for _ in range(at):
                u.run(0.03)
                u.busy()
        else:
            u.say(CUT)
            t0 = chip.time
            while chip.time - t0 < at - 1e-9:
                u.run(min(0.03, at - (chip.time - t0)))
        u.cancel()
        u._drain()
        n0 = len(log)
        u.say(RESAY)
        done(u)
        return spoken(log[n0:])
    finally:
        u.close()


def main():
    lang = "Spanish" if SPANISH else "English"
    print("emulated time (Z180 cycles, the chip model): not a measurement of a real unit")
    failures = 0
    # a few workers: run_tests.py runs the checks in parallel already, and its wall-clock checks must not starve
    with Pool(min(4, os.cpu_count() or 1)) as pool:
        for name, rate, kind, ats in SWEEPS:
            want = pool.map(one, [(1, rate, kind, None)])[0]
            modes = (1, 0) if LOCKSTEP else (1,)
            got = pool.map(one, [(m, rate, kind, at) for m in modes for at in ats])
            n = len(ats)
            ahead = [(at, g) for at, g in zip(ats, got[:n]) if g != want]
            lock = [(at, g) for at, g in zip(ats, got[n:]) if g != want]
            failures += bool(ahead)
            unit = "blocks" if kind == "blocks" else "s"
            print("%-4s %s: run ahead %d of %d led by other phonemes%s%s" % (
                "FAIL" if ahead else "ok", name, len(ahead), n,
                " (lockstep %d of %d, for reference)" % (len(lock), n) if LOCKSTEP else "",
                "".join("; %s %s: %s" % (at if kind == "blocks" else "%.2f" % at, unit, g[:6]) for at, g in ahead[:4])))
            if lock:
                print("     lockstep at %s" % ", ".join("%s %s: %s" % (at if kind == "blocks" else "%.3f" % at, unit,
                                                                        g[:5]) for at, g in lock[:4]))
    print("run ahead cancel (%s): %s" % (lang, "all ok" if not failures else "%d FAILED" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
