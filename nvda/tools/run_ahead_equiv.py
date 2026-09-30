"""The run-ahead mode (src/csrc/blazie/run_ahead.h) against today's lockstep, on the shipped path (bl.dll through
hosts/native_blazie.py, the unit booted as the NVDA driver boots it).

Every case is a session on ONE unit per mode: utterances in a row, a rate change, cancels mid-utterance.  For each
utterance it checks and reports:

  values   every SSI-263 write's register and value, in order, identical to the lockstep's, over the session cut at
           its cancels: up to a cancel, the writes both modes played agree (the unit run ahead is further on when ^X
           reaches it, so the cut itself differs); from the next utterance on they are identical again.  At the end of
           the session the two may differ by up to 4 of the unit's idle writes after its last utterance (the capture
           reaches them sooner), never in value.
  timing   say() to the first audible sample (chip ms), the first to the last spoken phoneme (the in-utterance time,
           and its change), and from the last spoken phoneme's load to "done" (busy() false).

    python run_ahead_equiv.py [--quick] [--es]      RUN_AHEAD_EQUIV_FLIP=1: one value flipped in the run-ahead log
                                                    (the control: must fail)
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
FLIP = os.environ.get("RUN_AHEAD_EQUIV_FLIP") == "1"
OFF = os.environ.get("RUN_AHEAD_EQUIV_OFF") == "1"      # the head control: "run ahead" left off, must fail the head
# say() to the first audible sample, chip time (the silence the driver's lead trim removes, and that the lockstep
# must emulate before it): run ahead at most HEAD_AHEAD_MS (the unit's own leading pause and a stop's closure); the
# lockstep at most HEAD_LOCKSTEP_MS (its x4 reading of a line: 251 ms for the longest line here)
HEAD_AHEAD_MS = 100.0
HEAD_LOCKSTEP_MS = 300.0
RATE = 22050
BLOCK = 0.03
THRESH = 0.003

# a case: a list of steps ("say", [lines]) | ("cancel", [lines], seconds in) | ("send", bytes)
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
]
ES = [
    [("say", ["Hola."]), ("say", ["Aceptar botón"]), ("say", ["Velocidad 50"])],
    [("say", ["El rápido zorro marrón salta sobre el perro perezoso,",
              "y luego corre muy lejos hacia el bosque hoy."])],
    [("cancel", ["Número 1234567, y 263."], 0.6), ("say", ["Uno, dos, tres, cuatro."])],
    [("say", ["¡Qué sorpresa! Mañana llueve."]), ("say", ["Archivo, nuevo, abrir, guardar como."])],
]
CASES = ES if SPANISH else EN
if QUICK:
    CASES = CASES[:5] if not SPANISH else CASES[:3]


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
    u.run_ahead = 1 if run_ahead and not OFF else 0
    marks = []
    for step in case:
        if step[0] == "send":
            u.send(step[1])
            u.run(0.02)
            continue
        u._drain()
        n0 = len(log)
        t_say = chip.time
        u.say(step[1])
        first = None
        limit = step[2] if step[0] == "cancel" else 30.0
        while chip.time - t_say < limit:
            y = u.run(BLOCK)
            if first is None:
                for k, v in enumerate(y):
                    if v > THRESH or v < -THRESH:
                        first = chip.time - (len(y) - k) / RATE
                        break
            u._drain()
            if step[0] == "say" and not u.busy():
                break
        t_done = chip.time
        u._drain()
        n_cut = len(log)
        if step[0] == "cancel":
            u.cancel()
            u._drain()
        marks.append(dict(kind=step[0], text=step[1][0], n0=n0, n_cut=n_cut, n1=len(log), t_say=t_say, first=first, done=t_done))
        u.run(0.3)                      # a moment of silence between utterances (lockstep in both modes)
        u._drain()
    u.close()
    return log, marks


def spoken(log, a, b):
    """(time, value) of the spoken loads in log[a:b] (R3 bit 7 clear when loaded; PA = code 00 is not speech)."""
    out, r3 = [], None
    for t, r, v in log[a:b]:
        if r == 3:
            r3 = v
        if r == 0 and (r3 is None or not r3 & 0x80) and v & 0x3F:
            out.append((t, v))
    return out


def values(log, a, b):
    return [(r, v) for _, r, v in log[a:b]]


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
for ci, case in enumerate(CASES):
    ref, rm = session(False, case)
    ahd, am = session(True, case)
    if FLIP and ci == 0:                # the control: one value changed where run-ahead wrote it
        k = am[0]["n0"] + (am[0]["n1"] - am[0]["n0"]) // 2
        t, r, v = ahd[k]
        ahd[k] = (t, r, v ^ 0x01)
    for bi, ((a0, a1, cut), (b0, b1, _)) in enumerate(zip(blocks(rm, len(ref)), blocks(am, len(ahd)))):
        va, vb = values(ref, a0, a1), values(ahd, b0, b1)
        n = min(len(va), len(vb))
        k = next((i for i in range(n) if va[i] != vb[i]), None)
        # both cut (a cancel), or the session's end (the unit's idle writes after the last utterance, which the
        # run-ahead capture reaches sooner): the shorter must be a prefix of the longer
        ok = k is None and (len(va) == len(vb) or cut or abs(len(vb) - len(va)) <= 4)
        what = "played before the cancel" if cut else "to the end of the session"
        how = ("%d/%d writes %s, identical over %d" % (len(va), len(vb), what, n)) if ok else (
            "write values DIFFER at write %s of %d/%d (%s)" % (k if k is not None else n, len(va), len(vb), what))
        failures += not ok
        print("%-4s case %d block %d: %s" % ("ok" if ok else "FAIL", ci + 1, bi + 1, how))
    for ui, (a, b) in enumerate(zip(rm, am)):
        sa, sb = spoken(ref, a["n0"], a["n_cut"]), spoken(ahd, b["n0"], b["n_cut"])
        if a["kind"] == "say" and sa and sb and a["first"] is not None and b["first"] is not None:
            span_a, span_b = sa[-1][0] - sa[0][0], sb[-1][0] - sb[0][0]
            head_a, head_b = (a["first"] - a["t_say"]) * 1e3, (b["first"] - b["t_say"]) * 1e3
            ok = head_a <= HEAD_LOCKSTEP_MS and head_b <= HEAD_AHEAD_MS
            failures += not ok
            print("%-4s %d.%d %-26.26s head %5.1f -> %5.1f ms | first to last phoneme %6.3f -> %6.3f s (%+6.1f ms) | "
                  "done %4.0f -> %4.0f ms after the last phoneme's load"
                  % ("ok" if ok else "FAIL", ci + 1, ui + 1, a["text"], head_a, head_b, span_a, span_b,
                     (span_b - span_a) * 1e3, (a["done"] - sa[-1][0]) * 1e3, (b["done"] - sb[-1][0]) * 1e3))
    sys.stdout.flush()
print("run ahead vs lockstep (%s): %s" % ("Spanish" if SPANISH else "English",
                                         "all identical" if not failures else "%d FAILED" % failures))
sys.exit(1 if failures else 0)
