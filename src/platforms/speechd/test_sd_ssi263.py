"""sd_ssi263 against speech-dispatcher's module protocol, with every message's audio checked byte for byte.

The harness plays the server: it starts the module, speaks the protocol on its stdin and stdout, and decodes the 705
AUDIO blocks.  A reference bl_voice in libssi263speech.so (ctypes), driven through the same calls, must give the
same PCM:

  speak    a message, rendered whole
  stop     STOP after 5 audio blocks, then a new message: the reference renders the same number of blocks, cancels,
           and speaks the new one -- the module must have cancelled the unit, or its leftovers reach the next message
  set      rate, pitch and volume from SET (SSIP -100..100 -> the driver's 0..100)
  spanish  language=es switches to the Spanish unit (when its files are there)
  key      KEY space speaks "space"

    python3 test_sd_ssi263.py <sd_ssi263> <libssi263speech.so> <data folder>
    SD_SSI263_TEST_NO_CANCEL=1 in the environment: the module leaves the unit uncancelled -- "stop" must FAIL
"""
import ctypes
import os
import subprocess
import sys

MODULE, LIB, DATA = sys.argv[1:4]
LONG = ("This is a long message for the stop test, with a comma or two, that keeps going well past the moment "
        "the harness says stop. It has a second sentence as well.")
lib = ctypes.CDLL(LIB)
lib.blv_create.restype = ctypes.c_void_p
lib.blv_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_double, ctypes.c_int,
                           ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
lib.blv_set.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5
lib.blv_speak.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.blv_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_short)),
                           ctypes.POINTER(ctypes.c_int)]
lib.blv_cancel.argtypes = [ctypes.c_void_p]
lib.blv_destroy.argtypes = [ctypes.c_void_p]


# ---- the reference -----------------------------------------------------------------------------------------------
class Ref:
    def __init__(self, spanish=False):
        fw, st = (("BL2SPA.BNS", "bl2spa_fresh.state") if spanish else ("BL2ENG.BNS", "bl2_2003_warm.state"))
        err = ctypes.create_string_buffer(256)
        self.v = lib.blv_create(os.path.join(DATA, fw).encode(), os.path.join(DATA, st).encode(), int(spanish),
                                22050.0, 1, 0, err, 256)
        assert self.v, err.value

    def say(self, text, rate=0, pitch=0, volume=100, blocks=None):
        """PCM of the message; with `blocks`, only that many non-empty blocks, then a cancel."""
        to100 = lambda s: max(0, min(100, (s + 100) // 2))      # noqa: E731
        lib.blv_set(self.v, to100(rate), to100(pitch), 7, to100(volume), 1)
        lib.blv_speak(self.v, text.encode("utf-8"))
        pcm, done, out, n_blocks = ctypes.POINTER(ctypes.c_short)(), ctypes.c_int(0), [], 0
        while not done.value:
            n = lib.blv_render(self.v, ctypes.byref(pcm), ctypes.byref(done))
            if n:
                out.append(ctypes.string_at(pcm, 2 * n))
                n_blocks += 1
                if blocks is not None and n_blocks == blocks and not done.value:
                    lib.blv_cancel(self.v)
                    break
        return b"".join(out)


# ---- the server's side of the protocol --------------------------------------------------------------------------
class Module:
    def __init__(self):
        env = dict(os.environ, SSI263_DATADIR=DATA)
        self.p = subprocess.Popen([MODULE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)

    def send(self, *lines):
        self.p.stdin.write(("\n".join(lines) + "\n").encode("utf-8"))
        self.p.stdin.flush()

    def line(self):
        s = self.p.stdout.readline()
        if not s:
            raise RuntimeError("the module exited")
        return s.rstrip(b"\n")

    def reply(self):
        """Lines up to the final one (NNN-... continues, NNN ... ends)."""
        out = []
        while True:
            s = self.line()
            out.append(s)
            if len(s) > 3 and s[3:4] == b" ":
                return out

    def audio_block(self, first):
        """A 705 block whose first header line was already read: returns its PCM."""
        n = None
        s = first
        while not s.startswith(b"705-AUDIO"):
            if s.startswith(b"705-num_samples="):
                n = int(s.split(b"=")[1])
            s = self.line()
        data = s[len(b"705-AUDIO") + 1:] + b"\n"   # the payload starts after the NUL, and may contain no raw '\n'
        while not data.endswith(b"\n705 AUDIO\n"):
            data += self.p.stdout.readline()
        raw, out, i = data[:-len(b"\n705 AUDIO\n")], bytearray(), 0
        while i < len(raw):
            if raw[i] == 0x7D:
                out.append(raw[i + 1] ^ 0x20)
                i += 2
            else:
                out.append(raw[i])
                i += 1
        assert len(out) == 2 * n, (len(out), n)
        return bytes(out)

    def speak(self, text, cmd="SPEAK", stop_after=None):
        """Returns (pcm, the event that ended it, blocks)."""
        self.send(cmd)
        assert self.line() == b"202 OK RECEIVING MESSAGE"
        self.send(text, ".")
        assert self.line() == b"200 OK SPEAKING"
        pcm, blocks = [], 0
        while True:
            s = self.line()
            if s == b"701 BEGIN":
                continue
            if s.startswith(b"705-"):
                pcm.append(self.audio_block(s))
                blocks += 1
                if stop_after is not None and blocks == stop_after:
                    self.send("STOP")
                continue
            if s in (b"702 END", b"703 STOP"):
                return b"".join(pcm), s.decode(), blocks
            raise RuntimeError("unexpected %r" % s)

    def set(self, **kw):
        self.send("SET")
        assert self.line() == b"202 OK RECEIVING MESSAGE"
        self.send(*["%s=%s" % kv for kv in kw.items()] + ["."])
        assert self.line() == b"203 OK SETTINGS RECEIVED"


def same(label, got, want):
    ok = got == want
    k = next((i for i in range(0, min(len(got), len(want)), 2) if got[i:i + 2] != want[i:i + 2]),
             min(len(got), len(want)))
    print("%-8s module %6d samples, reference %6d: %s" % (label, len(got) // 2, len(want) // 2,
                                                        "identical" if ok else "DIFFER at sample %d" % (k // 2)))
    return ok


m = Module()
m.send("INIT")
init = m.reply()
assert init[-1] == b"299 OK LOADED SUCCESSFULLY", init
m.send("LIST VOICES")
voices = [v.decode("utf-8") for v in m.reply()]
print("voices:", "; ".join(v[4:].replace("\t", " / ") for v in voices[:-1]))
ref = Ref()
results = []

pcm, ev, _ = m.speak("<speak>Hello from Linux &amp; speech-dispatcher.</speak>")
results.append(same("speak", pcm, ref.say("Hello from Linux & speech-dispatcher.")) and ev == "702 END")

pcm1, ev1, blocks = m.speak(LONG, stop_after=5)
pcm2, ev2, _ = m.speak("And the next message.")
first = ref.say(LONG, blocks=blocks)
results.append(ev1 == "703 STOP" and same("stop", pcm1, first))
results.append(same("after", pcm2, ref.say("And the next message.")) and ev2 == "702 END")

m.set(rate=40, pitch=-30, volume=-20)
pcm, ev, _ = m.speak("Faster, lower and quieter.")
results.append(same("set", pcm, ref.say("Faster, lower and quieter.", rate=40, pitch=-30, volume=-20)))
m.set(rate=0, pitch=0, volume=100)

pcm, ev, _ = m.speak("space", cmd="KEY")
results.append(same("key", pcm, ref.say("space")))

if os.path.isfile(os.path.join(DATA, "BL2SPA.BNS")):
    m.set(language="es")
    pcm, ev, _ = m.speak("Mañana, ¿qué tal?")
    results.append(same("spanish", pcm, Ref(spanish=True).say("Mañana, ¿qué tal?")))

m.send("QUIT")
m.p.wait(timeout=10)
print("%d of %d checks passed (stop after %d blocks)" % (sum(results), len(results), blocks))
sys.exit(0 if all(results) else 1)
