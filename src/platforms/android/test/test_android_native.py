"""The Android app's native part, proven without a phone: test_android_native.c (the app's front end, ssa_engine.c and
ssa_map.c, on the same chip, board, host and voice) against bl_voice driven directly the way the speech-dispatcher
module drives it (src/platforms/speechd/sd_ssi263.c: SSIP -100..100 through its to100), every case's PCM compared by
hash.  The reference is the desktop library the NVDA add-on and Linux ship (bl.dll + ssi263.dll on Windows,
libssi263speech.so on Linux), loaded with ctypes.

    python test_android_native.py                 build the desktop program, run it, compare
    python test_android_native.py --adb           also run build/android/<abi>/test_android_native on the attached
                                                  device (sh build_android.sh --test <abi> first; ANDROID_SERIAL picks
                                                  the device), pushed to /data/local/tmp and removed afterwards
    SSI263_ANDROID_TEST_BREAK=1                   the control: the program drops the request's rate (ssa_map.h), so
                                                  the "fast" case must differ -- this run must FAIL

Options: --firmware <folder> (default $SSI263_FIRMWARE, else firmware/blazie; the Spanish unit there or in its
spanish/ folder), --lib <reference library>, --chip <ssi263.dll bl.dll needs>, --abi <abi> (default arm64-v8a).
"""
import argparse
import ctypes
import os
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
# the control builds its own program (run_tests runs both at once: one output file would race)
OUT = os.path.join(REPO, "build", "android-host" + ("-control" if os.environ.get("SSI263_ANDROID_TEST_BREAK") else ""))
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


def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def to100(ssip):                                    # sd_ssi263.c's to100
    return max(0, min(100, (ssip + 100) // 2))


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

    def say(self, text, rate, pitch, blocks):
        lib = self.lib
        lib.blv_set(self.v, to100(rate), to100(pitch), 7, to100(100), 1)
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


# ---- the program ------------------------------------------------------------------------------------------------
def build_desktop():
    """test_android_native with the desktop compiler, the flags of build_linux.sh / build_board.py."""
    z180 = repo_paths.external("Z180EMU")
    if WINDOWS:
        bindir = repo_paths.bin_dir("W64DEVKIT")
        cc = os.path.join(bindir, "gcc.exe")
        env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    else:
        cc, env = os.environ.get("CC", "cc"), None
    os.makedirs(OUT, exist_ok=True)
    chip = ["-O2", "-std=c99", "-ffp-contract=off"]
    board = ["-O3", "-fcommon", "-std=gnu89", "-ffp-contract=off", "-w", "-I" + z180, "-I" + os.path.join(z180, "z180"),
             "-fmacro-prefix-map=%s=." % z180]
    front = ["-O2", "-std=c99", "-ffp-contract=off", "-Wall", "-I" + SRC, "-I" + CPP]
    units = [(chip, os.path.join(SRC, "ssi263.c")), (chip, os.path.join(SRC, "ssi263dsp.c")),
             (board, os.path.join(SRC, "blazie", "bl_unity.c")), (board, os.path.join(SRC, "blazie", "bl_host.c")),
             (board, os.path.join(SRC, "blazie", "bl_voice.c")), (front, os.path.join(CPP, "ssa_map.c")),
             (front, os.path.join(CPP, "ssa_engine.c")), (front, os.path.join(HERE, "test_android_native.c"))]
    objs = []
    for flags, src in units:
        obj = os.path.join(OUT, os.path.splitext(os.path.basename(src))[0] + ".o")
        subprocess.run([cc] + flags + ["-c", "-o", obj, src], check=True, env=env)
        objs.append(obj)
    exe = os.path.join(OUT, "test_android_native" + (".exe" if WINDOWS else ""))
    subprocess.run([cc, "-o", exe] + objs + ([] if WINDOWS else ["-lm"]), check=True, env=env)
    return exe


def parse(text):
    got = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3:
            got[parts[0]] = (int(parts[1]), parts[2])
    return got


def run_desktop(exe, data):
    r = subprocess.run([exe, data], capture_output=True, text=True)
    if r.returncode:
        sys.exit("test_android_native failed (%d): %s" % (r.returncode, r.stderr.strip()))
    return parse(r.stdout)


def run_adb(abi, data):
    adb = shutil.which("adb") or os.path.join(os.environ.get("ANDROID_HOME", ""), "platform-tools", "adb")
    exe = os.path.join(REPO, "build", "android", abi, "test_android_native")
    if not os.path.isfile(exe):
        sys.exit("no %s: sh build_android.sh --test %s" % (exe, abi))
    where = "/data/local/tmp/ssi263-android-test"
    subprocess.run([adb, "shell", "rm -rf %s && mkdir -p %s" % (where, where)], check=True)
    try:
        for f in [exe] + [os.path.join(data, n) for n in os.listdir(data)]:
            subprocess.run([adb, "push", f, where + "/"], check=True, capture_output=True)
        brk = os.environ.get("SSI263_ANDROID_TEST_BREAK", "")
        r = subprocess.run([adb, "shell", "cd %s && chmod 755 test_android_native && SSI263_ANDROID_TEST_BREAK=%s "
                            "./test_android_native ." % (where, brk)], capture_output=True, text=True)
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
        print("%-5s %-8s %-11s %s" % ("ok" if ok else "FAIL", label, name,
                                     "%d samples %s" % want[name] if ok else "got %s, reference %s" % (g, want[name])))
        bad += not ok
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
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="ssi263-android-test-")
    try:
        data = data_folder(a.firmware, tmp)
        want = reference(load_reference(a.lib, a.chip), data)
        want["probe"] = want["default"]            # ssa_probe: a fresh unit speaks the first case as it did
        bad = compare("desktop", run_desktop(build_desktop(), data), want)
        if a.adb:
            bad += compare("device", run_adb(a.abi, data), want)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED: %d case(s) differ" % bad if bad else "the app's native part speaks as the reference, byte for byte")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
