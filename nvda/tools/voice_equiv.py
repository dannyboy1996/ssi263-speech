"""The gate for bl_voice (src/csrc/blazie/bl_voice.c): the real NVDA Braille Lite driver, under the stand-in NVDA,
and the C voice speak the same texts with the same settings, and every PCM byte the driver feeds its player must
equal what blv_render returns.  English and (when its files are there) Spanish; line splitting and packing, the
text clean-up, settings changes and the lead trim are all on the path.

    python voice_equiv.py            # exit 1 on the first difference
    VOICE_EQUIV_BREAK=1              # control: the C side speaks with pack flipped -- must FAIL

Number words are off on both sides: bl_voice v1 leaves numbers to the firmware (bl_voice.h).
"""
import ctypes
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], "blazie"]
src = open(os.path.join(HERE, "fake_nvda_driver_test.py"), encoding="utf-8").read()
exec(src.split("time.sleep(2.0)")[0])

ENG = os.path.dirname(drv_mod.FIRMWARE)
ARCH = "x64" if struct.calcsize("P") == 8 else "x86"
LIB = os.path.join(os.path.dirname(HERE), "dist", "blazie-lib", ARCH, "bl.dll")
CHIP = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "ssi263", "_bin", ARCH, "ssi263.dll")
BREAK = os.environ.get("VOICE_EQUIV_BREAK") == "1"

# (text, rate, pitch, tone, volume, pack)
CASES = [
    ("Hello there.", 50, 50, 7, 100, True),
    ("Custom number processing check box checked", 50, 50, 7, 100, True),
    ("This is a longer sentence, with a comma early on, that keeps going well past the fourteen word limit of "
     "the unit. Second sentence here! And a third? Yes. Short one. Another short one.", 50, 50, 7, 100, True),
    ("It’s “quoted” text – with dashes… café and an emoji \U0001F389 too.",
     50, 50, 7, 100, True),
    ("Faster, lower and quieter now.", 70, 30, 12, 60, True),
    ("This is a longer sentence, with a comma early on, that keeps going well past the fourteen word limit of "
     "the unit. Second sentence here! And a third? Yes. Short one. Another short one.", 50, 50, 7, 100, False),
    ("Room 12, $3.50 and 1,234,567 items.", 50, 50, 7, 100, True),
    ("a. b  c\td\n\ne. f", 20, 90, 3, 100, True),
    ("OK button", 100, 0, 26, 100, True),
]
CASES_ES = [("Mañana, ¿qué tal? Él está aquí. ¡Olé!", 50, 50, 7, 100, True)]


def driver_side(cases):
    global mark
    out = []
    raw = []
    orig = d._player.feed

    def feed(data, onDone=None):
        if data:
            raw.append(bytes(data))
        return orig(data, onDone)
    d._player.feed = feed
    d._numbers = False
    for text, rate, pitch, tone, volume, pack in cases:
        d._rate, d._pitch, d._tone, d._volume, d._short = rate, pitch, str(tone), volume, pack
        del raw[:]
        mark = len(notified)
        d.speak([text])
        if not wait_idle(60):
            sys.exit("the driver did not finish %r" % text)
        out.append(b"".join(raw))
    d._player.feed = orig
    return out


def c_side(cases, fw, state, encoding):
    ctypes.CDLL(CHIP)                                   # bl.dll imports it: load that copy first
    lib = ctypes.CDLL(LIB)
    lib.blv_create.restype = ctypes.c_void_p
    lib.blv_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_double, ctypes.c_int,
                               ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    lib.blv_set.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5
    lib.blv_speak.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.blv_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_short)),
                               ctypes.POINTER(ctypes.c_int)]
    lib.blv_destroy.argtypes = [ctypes.c_void_p]
    err = ctypes.create_string_buffer(256)
    v = lib.blv_create(fw.encode("mbcs"), state.encode("mbcs"), encoding, float(d._out_rate), 1, 0, err, 256)
    if not v:
        sys.exit("blv_create: %s" % err.value.decode())
    out = []
    for text, rate, pitch, tone, volume, pack in cases:
        lib.blv_set(v, rate, pitch, tone, volume, int(pack) ^ (1 if BREAK else 0))
        lib.blv_speak(v, text.encode("utf-8"))
        pcm, done, chunks = ctypes.POINTER(ctypes.c_short)(), ctypes.c_int(0), []
        while not done.value:
            n = lib.blv_render(v, ctypes.byref(pcm), ctypes.byref(done))
            if n:
                chunks.append(ctypes.string_at(pcm, 2 * n))
        out.append(b"".join(chunks))
    lib.blv_destroy(v)
    return out


def compare(label, cases, a, b):
    bad = 0
    for (text, *_), x, y in zip(cases, a, b):
        if x == y:
            print("same  %-6s %6d samples  %r" % (label, len(x) // 2, text[:40]))
            continue
        bad += 1
        k = next((i for i in range(0, min(len(x), len(y)), 2) if x[i:i + 2] != y[i:i + 2]), min(len(x), len(y)))
        print("DIFF  %-6s driver %d / C %d samples, first difference at sample %d  %r"
              % (label, len(x) // 2, len(y) // 2, k // 2, text[:40]))
    return bad


while d._unit is None:
    time.sleep(0.05)
bad = compare("en", CASES, driver_side(CASES), c_side(CASES, drv_mod.FIRMWARE, drv_mod.STATE, 0))
if os.path.isfile(drv_mod.FIRMWARE_ES) and os.path.isfile(drv_mod.STATE_ES):
    d._set_voice("blazie_es")
    bad += compare("es", CASES_ES, driver_side(CASES_ES), c_side(CASES_ES, drv_mod.FIRMWARE_ES, drv_mod.STATE_ES, 1))
d.terminate()
total = len(CASES) + len(CASES_ES)
print("%d of %d utterances byte-identical to the NVDA driver" % (total - bad, total))
sys.exit(1 if bad else 0)
