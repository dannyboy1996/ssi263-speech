"""The lockstep's own cancel race (Astra, Reply 112 item 3, Replies 114 and 124-129): the shipping default must be
clean at every retained leak, and the race put back (cancel_settle 0) must reproduce each leak EXACTLY.

On the MAME Z180 (0.7) the race sits where the unit stages its next line ("And a third one.") into its line buffer,
in a three-line say cut ~1.04 s in, after two utterances and a rate change to 14 (the same session as
run_ahead_cancel.py's first sweep).  Astra's sweep of that window (1.015-1.090 s, 0.5 ms grid, 151 cancel times;
investigation/release07-review/mame_cancel_window.*, mame_cancel_default.*) found 8 leaks with cancel_settle 0 and
none with the default.  Two mechanisms, traced on the z180emu core with watchpoints (the trace build of bl.dll):

  the copier resumed  ^X arrives while the unit sleeps, and the chip's request is given to it in the same slice: the
                      firmware loads its pause, its ^X handler resets the line pointer to the buffer's start (43410
                      <- AC, 433AC <- 00), and the interrupted copier then stores one more character there (433AC <-
                      'A', 'n', ...).  The respoken text is led by it (or its first phoneme merges with it).
  the copy goes on    ^X arrives while the unit copies the line into its buffer: "And a " is there when the ^X is
                      handled, and leads the respoken text (12 56 37 8 2).

The fix, the default since 0.7 (bl_host.c, h->cancel_settle = 3; run_ahead's ra_settle carried over): before ^X the
unit runs on, A/R not requesting and its chip writes dropped, until its CPU waits for an interrupt (at most 0.1 s of
CPU: bit 0, which completes the copy), and A/R stays not requesting over the first ^X slice (bit 1, which keeps the
copier from resuming).

This check runs the DEFAULT, as the driver gets it (no override): every retained cancel time must respeak exactly
the reference -- the respoken text said alone after the same history.  The 1.040 and 1.050 s leaks of the z180emu
core (the 0.6 add-on's) are kept as times too: on MAME they are clean with and without the fix.

    python lockstep_cancel.py
    SSI263_BLAZIE_CANCEL_SETTLE=0   the control (hosts/native_blazie.py's override): the race put back.  Every MAME
                                    leak must come back EXACTLY as retained -- its respoken phonemes and the line
                                    buffer after the cancel -- each reported as a FAIL; a leak that moved or vanished
                                    is reported as such, and the control's marks then no longer match
    SSI263_BLAZIE_CANCEL_SETTLE=1   the settle alone (bit 0): the copier-resumed leaks come back, the copy-goes-on
                                    ones are cleared
"""
import os
import sys
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [REPO, os.path.join(REPO, "src")]
ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", "x64" if sys.maxsize > 2 ** 32 else "x86", "bl.dll")
FIRST = ["Hello, this is the first line.", "And a second line follows it."]
FASTER = "The rate is faster now."
CUT = ["One line to be cut short, early on.", "Another line it never reaches.", "And a third one."]
RESAY = "Respoken at once."
LINE = 0x433AC                          # the line buffer (the trace: its pointer, 43410, runs from AC)
SHIPPED = 3                             # bl_host.c's default since 0.7 (Astra, Reply 128)
# the retained MAME leaks with the race put back (cancel_settle 0; Astra's mame_cancel_default.json): cancel time ->
# (the phonemes the respoken text begins with, how many of the reference's first phonemes they replace, the line
# buffer's first 8 bytes after the cancel)
LEAKS = {
    1.0380: ([10], 0, "41ff6e6f74686572"),                       # the copier resumed: 'A'
    1.0385: ([56], 0, "6eff6e6f74686572"),                       # ... 'n'
    1.0395: ([10], 0, "61ff6e6f74686572"),                       # ... 'a'
    1.0410: ([7, 28], 1, "69ff6e6f74686572"),                    # ... 'i', merged with the R of "Respoken"
    1.0425: ([18, 28], 1, "6fff6e6f74686572"),                   # ... 'o', merged likewise
    1.0475: ([12, 56, 37], 0, "06416e64ff686572"),               # the copy goes on: "And"
    1.0480: ([12, 56, 37, 8, 2], 0, "06416e64206120ff"),  # ... "And a "
    1.0485: ([12, 56, 37, 8, 2, 54], 0, "06416e6420612074"),     # ... "And a t"
}
# the z180emu core's two leaks (0.6; the old check's times): no longer leak points on MAME, kept as clean times
OLD_TIMES = (1.040, 1.050)


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
    raise RuntimeError("an utterance never reached done")      # a timeout is never a pass (Astra, Reply 124)


def session(t_cancel):
    from ssi263.native import SSI263C
    from hosts.native_blazie import NativeBlazie
    chip = SSI263C(params={"closure_noise_lead_ms": 10.0}, out_rate=22050)
    log = []
    u = NativeBlazie(DLL, os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"), chip=chip,
                     out_rate=22050, menu=("punct_none", "numbers_toggle"), key_start=3000000, key_gap=1500000,
                     board_lowpass_hz=5000.0, on_write=lambda t, r, v: log.append((t, r, v)))
    settle = u.geti("cancel_settle")    # the host as built (and SSI263_BLAZIE_CANCEL_SETTLE, the controls' override)
    u.send(b"\x18")                     # the driver's boot (synthDrivers/blazie.py _boot)
    u.send(b"\r\x06")
    u.run(0.3)
    u.send(b"\x056V")
    u.run(0.05)
    u.turbo_between_lines = True
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
        cell = bytes(u.memory(0)[LINE:LINE + 8])
        settled = u.geti("cancel_settled")
        u._drain()
    n0 = len(log)
    u.say(RESAY)
    done(u)
    u._drain()
    u.close()
    return settle, t_cancel, spoken(log[n0:]), cell, cost, settled


def main():
    times = sorted(LEAKS) + list(OLD_TIMES)
    with Pool(min(len(times) + 1, os.cpu_count() or 1)) as p:
        res = p.map(session, [None] + times)
    ref = res[0][2]
    settles = {r[0] for r in res}
    if len(settles) != 1:
        print("FAIL the sessions ran with different cancel_settle values: %s" % sorted(settles))
        return 1
    settle = settles.pop()
    override = os.environ.get("SSI263_BLAZIE_CANCEL_SETTLE")
    if override is None and settle != SHIPPED:
        print("FAIL the shipping default is cancel_settle %d, not %d (bl_host.c)" % (settle, SHIPPED))
        return 1
    mode = "the default" if override is None else "SSI263_BLAZIE_CANCEL_SETTLE=%s" % override
    failures = 0
    for _, t, sp, cell, cost, settled in res[1:]:
        if sp[len(sp) - len(ref):] == ref and len(sp) >= len(ref):
            how = "the respoken text led by %s" % sp[:len(sp) - len(ref)] if len(sp) > len(ref) else \
                "the respoken text clean"
        else:
            how = "the respoken text begins %s (the reference %s)" % (sp[:4], ref[:3])
        info = "%s; line buffer after the cancel %s; cancel %.1f ms chip time%s" % (
            how, cell.hex(), cost * 1e3, ", settled %d" % settled if settle & 1 else "")
        clean = sp == ref
        if clean:
            print("ok   %.4f s, cancel_settle %d (%s): %s" % (t, settle, mode, info))
            continue
        failures += 1
        if t in LEAKS:
            lead, skip, buf = LEAKS[t]
            exact = sp == lead + ref[skip:] and cell.hex() == buf
            what = "the retained leak, exactly" if exact else "a leak, but NOT the retained one (%s, %s)" % (lead, buf)
        else:
            what = "a leak where none was retained"
        print("FAIL %.4f s, cancel_settle %d (%s): %s -- %s" % (t, settle, mode, info, what))
    print("lockstep cancel race (English, cancel_settle %d, %s; reference %s): %s" % (
        settle, mode, ref[:6], "all %d clean" % len(times) if not failures else "%d of %d FAILED" % (failures,
                                                                                                    len(times))))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
