"""The run-ahead mode (src/csrc/blazie/run_ahead.h; EXPERIMENTAL, opt-in) against today's lockstep, on the shipped path
(bl.dll through hosts/native_blazie.py, the unit booted as the NVDA driver boots it).

Every case is a session on ONE unit per mode: utterances in a row, a rate change, cancels mid-utterance, a second say
given while the first still speaks, a setting sent mid-utterance.  It checks and reports (Astra, Reply 107):

  values     every SSI-263 write's register and value, in order, over the session cut at its cancels.  A block that
             runs to the end of the session must be WHOLE-PHRASE identical, except that one mode may have written more
             at the very end -- and only idle writes (no phoneme load but a pause; R3 tracked), with the spoken loads
             identical.  A block cut by a cancel can only match up to the cut (the unit run ahead is further on when ^X
             reaches it): a CANCEL-PREFIX match, weaker, and counted apart.
  done       every say must end by busy() going false within its limit (never "done" at the limit), with spoken
             loads and a first audible sample in both modes.
  after done the continuation with no new input (0.3 s): no audible sample and no spoken load; then the next
             utterance.  Run ahead: the writes it replayed must be the ones it captured, in order; the counts of
             writes captured, replayed at done and after, and phonemes loaded are reported.
  timing     say() to the first audible sample (chip ms), first to last spoken phoneme, last spoken load to done.

    python run_ahead_equiv.py [--quick] [--es] [--cases=1,3]
The must-fail controls (nvda/tools/run_tests.py names the failure each must show):
    RUN_AHEAD_EQUIV_FLIP=1         one value flipped in the run-ahead log
    RUN_AHEAD_EQUIV_FLIP=send_mid  a spoken load flipped inside the classified window of a setting sent mid-line
    RUN_AHEAD_EQUIV_OFF=1          run ahead left off: the head limit fails
    RUN_AHEAD_EQUIV_NEVER=1        busy() never false in the run-ahead session: every say NEVER DONE
    RUN_AHEAD_EQUIV_MUTE=1         the run-ahead audio silenced: NO FIRST AUDIO
    RUN_AHEAD_EQUIV_DROP_SPOKEN=1  the run-ahead log cut before its last spoken load: a spoken load in the suffix
    RUN_AHEAD_EQUIV_OLD_SUFFIX=1   the 0.7 draft's end rule (up to 4 writes of any content): Astra's case accepted
    RUN_AHEAD_EQUIV_BREAK=completion  the host's 0.7-draft completion (run_ahead_break): AUDIO AFTER DONE
    RUN_AHEAD_EQUIV_SAY_LIMIT=s    a say's limit in chip seconds (default 30)
The texts are this file's own, none from the MASTER sessions (src/holdout_lines.txt).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [REPO, os.path.join(REPO, "src")]
from ssi263.native import SSI263C                 # noqa: E402
from hosts.native_blazie import NativeBlazie       # noqa: E402

ENG = os.path.join(REPO, "nvda", "dist", "blazie-build", "synthDrivers", "_ssi263_blazie")
DLL = os.path.join(REPO, "nvda", "dist", "blazie-lib", "x64" if sys.maxsize > 2 ** 32 else "x86", "bl.dll")
SPANISH = "--es" in sys.argv
QUICK = "--quick" in sys.argv
ENV = os.environ.get
FLIP = ENV("RUN_AHEAD_EQUIV_FLIP") == "1"
FLIP_MID = ENV("RUN_AHEAD_EQUIV_FLIP") == "send_mid"
OFF = ENV("RUN_AHEAD_EQUIV_OFF") == "1"
NEVER = ENV("RUN_AHEAD_EQUIV_NEVER") == "1"
MUTE = ENV("RUN_AHEAD_EQUIV_MUTE") == "1"
DROP_SPOKEN = ENV("RUN_AHEAD_EQUIV_DROP_SPOKEN") == "1"
OLD_SUFFIX = ENV("RUN_AHEAD_EQUIV_OLD_SUFFIX") == "1"
BREAK = {"": 0, "completion": 1}[ENV("RUN_AHEAD_EQUIV_BREAK", "")]
SAY_LIMIT = float(ENV("RUN_AHEAD_EQUIV_SAY_LIMIT", "30"))
# say() to the first audible sample (chip time; the silence the driver's lead trim removes, and that the lockstep must
# emulate before it): run ahead at most HEAD_AHEAD_MS (the unit's own leading pause and a stop's closure); the
# lockstep at most HEAD_LOCKSTEP_MS (its x4 reading of a line: 251 ms for the longest line here)
HEAD_AHEAD_MS = 100.0
HEAD_LOCKSTEP_MS = 300.0
RATE = 22050
BLOCK = 0.03
POST_S = 0.3                            # the continuation after done, no new input
THRESH = 0.003

# a case: steps ("say", [lines]) | ("cancel", [lines], seconds in) | ("send", bytes)
#   | ("overlap", [lines], seconds in, [lines]): a second say while the first speaks, no cancel
#   | ("send_mid", [lines], seconds in, bytes): a setting sent while the line speaks
EN = [
    [("say", ["Hello."]), ("say", ["OK button"]), ("say", ["Select synthesizer dialog"])],
    [("say", ["Custom number processing check box checked"]), ("say", ["Is this a question?"])],
    [("say", ["Settings.", "Voice settings. Synthesizer: combo box. Blazie. Collapsed."])],
    [("say", ["The quick brown fox jumps over the lazy dog,", "and then it runs far away into the forest today."])],
    [("cancel", ["The meeting starts at 3:45 PM on Tuesday, March 4th."], 0.8), ("say", ["Alt plus f"]),
     ("say", ["Wow! That was loud."])],
    [("send", b"\x0515E"), ("say", ["Number 1234567, and 263."]), ("send", b"\x0511E"), ("say", ["t"])],
    [("cancel", ["Error: the file could not be found. Try again later."], 0.3),
     ("cancel", ["Press enter to continue, or escape to cancel."], 1.2), ("say", ["$25.99 plus tax"])],
    [("say", ["A B C D E F G"]), ("say", ["www.example.com slash index dot html"])],
    [("overlap", ["The first line is still speaking"], 0.5, ["when the second one arrives."]),
     ("send_mid", ["A setting arrives in the middle of this line."], 0.6, b"\x0514E"), ("say", ["Done."])],
]
ES = [
    [("say", ["Hola."]), ("say", ["Aceptar botón"]), ("say", ["Velocidad 50"])],
    [("say", ["El rápido zorro marrón salta sobre el perro perezoso,",
              "y luego corre muy lejos hacia el bosque hoy."])],
    [("cancel", ["Número 1234567, y 263."], 0.6), ("say", ["Uno, dos, tres, cuatro."])],
    [("say", ["¡Qué sorpresa! Mañana llueve."]), ("say", ["Archivo, nuevo, abrir, guardar como."])],
    [("overlap", ["La primera línea todavía habla"], 0.5, ["cuando llega la segunda."])],
]
CASES = ES if SPANISH else EN
if QUICK:
    CASES = CASES[:5] if not SPANISH else CASES[:3]
PICK = [a for a in sys.argv if a.startswith("--cases=")]
NUMBERS = [int(x) for x in PICK[0][8:].split(",")] if PICK else list(range(1, len(CASES) + 1))
if PICK:
    CASES = [(ES if SPANISH else EN)[k - 1] for k in NUMBERS]


def session(run_ahead, case):
    if SPANISH:
        fw, st, enc = os.path.join(ENG, "BL2SPA.BNS"), os.path.join(ENG, "bl2spa_fresh.state"), "cp850"
    else:
        fw, st, enc = os.path.join(ENG, "BL2ENG.BNS"), os.path.join(ENG, "bl2_2003_warm.state"), "latin-1"
    chip = SSI263C(params={"closure_noise_lead_ms": 10.0}, out_rate=RATE)
    log = []
    u = NativeBlazie(DLL, fw, st, chip=chip, out_rate=RATE, menu=("punct_none", "numbers_toggle"), key_start=3000000,
                     key_gap=1500000, board_lowpass_hz=5000.0, on_write=lambda t, r, v: log.append((t, r, v)))
    u.encoding = enc
    u.send(b"\x18")                     # the driver's boot (synthDrivers/blazie.py _boot)
    u.send(b"\r\x06")
    u.run(0.3)
    u.send(b"\x056V")
    u.run(0.05)
    u.turbo_between_lines = True        # the driver's default ("short pauses")
    ahead = run_ahead and not OFF
    u.run_ahead = 1 if ahead else 0
    if ahead:
        u.run_ahead_break = BREAK
    marks = []
    for step in case:
        kind = step[0]
        if kind == "send":
            u.send(step[1])
            u.run(0.02)
            continue
        u._drain()
        n0 = len(log)
        t_say = chip.time
        u.say(step[1])
        first, done = None, None
        pending = step[3] if kind in ("overlap", "send_mid") else None
        limit = step[2] if kind == "cancel" else SAY_LIMIT
        while chip.time - t_say < limit - 1e-9:
            y = u.run(BLOCK)
            if MUTE and run_ahead:
                y = [0.0] * len(y)
            if first is None:
                for k, v in enumerate(y):
                    if v > THRESH or v < -THRESH:
                        first = chip.time - (len(y) - k) / RATE
                        break
            u._drain()
            if pending is not None:
                if chip.time - t_say >= step[2]:
                    (u.say if kind == "overlap" else u.send)(pending)
                    pending = None
                    u._drain()
                continue
            if kind != "cancel" and not (NEVER and run_ahead) and not u.busy():
                done = chip.time            # observed: busy() went false
                break
        u._drain()
        n_cut = len(log)
        info = {}
        if ahead:
            info = dict(state_done=u.run_ahead_state, captured=u.geti("run_ahead_captured"),
                        replayed_done=u.geti("run_ahead_played"))
        if kind == "cancel":
            u.cancel()
            u._drain()
        n_done = len(log)
        y = u.run(POST_S)                   # the continuation: no new input
        if MUTE and run_ahead:
            y = [0.0] * len(y)
        u._drain()
        loud = [k for k, v in enumerate(y) if v > THRESH or v < -THRESH]
        if ahead:
            info.update(state_post=u.run_ahead_state, replayed_post=u.geti("run_ahead_played"),
                        script=[(r, v) for _, _, r, v in u.script()])
        marks.append(dict(kind=kind, text=step[1][0], n0=n0, n_cut=n_cut, n_done=n_done, n_post=len(log), t_say=t_say,
                          first=first, done=done, post_loud=len(loud), post_max=max((abs(v) for v in y), default=0.0),
                          post_last_ms=(loud[-1] / RATE * 1e3 if loud else 0.0), **info))
    u.close()
    return log, marks


def r3_before(log, n):
    r3 = 0
    for _, r, v in log[:n]:
        if r == 3:
            r3 = v
    return r3


def spoken(log, a, b, r3=None):
    """(time, value) of the spoken loads in log[a:b] (R3 bit 7 clear when loaded; PA = code 00 is not speech)."""
    out = []
    if r3 is None:
        r3 = r3_before(log, a)
    for t, r, v in log[a:b]:
        if r == 3:
            r3 = v
        if r == 0 and not r3 & 0x80 and v & 0x3F:
            out.append((t, v))
    return out


def values(log, a, b):
    return [(r, v) for _, r, v in log[a:b]]


def idle_writes(vals, r3):
    """None when every write is idle -- no phoneme load but a pause (code 00), with the R3 each write meets -- else
    the first that is not"""
    for i, (r, v) in enumerate(vals):
        if r == 3:
            r3 = v
        if r == 0 and not r3 & 0x80 and v & 0x3F:
            return i, v
    return None


def suffix_ok(va, vb, r3):
    """the end of a session: one list is the other plus a suffix, and that suffix is idle writes (Astra, Reply 107:
    the 0.7 draft accepted up to 4 writes of any content -- a missing final spoken load among them).  (ok, why)"""
    n = min(len(va), len(vb))
    k = next((i for i in range(n) if va[i] != vb[i]), None)
    if k is not None:
        return False, "write values DIFFER at write %d" % k
    if OLD_SUFFIX:
        return abs(len(va) - len(vb)) <= 4, "more than 4 extra writes"
    longer = va if len(va) > len(vb) else vb
    for r, v in longer[:n]:
        if r == 3:
            r3 = v
    bad = idle_writes(longer[n:], r3)
    if bad is not None:
        return False, "a SPOKEN LOAD (%02X) in the %d-write suffix that only %s has" % (
            bad[1], len(longer) - n, "the lockstep" if longer is va else "run ahead")
    return True, ""


def blocks(marks, n_log):
    """(start, end, cancelled): the session cut at its cancels -- a block runs from an utterance's say to the next
    cancel's cut (what was played before it), or to the end of the session"""
    out, start = [], marks[0]["n0"]
    for i, m in enumerate(marks):
        if m["kind"] == "cancel":
            out.append((start, m["n_cut"], True))
            start = marks[i + 1]["n0"] if i + 1 < len(marks) else None
    if start is not None:
        out.append((start, n_log, False))
    return out


failures = 0
# the end rule itself, on Astra's case (Reply 107, run-ahead-tail-predicate.json): the lockstep's writes end with a
# spoken load (R3 = 1Fh: 81h, 82h are codes 1 and 2) that run ahead lacks.  It must be refused; an idle suffix taken.
ok_miss = not suffix_ok([(3, 0x1F), (0, 0x81), (0, 0x82)], [(3, 0x1F), (0, 0x81)], 0)[0]
ok_idle = suffix_ok([(3, 0x1F), (0, 0x81), (0, 0xC0), (1, 0x45)], [(3, 0x1F), (0, 0x81)], 0)[0]
failures += not (ok_miss and ok_idle)
print("%-4s the end rule: a missing final spoken load %s (Astra's case), an idle suffix %s"
      % ("ok" if ok_miss and ok_idle else "FAIL", "refused" if ok_miss else "ACCEPTED",
         "accepted" if ok_idle else "REFUSED"))
if "--selftest" in sys.argv:
    print("run ahead vs lockstep (end rule): %s" % ("ok" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)

whole = prefix = utterances = 0
for ci, case in zip(NUMBERS, CASES):
    ref, rm = session(False, case)
    ahd, am = session(True, case)
    if FLIP and ci == NUMBERS[0]:       # the control: one value changed where run-ahead wrote it
        k = am[0]["n0"] + (am[0]["n_post"] - am[0]["n0"]) // 2
        t, r, v = ahd[k]
        ahd[k] = (t, r, v ^ 0x01)
    for m in am if FLIP_MID else ():    # the control: a spoken load changed inside the classified window
        if m["kind"] == "send_mid":
            t, v = spoken(ahd, m["n0"], m["n_cut"])[3]
            k = next(i for i in range(m["n0"], m["n_cut"]) if ahd[i][0] == t and ahd[i][1] == 0)
            ahd[k] = (t, 0, v ^ 0x01)
    if DROP_SPOKEN and ci == NUMBERS[0]:   # the control: run ahead's log cut before its last spoken load
        last = spoken(ahd, am[-1]["n0"], len(ahd))[-1][0]
        k = max(i for i, (t, r, v) in enumerate(ahd) if t == last and r == 0)
        del ahd[k:]
    if len(rm) != len(am):
        failures += 1
        print("FAIL case %d: %d utterances in the lockstep, %d run ahead" % (ci, len(rm), len(am)))
        continue
    ba, bb = blocks(rm, len(ref)), blocks(am, len(ahd))
    if len(ba) != len(bb):
        failures += 1
        print("FAIL case %d: %d blocks in the lockstep, %d run ahead" % (ci, len(ba), len(bb)))
        continue
    for bi, ((a0, a1, cut), (b0, b1, _)) in enumerate(zip(ba, bb)):
        va, vb = values(ref, a0, a1), values(ahd, b0, b1)
        r3 = r3_before(ref, a0)
        # classified: a setting sent mid-utterance reaches the lockstep's unit behind the line, in time for its idle
        # writes after the speech; run ahead holds it until the speech is over (bl_host.c hold()), so those idle writes
        # keep the old value.  Only register writes (never a load) inside that utterance's window, the same register
        classified = []
        for mi, m in enumerate(rm):
            if m["kind"] != "send_mid" or not a0 <= m["n0"] < a1:
                continue
            end = (rm[mi + 1]["n0"] if mi + 1 < len(rm) else a1) - a0
            for i in range(m["n0"] - a0, min(end, len(va), len(vb))):
                if va[i] != vb[i] and va[i][0] == vb[i][0] != 0:
                    classified.append((i, va[i], vb[i]))
                    vb[i] = va[i]
        if cut:
            n = min(len(va), len(vb))
            k = next((i for i in range(n) if va[i] != vb[i]), None)
            ok, why = k is None, "write values DIFFER at write %s" % k
            how = "%d/%d writes played before the cancel, a CANCEL-PREFIX match over %d" % (len(va), len(vb), n)
            prefix += ok
        else:
            sa, sb = spoken(ref, a0, a1), spoken(ahd, b0, b1)
            ok, why = suffix_ok(va, vb, r3)
            if ok and [v for _, v in sa] != [v for _, v in sb]:
                ok, why = False, "SPOKEN LOADS DIFFER: %d in the lockstep, %d run ahead" % (len(sa), len(sb))
            how = "%d/%d writes to the end of the session, WHOLE-PHRASE identical%s" % (
                len(va), len(vb), "" if len(va) == len(vb) else " (+%d idle writes at the end)" % abs(len(va) - len(vb)))
            if classified:
                regs = sorted(set("R%d %02X->%02X" % (x[1][0], x[1][1], x[2][1]) for x in classified))
                how = ("%d/%d writes to the end of the session, identical but for %d CLASSIFIED idle writes (%s: a "
                       "setting sent mid-utterance, held until the speech was over)"
                       % (len(va), len(vb), len(classified), ", ".join(regs)))
            whole += ok and not classified
        failures += not ok
        print("%-4s case %d block %d: %s" % ("ok" if ok else "FAIL", ci, bi + 1, how if ok else why))
    for ui, (a, b) in enumerate(zip(rm, am)):
        if a["kind"] == "cancel":
            continue
        utterances += 1
        name = "%d.%d %-26.26s" % (ci, ui + 1, a["text"])
        problems = []
        sa, sb = spoken(ref, a["n0"], a["n_cut"]), spoken(ahd, b["n0"], b["n_cut"])
        for who, m in (("lockstep", a), ("run ahead", b)):
            if m["done"] is None:
                problems.append("NEVER DONE (%s, %.0f s)" % (who, SAY_LIMIT))
            if m["first"] is None:
                problems.append("NO FIRST AUDIO (%s)" % who)
            if m["post_loud"]:
                problems.append("AUDIO AFTER DONE (%s): %d samples, max %.4f, up to %.1f ms" % (
                    who, m["post_loud"], m["post_max"], m["post_last_ms"]))
        if not sa or not sb:
            problems.append("NO SPOKEN LOADS (%d lockstep, %d run ahead)" % (len(sa), len(sb)))
        for who, log, m in (("lockstep", ref, a), ("run ahead", ahd, b)):
            bad = idle_writes(values(log, m["n_cut"], m["n_post"]), r3_before(log, m["n_cut"]))
            if bad is not None and m["done"] is not None:
                problems.append("SPEECH AFTER DONE (%s): load %02X" % (who, bad[1]))
        if "script" in b:                   # the last capture of the step (after an overlap: the second say's)
            want, win = b["script"][:b["replayed_post"]], values(ahd, b["n0"], b["n_post"])
            if not any(win[s:s + len(want)] == want for s in range(len(win) - len(want) + 1)):
                problems.append("REPLAY DIFFERS FROM CAPTURE (%d writes replayed)" % len(want))
        if problems:
            failures += 1
            print("FAIL %s %s" % (name, "; ".join(problems)))
            continue
        span_a, span_b = sa[-1][0] - sa[0][0], sb[-1][0] - sb[0][0]
        head_a, head_b = (a["first"] - a["t_say"]) * 1e3, (b["first"] - b["t_say"]) * 1e3
        ok = head_a <= HEAD_LOCKSTEP_MS and head_b <= HEAD_AHEAD_MS
        failures += not ok
        extra = ""
        if "script" in b:
            extra = " | captured %d, replayed %d at done (%s), %d after (%s), %d spoken loaded" % (
                b["captured"], b["replayed_done"], b["state_done"], b["replayed_post"], b["state_post"], len(sb))
        print("%-4s %s head %5.1f -> %5.1f ms | first to last phoneme %6.3f -> %6.3f s (%+6.1f ms) | "
              "done %4.0f -> %4.0f ms after the last phoneme's load%s"
              % ("ok" if ok else "FAIL", name, head_a, head_b, span_a, span_b, (span_b - span_a) * 1e3,
                 (a["done"] - sa[-1][0]) * 1e3, (b["done"] - sb[-1][0]) * 1e3, extra))
    sys.stdout.flush()
lang = "Spanish" if SPANISH else "English"
print("run ahead vs lockstep (%s): %d utterances, %d blocks whole-phrase identical, %d cancel-prefix matches (weaker)%s"
      % (lang, utterances, whole, prefix, "" if not failures else ", %d FAILED" % failures))
sys.exit(1 if failures else 0)
