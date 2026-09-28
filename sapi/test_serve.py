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
SERVE = os.path.join(HERE, "ssi_serve.py")
RATE = 22050
REQ, RSP, CANCEL = 0x4F535034, 0x4F535052, 0x4F535043


def voices():
    out = subprocess.run([PY, SERVE, "--list"], capture_output=True, timeout=120).stdout.decode("utf-8")
    return [ln.split("\t") for ln in out.splitlines() if "\t" in ln]


class Client:
    def __init__(self):
        self.p = subprocess.Popen([PY, SERVE, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
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
print("serve: %s" % ("ok" if not bad else "%d FAILED" % bad))
sys.exit(1 if bad else 0)
