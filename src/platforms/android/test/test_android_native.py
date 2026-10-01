"""The Android app's native part, proven without a phone: test_android_native.c (the app's front end, ssa_engine.c and
ssa_map.c, on the same chip, boards, hosts and voices), every case's PCM compared by hash.

- The Braille Lite: against bl_voice driven directly the way the speech-dispatcher module drives it
  (src/platforms/speechd/sd_ssi263.c: SSIP -100..100 through its to100), from the desktop library the NVDA add-on
  and Linux ship (bl.dll + ssi263.dll on Windows, libssi263speech.so on Linux), loaded with ctypes.
- The Aicom Accent SA (the built-in voice): against the NVDA Accent add-on's own driver, its "sa" voice on the
  desktop's C host (accent_reference.py; the built nvda/dist/accent-build and accentsa-lib), a fresh unit per case as
  the app has; the request's rate and pitch put on the driver's scales as ssa_map.c does, the pitch as a capital's
  PitchCommand.  Its front end's text (currencies, _clean, number words) is compared on TEXTS as well.  And a capital:
  a request's raised pitch (150 %, and 120 %, which ssa_accent_pitch moves to the next of the Accent's ten steps) and
  lowered one (75 %) must sound different from 100 %, and the next request at 100 % must be byte-identical to the
  first.

    python test_android_native.py                 build the desktop program, run it, compare
    python test_android_native.py --adb           also run build/android/<abi>/test_android_native on the attached
                                                  device (sh build_android.sh --test <abi> first; ANDROID_SERIAL picks
                                                  the device), pushed to /data/local/tmp and removed afterwards
    SSI263_ANDROID_TEST_BREAK=1                   the control: the program drops the request's rate (ssa_map.h), so
                                                  the "fast" cases must differ -- this run must FAIL
    SSI263_ANDROID_TEST_BREAK=accent-pitch        the Accent SA's controls (ssa_engine.h's ssa_accent_break), each
                             accent-glide         must FAIL: the request's pitch dropped; the pitch sent as a setting,
                             accent-reuse         glided to (no snap_pitch); one unit kept across utterances; the
                             accent-step          plain pitch mapping (a 120 % request on the 100 % step)

Options: --firmware <folder> (default $SSI263_FIRMWARE, else firmware/blazie; the Spanish unit there or in its
spanish/ folder), --lib <reference library>, --chip <ssi263.dll bl.dll needs>, --abi <abi> (default arm64-v8a),
--aicom <folder> (default firmware/aicom-accent-sa).
"""
import argparse
import ctypes
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

SRC = os.path.join(REPO, "src", "csrc")
CPP = os.path.join(REPO, "src", "platforms", "android", "app", "src", "main", "cpp")
# each control builds its own program (run_tests runs them all at once: one output file would race)
OUT = os.path.join(REPO, "build", "android-host" + ("-control-" + re.sub(r"\W", "_", os.environ["SSI263_ANDROID_TEST_BREAK"])
                                                     if os.environ.get("SSI263_ANDROID_TEST_BREAK") else ""))
WINDOWS = sys.platform == "win32"

HELLO = "Hello there. This is the Braille Lite, speaking on a phone."
LONG = ("This is a long message for the stop test, with a comma or two, that keeps going well past the moment the "
        "harness says stop. It has a second sentence as well.")
SPANISH = "Mañana, ¿qué tal? Él está aquí."
# (name, text, SSIP rate, SSIP pitch, blocks before a cancel or None) -- the same order as the C program, one unit
# for the English cases: the unit's state carries from one utterance to the next on both sides
CASES = [
    ("default", HELLO, 0, 0, None),
    ("default-97", HELLO, 0, 0, None),
    ("fast", HELLO, 50, 0, None),          # Android 200% rate = SSIP 50
    ("slow-low", HELLO, -40, -50, None),   # the app's rate slider at 30 = SSIP -40; Android 50% pitch = SSIP -50
    ("stopped", LONG, 0, 0, 5),
    ("after-stop", "Next message.", 0, 0, None),
]
FILES = {"en": ("BL2ENG.BNS", "bl2_2003_warm.state"), "es": ("BL2SPA.BNS", "bl2spa_fresh.state")}
AICOM = os.path.join(REPO, "firmware", "aicom-accent-sa")
ROMS = ("u2.BIN", "u3.BIN", "u4.BIN")

# The Accent SA's cases: test_android_native.c's accent_cases, in the same order -- (name, text, the app's rate and
# pitch sliders, the request's rate and pitch percentages, the engine volume, inflection, sample rate, pull size,
# blocks before a stop).  Each is a unit of its own on both sides.
HELLO_A = "Hello there. This is the Accent SA, speaking on a phone."
LONG_A = ("This is a long message for the stop test, with a comma or two, that keeps going well past the moment the "
          "harness says stop. It has a second sentence as well.")
NUMBERS_A = "You owe $1234.50 for 3 items: 100 percent, the 21st of 1,000,000 and -2.5 degrees."
TEXT_A = "It\u2019s \u201cquoted\u201d \u2013 see ~/code\u2026 \u00a32.63, 5 \u20ac and caf\u00e9\tend \U0001F389"
CAP_A = "B"
ACCENT_CASES = [
    ("a-default", HELLO_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-default-97", HELLO_A, 50, 50, 100, 100, 100, 1, 22050, 97, 0),
    ("a-fast", HELLO_A, 50, 50, 200, 100, 100, 1, 22050, 4096, 0),
    ("a-slow-low", HELLO_A, 30, 50, 100, 50, 100, 1, 22050, 4096, 0),
    ("a-sliders", HELLO_A, 70, 80, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-numbers", NUMBERS_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-text", TEXT_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-pitch-100", CAP_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-pitch-150", CAP_A, 50, 50, 100, 150, 100, 1, 22050, 4096, 0),
    ("a-pitch-75", CAP_A, 50, 50, 100, 75, 100, 1, 22050, 4096, 0),
    ("a-pitch-120", CAP_A, 50, 50, 100, 120, 100, 1, 22050, 4096, 0),     # a raise that would land on the same step
    ("a-pitch-100-again", CAP_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-stopped", LONG_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 5),
    ("a-after-stop", "Next message.", 50, 50, 100, 100, 100, 1, 22050, 4096, 0),
    ("a-volume-150", HELLO_A, 50, 50, 100, 100, 150, 1, 22050, 4096, 0),
    ("a-monotone", HELLO_A, 50, 50, 100, 100, 100, 0, 22050, 4096, 0),
    ("a-11k", HELLO_A, 50, 50, 100, 100, 100, 1, 11025, 4096, 0),
]
# The front end's text, against the driver's (currencies, _clean, strip, _numbers): numbers and money in every shape
# the regexes treat differently, and the characters _clean maps
TEXTS = [
    "100", "1,234,567.", "3.5.", "192.168.0.1", "1st 2nd 3rd 4th 11th 12th 21st 22nd 101st 1000000th 3RD", "-5 and 5-3",
    "1,2345 and 1234,567", "1,234,567.x", "1,234.56a", "0.5 .5 00012 0", "12345678901234567890 and 1,000,000,000,000",
    "$5 $1234.50 $1,234,567.89 $.50 $5, $0012 $1234567890123456 $12,34", "room 12a b12 12_3 x.5 5.x",
    "\u00a32.63 \u20ac5 5 \u20ac 50\u00a2 \u00a51000 a\u00a33 \u00a3 alone", "caf\u00e9 \u00f1 \u00e7a \u00fc\u00f6 \u00e0\u00e8\u00e1",
    "tabs\tand\nnewlines\x07 and ~tilde~ \x7f", "  leading and trailing  ", "", "...", "\u2014 \u2013 \u2018q\u2019 \u201cw\u201d \u2026 \u00bf\u00a1",
    "The 3rd of May, 2026 at 10:30, 99.9% of 7,000 people.", "Version 2.0.1, 3.14159 and 1,000.5",
]


def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def to100(ssip):                                    # sd_ssi263.c's to100
    return max(0, min(100, (ssip + 100) // 2))


def ssip_from_percent(p):                           # ssa_map.c's
    return 0 if p <= 0 or p == 100 else max(-100, min(100, int(math.floor(50.0 * math.log2(p / 100.0) + 0.5))))


def on_top(slider, percent):                        # ssa_map.c's ssa_rate / ssa_pitch: the request on the slider
    return max(0, min(100, max(0, min(100, slider)) + to100(ssip_from_percent(percent)) - 50))


def accent_step(p):                                 # as_voice.c's accent_pitch: the driver's _accent_pitch
    p = max(0, min(100, p))
    return int(p * 5 / 50 + 0.5) if p <= 50 else 5 + int((p - 50) * 4 / 50 + 0.5)


def accent_pitch(slider, percent):                  # ssa_engine.c's ssa_accent_pitch: at least one step moved
    p, base = on_top(slider, percent), accent_step(slider)
    if percent > 100:
        while p < 100 and accent_step(p) == base:
            p += 1
    elif 0 < percent < 100:
        while p > 0 and accent_step(p) == base:
            p -= 1
    return p


def accent_level():
    """SSA_ACCENT_LEVEL, from ssa_engine.h (one place)"""
    m = re.search(r"#define SSA_ACCENT_LEVEL (\d+)", open(os.path.join(CPP, "ssa_engine.h"), encoding="utf-8").read())
    return int(m.group(1))


# ---- the reference: bl_voice, as sd_ssi263 drives it --------------------------------------------------------------
class Ref:
    def __init__(self, lib, data, spanish):
        fw, st = FILES["es" if spanish else "en"]
        err = ctypes.create_string_buffer(256)
        self.lib = lib
        self.v = lib.blv_create(os.path.join(data, fw).encode(), os.path.join(data, st).encode(), int(spanish),
                                22050.0, 1, 0, err, 256)
        if not self.v:
            sys.exit("reference boot failed: %s" % err.value)

    def say(self, text, rate, pitch, blocks, volume=100):
        lib = self.lib
        lib.blv_set(self.v, to100(rate), to100(pitch), 7, volume, 1)
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


def load_reference(lib_path, chip_path):
    """The desktop reference library.  It must be the shipping kind -- the Braille Lite on MAME's Z180 -- never a
    z180emu build (a legacy bl.dll left in nvda/dist would compare the app's MAME engine with the old core)."""
    from tools import check_no_gpl
    hits = check_no_gpl.check(lib_path)[0]
    if hits:
        sys.exit("the reference library is not the shipping MAME build: %s" % hits[0])
    if chip_path:
        ctypes.CDLL(chip_path)                      # bl.dll imports ssi263.dll: load that copy first
    lib = ctypes.CDLL(lib_path)
    lib.blv_create.restype = ctypes.c_void_p
    lib.blv_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_double, ctypes.c_int,
                               ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    lib.blv_set.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 5
    lib.blv_speak.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.blv_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_short)),
                               ctypes.POINTER(ctypes.c_int)]
    lib.blv_cancel.argtypes = [ctypes.c_void_p]
    lib.blv_destroy.argtypes = [ctypes.c_void_p]
    return lib


def reference(lib, data):
    ref = Ref(lib, data, False)
    got = {}
    for name, text, rate, pitch, blocks in CASES:
        pcm = ref.say(text, rate, pitch, blocks)
        got[name] = (len(pcm) // 2, fnv(pcm))
    lib.blv_destroy(ref.v)
    if all(os.path.isfile(os.path.join(data, f)) for f in FILES["es"]):
        es = Ref(lib, data, True)
        pcm = es.say(SPANISH, 0, 0, None)
        got["spanish"] = (len(pcm) // 2, fnv(pcm))
        lib.blv_destroy(es.v)
    return got


# ---- the Accent SA's reference: the NVDA driver (accent_reference.py) -----------------------------------------------
def accent_reference():
    level = accent_level()
    speech = []
    for name, text, rate, pitch, req_rate, req_pitch, volume, inflection, sample_rate, chunk, stop in ACCENT_CASES:
        speech.append(dict(name=name, text=text, rate=on_top(rate, req_rate), offset=accent_pitch(pitch, req_pitch) - 50,
                           inflection=100 if inflection else 0, volume=volume * level // 100,
                           sample_rate=sample_rate, blocks=stop))
    tmp = tempfile.mkdtemp(prefix="ssi263-accent-ref-")
    try:
        path = os.path.join(tmp, "cases.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"speech": speech, "texts": TEXTS}, f)
        env = dict(os.environ, SSI263_ACCENT_SA_CORE="c", PYTHONIOENCODING="utf-8")
        r = subprocess.run([sys.executable, os.path.join(HERE, "accent_reference.py"), path], capture_output=True,
                           text=True, env=env)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode:
        sys.exit("accent_reference.py failed (%d): %s" % (r.returncode, (r.stdout + r.stderr).strip()[-2000:]))
    got = parse(r.stdout)
    texts = {int(k): v for k, v in re.findall(r"^text (\d+) ([0-9a-f]*)$", r.stdout, re.M)}
    return got, texts


# ---- the program ------------------------------------------------------------------------------------------------
def build_desktop():
    """test_android_native with the desktop compiler, the flags of build_linux.sh / build_board.py."""
    if WINDOWS:
        bindir = repo_paths.bin_dir("W64DEVKIT")
        cc, cxx = os.path.join(bindir, "gcc.exe"), os.path.join(bindir, "g++.exe")
        env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    else:
        cc, cxx, env = os.environ.get("CC", "cc"), os.environ.get("CXX", "c++"), None
    os.makedirs(OUT, exist_ok=True)
    chip = ["-O2", "-std=c99", "-ffp-contract=off"]
    # the Braille Lite's board on MAME's Z180, as the shipping libraries (bl.dll, libssi263speech.so, the app's)
    board = ["-O3", "-std=gnu89", "-ffp-contract=off", "-w", "-DBL_Z180_MAME", "-I" + os.path.join(SRC, "blazie"),
             "-I" + os.path.join(SRC, "cpu"), "-I" + SRC]
    z180 = ["-O3", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-ffp-contract=off", "-I" + os.path.join(SRC, "cpu"),
            "-I" + SRC]
    front = ["-O2", "-std=c99", "-ffp-contract=off", "-Wall", "-I" + SRC, "-I" + CPP]
    mame = ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-ffp-contract=off", "-Wall", "-Wno-sign-compare",
            "-I" + os.path.join(SRC, "cpu"), "-I" + SRC]
    accent = ["-O2", "-std=gnu89", "-ffp-contract=off", "-Wall", "-I" + os.path.join(SRC, "cpu"),
              "-I" + os.path.join(SRC, "accentsa"), "-I" + SRC]
    asa = os.path.join(SRC, "accentsa")
    units = [(chip, os.path.join(SRC, "ssi263.c")), (chip, os.path.join(SRC, "ssi263dsp.c")),
             (z180, os.path.join(SRC, "cpu", "z180_mame.cpp")), (z180, os.path.join(SRC, "cpu", "z180_asci.cpp"))] + \
            [(board, os.path.join(SRC, "blazie", n + ".c")) for n in ("bl_board", "flash29", "bl_serial", "bl_idle",
                                                                     "bl_host", "bl_voice")] + [
             (mame, os.path.join(SRC, "cpu", "i8085_mame.cpp")), (accent, os.path.join(asa, "as_board.c")),
             (accent, os.path.join(asa, "as_usart.c")), (accent, os.path.join(asa, "as_host.c")),
             (front, os.path.join(asa, "as_voice.c")), (front, os.path.join(SRC, "numwords.c")),
             (front, os.path.join(CPP, "ssa_map.c")),
             (front, os.path.join(CPP, "ssa_engine.c")), (front, os.path.join(HERE, "test_android_native.c"))]
    objs = []
    for flags, src in units:
        obj = os.path.join(OUT, os.path.splitext(os.path.basename(src))[0] + ".o")
        subprocess.run([cxx if src.endswith(".cpp") else cc] + flags + ["-c", "-o", obj, src], check=True, env=env)
        objs.append(obj)
    exe = os.path.join(OUT, "test_android_native" + (".exe" if WINDOWS else ""))
    link = ["-static", "-static-libstdc++", "-static-libgcc"] if WINDOWS else ["-lm"]
    subprocess.run([cxx, "-o", exe] + objs + link, check=True, env=env)
    return exe


def parse(text):
    got = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] != "text":
            got[parts[0]] = (int(parts[1]), parts[2])
    return got


def run_desktop(exe, data, aicom):
    r = subprocess.run([exe, data, aicom], capture_output=True, text=True)
    if r.returncode:
        sys.exit("test_android_native failed (%d): %s" % (r.returncode, r.stderr.strip()))
    return parse(r.stdout)


def run_texts(exe):
    r = subprocess.run([exe, "--texts"], input="".join(t.encode("utf-8").hex() + "\n" for t in TEXTS),
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("test_android_native --texts failed (%d): %s" % (r.returncode, r.stderr.strip()))
    return {int(k): v for k, v in re.findall(r"^text (\d+) ([0-9a-f]*)$", r.stdout, re.M)}


def run_adb(abi, data, aicom):
    adb = shutil.which("adb") or os.path.join(os.environ.get("ANDROID_HOME", ""), "platform-tools", "adb")
    exe = os.path.join(REPO, "build", "android", abi, "test_android_native")
    if not os.path.isfile(exe):
        sys.exit("no %s: sh build_android.sh --test %s" % (exe, abi))
    where = "/data/local/tmp/ssi263-android-test"
    subprocess.run([adb, "shell", "rm -rf %s && mkdir -p %s" % (where, where)], check=True)
    try:
        for f in [exe] + [os.path.join(data, n) for n in os.listdir(data)]:
            subprocess.run([adb, "push", f, where + "/"], check=True, capture_output=True)
        subprocess.run([adb, "shell", "mkdir -p %s/aicom" % where], check=True)
        for n in ROMS:
            subprocess.run([adb, "push", os.path.join(aicom, n), where + "/aicom/"], check=True, capture_output=True)
        brk = os.environ.get("SSI263_ANDROID_TEST_BREAK", "")
        r = subprocess.run([adb, "shell", "cd %s && chmod 755 test_android_native && SSI263_ANDROID_TEST_BREAK=%s "
                            "./test_android_native . aicom" % (where, brk)], capture_output=True, text=True)
        if r.returncode:
            sys.exit("on the device, test_android_native failed (%d): %s" % (r.returncode, r.stderr.strip()))
        return parse(r.stdout)
    finally:
        subprocess.run([adb, "shell", "rm -rf %s" % where], capture_output=True)


def data_folder(firmware, tmp):
    """The unit's files in one folder, as the app has them (English, and Spanish when both of its files exist)."""
    for f in FILES["en"]:
        shutil.copy2(os.path.join(firmware, f), tmp)
    for d in (firmware, os.path.join(firmware, "spanish")):
        if all(os.path.isfile(os.path.join(d, f)) for f in FILES["es"]):
            for f in FILES["es"]:
                shutil.copy2(os.path.join(d, f), tmp)
            break
    return tmp


def compare(label, got, want):
    bad = 0
    for name in want:
        g = got.get(name)
        ok = g == want[name]
        print("%-5s %-8s %-17s %s" % ("ok" if ok else "FAIL", label, name,
                                     "%d samples %s" % want[name] if ok else "got %s, reference %s" % (g, want[name])))
        bad += not ok
    return bad


def capitals(label, got):
    """A capital's pitch (TalkBack raises the request's pitch for its own utterance): heard, and gone after it."""
    bad = 0
    base, again = got.get("a-pitch-100"), got.get("a-pitch-100-again")
    for name, other in (("a-pitch-150", "a-pitch-100"), ("a-pitch-75", "a-pitch-100"), ("a-pitch-150", "a-pitch-75"),
                        ("a-pitch-120", "a-pitch-100")):
        ok = got.get(name) is not None and got.get(name)[1] != (got.get(other) or (0, None))[1]
        print("%-5s %-8s %s sounds different from %s" % ("ok" if ok else "FAIL", label, name, other))
        bad += not ok
    ok = base is not None and base == again
    print("%-5s %-8s a-pitch-100-again = a-pitch-100, byte for byte (the pitch came back)" % ("ok" if ok else "FAIL",
                                                                                               label))
    return bad + (not ok)


def compare_texts(got, want):
    bad = 0
    for i, t in enumerate(TEXTS):
        ok = got.get(i) == want.get(i)
        if not ok:
            print("FAIL text     %r: C %r, driver %r" % (t, bytes.fromhex(got.get(i, "")).decode("latin-1"),
                                                       bytes.fromhex(want.get(i, "")).decode("latin-1")))
        bad += not ok
    print("%-5s text     %d of %d texts as the driver sends them" % ("ok" if not bad else "FAIL", len(TEXTS) - bad,
                                                                    len(TEXTS)))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", default=os.environ.get("SSI263_FIRMWARE") or os.path.join(REPO, "firmware", "blazie"))
    arch = "x64" if struct.calcsize("P") == 8 else "x86"
    ap.add_argument("--lib", default=os.path.join(REPO, "nvda", "dist", "blazie-lib", arch, "bl.dll") if WINDOWS
                    else os.path.join(REPO, "build", "linux", "libssi263speech.so"))
    ap.add_argument("--chip", default=os.path.join(REPO, "src", "ssi263", "_bin", arch, "ssi263.dll") if WINDOWS
                    else None)
    ap.add_argument("--adb", action="store_true")
    ap.add_argument("--abi", default="arm64-v8a")
    ap.add_argument("--aicom", default=AICOM)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="ssi263-android-test-")
    try:
        data = data_folder(a.firmware, tmp)
        want = reference(load_reference(a.lib, a.chip), data)
        want["probe"] = want["default"]            # ssa_probe: a fresh unit speaks the first case as it did
        accent_want, text_want = accent_reference()
        want.update(accent_want)
        exe = build_desktop()
        got = run_desktop(exe, data, a.aicom)
        bad = compare("desktop", got, want) + capitals("desktop", got) + compare_texts(run_texts(exe), text_want)
        if a.adb:
            got = run_adb(a.abi, data, a.aicom)
            bad += compare("device", got, want) + capitals("device", got)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED: %d case(s) differ" % bad if bad else "the app's native part speaks as the reference, byte for byte")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
