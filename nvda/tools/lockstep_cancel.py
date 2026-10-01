"""The DEFAULT lockstep's own cancel race (Astra, Reply 112 item 3 and Reply 114): retained, reproduced exactly, and
the opt-in prototype that clears it (bl_host.h "cancel_settle"; EXPERIMENTAL, off -- the default is unchanged).

On a 2 ms grid of cancel times (0.2-2.0 s into a three-line say, after two utterances and a rate change to 14; the
same session as run_ahead_cancel.py's first sweep) the lockstep leaked at 2 of 901 points, both where the unit stages
its next line ("And a third one.") into its line buffer -- traced with watchpoints (the trace build of bl.dll; the
retained traces are in the private investigation folder):

  1.040 s  the unit sleeps; ^X arrives, and the chip's request is given to it in that same slice.  The firmware loads
           its pause, its ^X handler resets the line pointer to the buffer's start (43410 <- AC, 433AC <- 00) -- and
           the interrupted copier then stores the next line's first character there (433AC <- 'A').  The respoken
           text is led by that "A": one phoneme (10), 33 ms.
  1.050 s  ^X arrives while the unit copies that line into its line buffer: the copy goes on, "And a " is in the
           buffer when the ^X is handled, and leads the respoken text: five phonemes (12 56 37 8 2), 150 ms.

The prototype is run_ahead's cancel fix carried over: before ^X the unit runs on, A/R not requesting and its chip
writes dropped, until its CPU waits for an interrupt (at most 0.1 s of CPU: bit 0, which completes the 1.050 copy),
and A/R stays not requesting over the first ^X slice (bit 1, which keeps the 1.040 copier from resuming).  Each bit
alone leaves one of the two.  On the 901-point grid: 2 leaks by default, 1 with either bit, 0 with both.

This check requires the default to reproduce both leaks EXACTLY (the respoken phonemes, and the line buffer's first
cell after the cancel), and the prototype (both bits) to clear them; the half fixes are reported.

    python lockstep_cancel.py
    LOCKSTEP_CANCEL_PROTO=n   the prototype's cancel_settle value under test (default 3; 1 or 2 must fail)
"""
import os
import sys
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [REPO, os.path.join(REPO, "src")]
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", "x64" if sys.maxsize > 2 ** 32 else "x86", "bl.dll")
PROTO = int(os.environ.get("LOCKSTEP_CANCEL_PROTO", "3"))
FIRST = ["Hello, this is the first line.", "And a second line follows it."]
FASTER = "The rate is faster now."
CUT = ["One line to be cut short, early on.", "Another line it never reaches.", "And a third one."]
RESAY = "Respoken at once."
LINE = 0x433AC                          # the line buffer (the trace: its pointer, 43410, runs from AC)
# the retained leaks: cancel time -> (the phonemes leading the respoken text, the cancelled text in the line buffer
# after the cancel, from 433AC)
LEAKS = {1.040: ([10], b"A"), 1.050: ([12, 56, 37, 8, 2], b"\x06And a ")}


def spoken(ws):
    r3, out = 0, []
    for _, r, v in ws:
        if r == 3:
            r3 = v
        if r == 0 and not r3 & 0x80 and v & 0x3F:
            out.append(v & 0x3F)
    return out


def done(u):
    for _ in range(300):
        u.run(0.03)
        if not u.busy():
            return


def session(args):
    settle, t_cancel = args
    from ssi263.native import SSI263C
    from hosts.native_blazie import NativeBlazie
    chip = SSI263C(params={"closure_noise_lead_ms": 10.0}, out_rate=22050)
    log = []
    u = NativeBlazie(DLL, os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"), chip=chip,
                     out_rate=22050, menu=("punct_none", "numbers_toggle"), key_start=3000000, key_gap=1500000,
                     board_lowpass_hz=5000.0, on_write=lambda t, r, v: log.append((t, r, v)))
    u.send(b"\x18")                     # the driver's boot (synthDrivers/blazie.py _boot)
    u.send(b"\r\x06")
    u.run(0.3)
    u.send(b"\x056V")
    u.run(0.05)
    u.turbo_between_lines = True
    u._lib.bh_set_int(u._h, b"cancel_settle", settle)
    u.say(FIRST)
    done(u)
    u.run(0.5)
    u.send(b"\x0514E")
    u.run(0.52)
    u.say(FASTER)
    done(u)
    u.run(0.5)
    cell = cost = settled = None
    if t_cancel is not None:
        u.say(CUT)
        t0 = chip.time
        while chip.time - t0 < t_cancel - 1e-9:
            u.run(min(0.03, t_cancel - (chip.time - t0)))
        cost = u.cancel()
        cell = u.memory(0)[LINE:LINE + 8]
        settled = u.geti("cancel_settled")
        u._drain()
    n0 = len(log)
    u.say(RESAY)
    done(u)
    u._drain()
    u.close()
    return settle, t_cancel, spoken(log[n0:]), cell, cost, settled


if __name__ == "__main__":
    runs = [(0, None)] + [(s, t) for s in sorted({0, 1, 2, 3, PROTO}) for t in sorted(LEAKS)]
    with Pool(min(len(runs), os.cpu_count() or 1)) as p:
        res = p.map(session, runs)
    ref = res[0][2]
    failures = 0
    for settle, t, sp, cell, cost, settled in res[1:]:
        lead = LEAKS[t][0]
        led = sp[:len(sp) - len(ref)] if sp[len(sp) - len(ref):] == ref else None
        how = ("the respoken text led by %s" % led if led else "the respoken text clean") if led is not None \
            else "the respoken text NOT found: %s" % sp[:8]
        info = "%s; line buffer after the cancel %r; cancel %.0f ms chip time%s" % (
            how, cell.split(b"\xff")[0], cost * 1e3, "" if settle & 1 == 0 else ", settled %d" % settled)
        if settle == 0:                 # the default: the retained leak, exactly
            ok = led == lead and cell.startswith(LEAKS[t][1] + b"\xff")
            what = "the default reproduces the retained leak"
        elif settle == PROTO:           # the prototype under test: clean
            ok = led == []
            what = "the prototype clears it"
        else:
            print("     %.3f s, cancel_settle %d (half the prototype, reported): %s" % (t, settle, info))
            continue
        failures += not ok
        print("%-4s %.3f s, cancel_settle %d (%s): %s" % ("ok" if ok else "FAIL", t, settle, what, info))
    print("lockstep cancel race (English, reference %s): %s" % (ref[:6], "all ok" if not failures
                                                                else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)
