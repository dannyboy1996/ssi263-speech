"""The SAPI pipe server (ssi_serve.py) as the DLL drives it: every voice --list offers speaks voiced audio, and a
cancel mid-utterance keeps the stream in step -- the cancelled response still ends with its terminator, and the
next utterance comes out whole (as long as when spoken on its own, within 10 %).  And the same with the Braille
Lite's "Run the unit ahead" on (--run-ahead 1), against it off: the same phonemes in every Braille Lite line, from
the server's write log (SSI263_SAPI_WRITE_LOG), nothing of a cancelled line at the next one's head, and the setting
reaching the unit.

The server runs 0.7.0's Python drivers (nvda/tools/legacy_drivers.py), the ones it ran when it was the engine, not
nvda/dist's, which since 0.7.5 are the native ones; sapi/reference_drivers.py checks that first and fails the run if not.

    python sapi/test_serve.py [python.exe] [--run-ahead-only]   # default: this interpreter
    SSI263_SAPI_REF_BREAK=dist     control: the server on nvda/dist's (native) drivers -- the reference check FAILS
"""
import os
import struct
import subprocess
import sys
import threading
import time

import reference_drivers

HERE = os.path.dirname(os.path.abspath(__file__))
ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
ONLY_RUN_AHEAD = "--run-ahead-only" in sys.argv[1:]     # run_tests.py's must-fail control runs only that part
PY = ARGS[0] if ARGS else sys.executable
SERVE = os.environ.get("SSI_SERVE", os.path.join(HERE, "ssi_serve.py"))   # a staged copy: nvda/dist/sapi/ssi_serve.py
REF_ENV = reference_drivers.env()                       # every server this test starts: 0.7.0's drivers
RATE = 22050
REQ, RSP, CANCEL = 0x4F535034, 0x4F535052, 0x4F535043


def voices():
    out = subprocess.run([PY, SERVE, "--list"], capture_output=True, timeout=120, env=REF_ENV).stdout.decode("utf-8")
    return [ln.split("\t") for ln in out.splitlines() if "\t" in ln]


class Client:
    def __init__(self, extra=(), write_log=None):
        env = dict(REF_ENV)
        if write_log:
            env["SSI263_SAPI_WRITE_LOG"] = write_log      # the server's test hook: the Braille Lite's writes
        self.p = subprocess.Popen([PY, SERVE, "--serve"] + list(extra), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, env=env)
        self.seq = 0

    def exact(self, n):
        buf = b""
        while len(buf) < n:
            c = self.p.stdout.read(n - len(buf))
            if not c:
                raise EOFError("server closed the pipe")
            buf += c
        return buf

    def send(self, voice, text, rate=50, pitch=50, volume=100):
        self.seq += 1
        v, t = voice.encode("utf-8"), text.encode("utf-8")
        self.p.stdin.write(struct.pack("<IIiiiII", REQ, self.seq, rate, pitch, volume, len(v), len(t)) + v + t)
        self.p.stdin.flush()
        return self.seq

    def cancel(self, seq):
        self.p.stdin.write(struct.pack("<II", CANCEL, seq))
        self.p.stdin.flush()

    def response(self, on_chunk=None):
        magic, status = struct.unpack("<Ii", self.exact(8))
        assert magic == RSP, "desynced stream: %08X" % magic
        pcm = bytearray()
        while True:
            (frames,) = struct.unpack("<I", self.exact(4))
            if not frames:
                return status, bytes(pcm)
            assert frames < RATE * 10, "desynced stream: %d frames" % frames
            pcm += self.exact(frames * 2)
            if on_chunk:
                on_chunk(len(pcm) // 2)

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=20)


def voiced(pcm):
    import array
    a = array.array("h", pcm)
    return len(a) and (sum(x * x for x in a[::4]) / max(1, len(a[::4]))) ** 0.5 > 300


def utterances(path):
    """[[voice, run_ahead, text, phonemes]]: each Braille Lite say in the server's write log (SSI263_SAPI_WRITE_LOG),
    with the phonemes spoken from it up to the next say or cancel -- run_ahead_driver.py's reading: a register-0
    write while the control register's bit 7 is clear, its code not a pause."""
    out, cur, ctrl = [], None, 0
    with open(path, encoding="utf-8") as f:
        for ln in f:
            parts = ln.rstrip("\n").split(" ", 3)
            if parts[0] == "say":
                cur = [parts[1], parts[2], parts[3], []]
                out.append(cur)
            elif parts[0] == "cancel":
                cur = None
            elif parts[0] == "w":
                reg, val = int(parts[1]), int(parts[2])
                if reg == 3:
                    ctrl = val
                elif reg == 0 and cur is not None and not ctrl & 0x80 and val & 0x3F:
                    cur[3].append(val & 0x3F)
    return out


def line_for(lang):
    return "Hola, ¿cómo estás?" if lang == "es" else "Hello, how are you??"


# the run-ahead session's Braille Lite lines: a short one, said whole, then again after a long one cancelled mid-way
RA_TEXTS = {"en": ("OK button", "This sentence is long enough to be cancelled somewhere in the middle of it, surely."),
            "es": ("Botón aceptar", "Esta frase es bastante larga para cortarla en algún lugar de la mitad, seguro.")}


def run_ahead_session(extra, log):
    """The same requests with the setting on or off: every voice's line, then for each Braille Lite voice the short
    line, the long one cancelled 0.3 s in, and the short one again.  Returns the responses by (voice, part), the
    Braille Lite's utterances from the write log, and what each of those was."""
    heard, parts = {}, []
    c = Client(extra, log)
    try:
        for vid, _name, lang in vs:
            c.send(vid, line_for(lang))
            heard[vid, "line"] = c.response()
            if vid.startswith("blazie:"):
                parts.append((vid, "line"))
        for vid, _name, lang in vs:
            if not vid.startswith("blazie:"):
                continue
            short, long_ = RA_TEXTS["es" if lang == "es" else "en"]
            c.send(vid, short)
            heard[vid, "whole"] = c.response()
            seq = c.send(vid, long_)
            fired = []

            def chunk(n, seq=seq, fired=fired):
                if not fired and n > RATE * 0.3:
                    fired.append(n)
                    threading.Thread(target=c.cancel, args=(seq,)).start()
            heard[vid, "cut"] = c.response(chunk)
            heard[vid, "fired"] = fired
            c.send(vid, short)
            heard[vid, "after"] = c.response()
            parts += [(vid, "whole"), (vid, "cut"), (vid, "after")]
    finally:
        c.close()
    return heard, utterances(log), parts


def run_ahead_checks():
    """The Braille Lite's "Run the unit ahead" (--run-ahead 1, the dialog's; EXPERIMENTAL, off by default): every voice
    still speaks whole lines, each Braille Lite line spoken with the same phonemes as with the setting off (the timing
    may differ), a cancel mid-utterance keeps the stream in step with nothing of the cancelled text at the head of the
    next, and the setting reaches the unit: run_ahead 1 at every Braille Lite say, 0 without the flag (its control,
    SSI263_SAPI_IGNORE_RUN_AHEAD=1 in the server, must fail that)."""
    import shutil
    import tempfile
    nbad = 0
    tmp = tempfile.mkdtemp(prefix="ssi_serve_ra_")
    try:
        on, on_utts, parts = run_ahead_session(["--run-ahead", "1"], os.path.join(tmp, "on.log"))
        off, off_utts, _parts = run_ahead_session([], os.path.join(tmp, "off.log"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for vid, _name, _lang in vs:
        (s1, p1), (_s0, p0) = on[vid, "line"], off[vid, "line"]
        ok = s1 == 0 and voiced(p1)
        nbad += not ok
        print("%-4s %-22s run ahead: %.2f s of audio (off: %.2f s)" % ("ok" if ok else "FAIL", vid,
                                                                       len(p1) / 2 / RATE, len(p0) / 2 / RATE))
    # the write logs, utterance by utterance, against what was asked
    whole = len(on_utts) == len(parts) == len(off_utts) and \
        all(u[0] == vid.split(":")[1] for u, (vid, _p) in zip(on_utts, parts))
    nbad += not whole
    print("%-4s the write logs hold one say per Braille Lite request (%d with run ahead, %d without, %d asked)" % (
        "ok" if whole else "FAIL", len(on_utts), len(off_utts), len(parts)))
    if not whole:
        return nbad
    by = {p: (u, v) for p, u, v in zip(parts, on_utts, off_utts)}
    for vid in sorted({v for v, _p in parts}):
        for part in ("line", "whole", "after"):
            (u, v) = by[vid, part]
            ok = bool(u[3]) and u[3] == v[3]
            nbad += not ok
            print("%-4s %-22s %-5s the same %d phonemes with run ahead as without%s" % (
                "ok" if ok else "FAIL", vid, part, len(v[3]), "" if ok else ": %s against %s" % (u[3][:8], v[3][:8])))
        # the cancel: the cancelled response ends in step, the next line whole, nothing of the cancelled one in it
        (_sw, pw), (_sa, pa), fired = on[vid, "whole"], on[vid, "after"], on[vid, "fired"]
        same = by[vid, "after"][0][3] == by[vid, "whole"][0][3]
        ok = bool(fired) and same and voiced(pa) and abs(len(pa) - len(pw)) <= 0.1 * len(pw)
        nbad += not ok
        print("%-4s %-22s run ahead, cancel after %.2f s (%d phonemes spoken): the next line %.2f s (alone %.2f s), "
              "its phonemes %s" % ("ok" if ok else "FAIL", vid, (fired[0] if fired else 0) / RATE,
                                   len(by[vid, "cut"][0][3]), len(pa) / 2 / RATE, len(pw) / 2 / RATE,
                                   "its own" if same else "NOT its own: %s" % by[vid, "after"][0][3][:8]))
    # the setting reaches the unit, and is off by default
    ahead = sorted({u[1] for u in on_utts})
    plain = sorted({u[1] for u in off_utts})
    ok = ahead == ["1"] and plain == ["0"]
    nbad += not ok
    print("%-4s run ahead reaches the Braille Lite unit: run_ahead %s at its says with --run-ahead 1, %s without" % (
        "ok" if ok else "FAIL", "/".join(ahead), "/".join(plain)))
    # ... and is heard: its timing differs from the lockstep's (the same phonemes, above), which is all SAPI itself
    # can see (test_sapi_settings.ps1)
    for vid in sorted({v for v, _p in parts}):
        ok = on[vid, "line"][1] != off[vid, "line"][1]
        nbad += not ok
        print("%-4s %-22s run ahead changes the audio (%d samples, lockstep %d)" % (
            "ok" if ok else "FAIL", vid, len(on[vid, "line"][1]) // 2, len(off[vid, "line"][1]) // 2))
    return nbad


bad = 0
# the server on 0.7.0's Python drivers, never nvda/dist's native ones: every server here gets REF_ENV, checked first
# (reference_drivers.py; SSI263_SAPI_REF_BREAK=dist must fail here)
reference_drivers.guard(PY, SERVE, REF_ENV, "serve")
vs = voices()
print("voices: %s" % ", ".join(v[0] for v in vs))
if any(v[0].startswith("blazie:") for v in vs):
    bad += run_ahead_checks()
if ONLY_RUN_AHEAD:
    print("serve (run ahead only): %s" % ("ok" if not bad else "%d FAILED" % bad))
    sys.exit(1 if bad else 0)
c = Client()
try:
    for vid, name, lang in vs:
        t0 = time.perf_counter()
        c.send(vid, line_for(lang))
        status, pcm = c.response()
        ok = status == 0 and voiced(pcm)
        bad += not ok
        print("%-4s %-22s %.2f s of audio, %.2f s to render" % ("ok" if ok else "FAIL", vid, len(pcm) / 2 / RATE,
                                                               time.perf_counter() - t0))
    # cancel mid-utterance, then the next must be whole and the stream in step
    for vid in ("blazie:blazie", "speakout:speakout", "accentmini:mini"):
        if vid not in [v[0] for v in vs]:
            continue
        c.send(vid, "OK button")
        _s, whole = c.response()
        seq = c.send(vid, "This sentence is long enough to be cancelled somewhere in the middle of it, surely.")
        fired = []

        def chunk(n, seq=seq):
            if not fired and n > RATE * 0.3:
                fired.append(n)
                threading.Thread(target=c.cancel, args=(seq,)).start()
        _s, cut = c.response(chunk)
        c.send(vid, "OK button")
        _s, after = c.response()
        ok = fired and abs(len(after) - len(whole)) <= 0.1 * len(whole) and voiced(after)
        bad += not ok
        print("%-4s %-22s cancel after %.2f s: the cancelled one gave %.2f s; the next %.2f s (alone %.2f s)" % (
            "ok" if ok else "FAIL", vid, (fired[0] if fired else 0) / RATE, len(cut) / 2 / RATE,
            len(after) / 2 / RATE, len(whole) / 2 / RATE))
finally:
    c.close()
# the settings dialog's two Braille Lite settings reach the driver: each changes the sound
if "blazie:blazie" in [v[0] for v in vs]:
    heard = {}
    for label, extra in (("default", []), ("default again", []), ("inflection off", ["--inflection", "0"]),
                         ("whine", ["--whine", "whine"])):
        c = Client(extra)
        try:
            c.send("blazie:blazie", "Is it ready?")
            _s, heard[label] = c.response()
        finally:
            c.close()
    # the control: the same settings twice sound identical, so a difference below is the setting's
    same = heard["default again"] == heard["default"]
    bad += not same
    print("%-4s the same settings twice give identical audio: %s" % ("ok" if same else "FAIL", same))
    for label in ("inflection off", "whine"):
        ok = voiced(heard[label]) and heard[label] != heard["default"]
        bad += not ok
        print("%-4s setting %-15s changes the Braille Lite's sound: %s" % ("ok" if ok else "FAIL", label, ok))
    # ... but never the NVDA driver's open channel: SAPI sends the next text only after this stream ends, so the
    # idle whine after speech would hold every queued utterance back ~10 s.  The whine's utterance lasts as long
    # as the plain one (SSI263_SAPI_KEEP_OPEN=1 in the server: the check must fail).
    extra_s = (len(heard["whine"]) - len(heard["default"])) / 2.0 / RATE
    ok = abs(extra_s) < 0.5
    bad += not ok
    print("%-4s with the whine on, the utterance ends with its speech (%+.2f s against the plain one)" % (
        "ok" if ok else "FAIL", extra_s))
# the dialog's Accent inflection reaches its driver: the default twice is identical (the control), 0 changes it
if "accentmini:mini" in [v[0] for v in vs]:
    heard = {}
    for label, extra in (("default", []), ("default again", []), ("inflection 0", ["--accent-inflection", "0"])):
        c = Client(extra)
        try:
            c.send("accentmini:mini", "Is it ready?")
            _s, heard[label] = c.response()
        finally:
            c.close()
    ok = heard["default again"] == heard["default"] and voiced(heard["inflection 0"]) \
        and heard["inflection 0"] != heard["default"]
    bad += not ok
    print("%-4s the Accent's inflection setting changes its sound (and the default repeats exactly): %s" % (
        "ok" if ok else "FAIL", ok))
# --rate (the dialog's sample rate, declared by the DLL): the same phrase lasts the same time at every rate, so the
# sample count scales with it.  A server that ignored --rate would give equal sample counts -- half or double the
# duration -- and fail this.  Within 10 %: the Speak-Out's length itself depends on the rate (0.771 s at 22 kHz,
# 0.810 at 11 kHz, 0.796 at 44 kHz, identical run to run; the same in its NVDA driver), an open item of its own.
for vid in ("blazie:blazie", "speakout:speakout", "accentmini:mini"):
    if vid not in [v[0] for v in vs]:
        continue
    secs = {}
    for r in (11025, 22050, 44100):
        c = Client(["--rate", str(r)])
        try:
            c.send(vid, "OK button")
            _s, pcm = c.response()
        finally:
            c.close()
        secs[r] = (len(pcm) / 2 / r, len(pcm) // 2, voiced(pcm))
    base = secs[22050][0]
    ok = all(v[2] and abs(v[0] - base) <= 0.10 * base for v in secs.values())
    bad += not ok
    print("%-4s %-22s --rate: %s" % ("ok" if ok else "FAIL", vid, "; ".join(
        "%d Hz %d samples = %.3f s" % (r, v[1], v[0]) for r, v in sorted(secs.items()))))
print("serve: %s" % ("ok" if not bad else "%d FAILED" % bad))
sys.exit(1 if bad else 0)
