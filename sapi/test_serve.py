"""The SAPI pipe server (ssi_serve.py) as the DLL drives it: every voice --list offers speaks voiced audio, and a
cancel mid-utterance keeps the stream in step -- the cancelled response still ends with its terminator, and the
next utterance comes out whole (as long as when spoken on its own, within 10 %).

    python sapi/test_serve.py [python.exe]        # default: this interpreter, against nvda/dist's built add-ons
"""
import os
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.argv[1] if len(sys.argv) > 1 else sys.executable
SERVE = os.environ.get("SSI_SERVE", os.path.join(HERE, "ssi_serve.py"))   # a staged copy: nvda/dist/sapi/ssi_serve.py
RATE = 22050
REQ, RSP, CANCEL = 0x4F535034, 0x4F535052, 0x4F535043


def voices():
    out = subprocess.run([PY, SERVE, "--list"], capture_output=True, timeout=120).stdout.decode("utf-8")
    return [ln.split("\t") for ln in out.splitlines() if "\t" in ln]


class Client:
    def __init__(self, extra=()):
        self.p = subprocess.Popen([PY, SERVE, "--serve"] + list(extra), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
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


bad = 0
vs = voices()
print("voices: %s" % ", ".join(v[0] for v in vs))
c = Client()
try:
    for vid, name, lang in vs:
        text = "Hola, ¿cómo estás?" if lang == "es" else "Hello, how are you??"
        t0 = time.perf_counter()
        c.send(vid, text)
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
