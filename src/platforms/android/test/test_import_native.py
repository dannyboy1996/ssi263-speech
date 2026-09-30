"""The firmware import's native part, on the desktop: what the Android app does with a file a user brings
(src/csrc/blazie/bl_firmware.c) and the state it makes from the firmware (bl_state.c), through the desktop library
(bl.dll + ssi263.dll on Windows, libssi263speech.so on Linux), loaded with ctypes.

    python test_import_native.py            every case; the generated states must be the shipped ones, byte for byte,
                                            and speak as they do (test_android_native.py's cases, through bl_voice)
    SSI263_IMPORT_TEST_BREAK=1              the control: the English warm reset holds the wrong chord (bl_state.h), so
                                            the English state and its speech must differ -- this run must FAIL
                                            (English only: Spanish takes 1150 million instructions)

Every fixture is made here, at test time, from the unit's files in the firmware folder (--firmware, default
$SSI263_FIRMWARE, else firmware/blazie): BL2ENG.BNS + bl2_2003_warm.state, spanish/BL2SPA.BNS + bl2spa_fresh.state,
tns/TNSENG.TNS (a Type 'n Speak image: Blazie's notice, but not a Braille Lite the voice can run).  Nothing is kept.
"""
import argparse
import ctypes
import hashlib
import os
import random
import shutil
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, HERE)
import test_android_native as TAN  # noqa: E402

WINDOWS = sys.platform == "win32"
BREAK = bool(os.environ.get("SSI263_IMPORT_TEST_BREAK"))
ENGLISH, SPANISH, OTHER, NONE, REFUSED = 0, 1, 2, -1, -2
NAMES = {ENGLISH: "English", SPANISH: "Spanish", OTHER: "other", NONE: "none", REFUSED: "refused", -3: "write"}

bad = 0


def result(ok, what, detail=""):
    global bad
    print("%-4s %s%s" % ("ok" if ok else "FAIL", what, (": " + detail) if detail else ""))
    bad += not ok


def load(lib_path, chip_path):
    lib = TAN.load_reference(lib_path, chip_path)
    lib.blv_find_image.restype = ctypes.c_long
    lib.blv_find_image.argtypes = [ctypes.c_char_p, ctypes.c_long]
    lib.blv_import_firmware.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.blv_state_language.argtypes = [ctypes.c_char_p, ctypes.c_long]
    lib.blv_sha256.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_char_p]
    lib.blv_make_state.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_char_p, ctypes.c_int]
    ctypes.c_int.in_dll(lib, "blv_state_break").value = 1 if BREAK else 0
    return lib


def import_bytes(lib, data, out):
    err = ctypes.create_string_buffer(256)
    return lib.blv_import_firmware(data, len(data), out.encode(), err, 256), err.value.decode()


def check_sha256(lib):
    rng = random.Random(263)
    good = True
    for n in (0, 1, 55, 56, 63, 64, 65, 119, 120, 1000, 262074):
        data = bytes(rng.getrandbits(8) for _ in range(n))
        out = ctypes.create_string_buffer(32)
        lib.blv_sha256(data, n, out)
        good = good and out.raw == hashlib.sha256(data).digest()
    result(good, "sha256 = hashlib's, at every padding boundary")


def check_import(lib, fw, tmp):
    eng = open(os.path.join(fw, "BL2ENG.BNS"), "rb").read()
    spa_dir = fw if os.path.isfile(os.path.join(fw, "BL2SPA.BNS")) else os.path.join(fw, "spanish")
    spa = open(os.path.join(spa_dir, "BL2SPA.BNS"), "rb").read()
    tns = open(os.path.join(fw, "tns", "TNSENG.TNS"), "rb").read()
    out = os.path.join(tmp, "out.BNS")
    rng = random.Random(1)
    junk = lambda n: b"MZ" + bytes(rng.getrandbits(8) for _ in range(n - 2))  # noqa: E731

    def case(name, data, want, want_bytes=None, want_err=None):
        if os.path.exists(out):
            os.remove(out)
        got, err = import_bytes(lib, data, out)
        ok = got == want
        detail = "%s (%s)" % (NAMES.get(got, got), err) if not ok else NAMES[got]
        if ok and want_bytes is not None:
            ok = os.path.isfile(out) and open(out, "rb").read() == want_bytes
            detail += "" if ok else ", but the file written is not the one expected"
        if ok and want < 0:
            ok = not os.path.exists(out) and (want_err is None or err == want_err)
            detail += "" if ok else ", but %s" % ("a file was left" if os.path.exists(out) else "said %r" % err)
        result(ok, name, detail)

    case("BL2ENG.BNS: English, written as it came", eng, ENGLISH, eng)
    case("BL2SPA.BNS: Spanish, written as it came", spa, SPANISH, spa)
    image = eng[0x3000:]
    case("an update program with the .BNS inside it: English, the image at 3000h",
         junk(0x1234) + eng + junk(5000), ENGLISH, bytes(0x3000) + image)
    case("the 1998 layout (image at 5000h): English", junk(0x5000) + image, ENGLISH, bytes(0x3000) + image)
    changed = bytearray(eng)
    changed[-1] ^= 0xFF
    case("a release not known here, that the voice can run: other", bytes(changed), OTHER, bytes(changed))
    case("TNSENG.TNS (Type 'n Speak): refused", tns, REFUSED,
         want_err="Braille Lite firmware, but not a release this voice can run")
    case("the English state (no image in it): none", open(os.path.join(fw, "bl2_2003_warm.state"), "rb").read(),
         NONE, want_err="no Braille Lite firmware in it")
    case("random bytes: none", junk(300000), NONE, want_err="no Braille Lite firmware in it")
    case("an empty file: none", b"", NONE)
    at = lib.blv_find_image(image[:9] + b"x" + image[10:], len(image))
    result(at == -1, "the notice must follow F3 C3 xx xx FF", "found at %d" % at if at != -1 else "")


def check_states(lib, fw, spa_dir):
    for name, path, want in (("bl2_2003_warm.state", os.path.join(fw, "bl2_2003_warm.state"), ENGLISH),
                             ("bl2spa_fresh.state", os.path.join(spa_dir, "bl2spa_fresh.state"), SPANISH)):
        data = open(path, "rb").read()
        got = lib.blv_state_language(data, len(data))
        result(got == want, "%s is known as %s" % (name, NAMES[want]), "" if got == want else "got %s" % got)
        data = data[:-1] + bytes([data[-1] ^ 1])
        got = lib.blv_state_language(data, len(data))
        result(got == -1, "%s with one byte changed is not" % name, "" if got == -1 else "got %s" % got)


def make_state(lib, firmware, language, out):
    err = ctypes.create_string_buffer(256)
    t = time.time()
    ok = lib.blv_make_state(firmware.encode(), language, out.encode(), None, None, err, 256)
    return ok, err.value.decode(), time.time() - t


def main():
    arch = "x64" if struct.calcsize("P") == 8 else "x86"
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", default=os.environ.get("SSI263_FIRMWARE") or os.path.join(REPO, "firmware", "blazie"))
    ap.add_argument("--lib", default=os.path.join(REPO, "nvda", "dist", "blazie-lib", arch, "bl.dll") if WINDOWS
                    else os.path.join(REPO, "build", "linux", "libssi263speech.so"))
    ap.add_argument("--chip", default=os.path.join(REPO, "src", "ssi263", "_bin", arch, "ssi263.dll") if WINDOWS
                    else None)
    a = ap.parse_args()
    fw = a.firmware
    spa_dir = fw if os.path.isfile(os.path.join(fw, "BL2SPA.BNS")) else os.path.join(fw, "spanish")
    lib = load(a.lib, a.chip)
    tmp = tempfile.mkdtemp(prefix="ssi263-import-test-")
    try:
        if not BREAK:
            check_sha256(lib)
            check_import(lib, fw, tmp)
            check_states(lib, fw, spa_dir)
        # the states, made from the firmware alone, into a folder laid out as the app's
        made = os.path.join(tmp, "made")
        os.makedirs(made)
        shutil.copy2(os.path.join(fw, "BL2ENG.BNS"), made)
        langs = [("English", ENGLISH, "BL2ENG.BNS", "bl2_2003_warm.state", fw)]
        if not BREAK:
            shutil.copy2(os.path.join(spa_dir, "BL2SPA.BNS"), made)
            langs.append(("Spanish", SPANISH, "BL2SPA.BNS", "bl2spa_fresh.state", spa_dir))
        for label, lang, bns, state, where in langs:
            ok, err, secs = make_state(lib, os.path.join(made, bns), lang, os.path.join(made, state))
            same = ok and open(os.path.join(made, state), "rb").read() == open(os.path.join(where, state), "rb").read()
            result(same, "generated %s state = the shipped %s, byte for byte" % (label, state),
                   "%.1f s" % secs if same else (err or "different bytes"))
        # ... and it speaks as the shipped one does, every case of the Android test
        shipped = os.path.join(tmp, "shipped")
        os.makedirs(shipped)
        want = TAN.reference(lib, TAN.data_folder(fw, shipped))
        if BREAK:
            want.pop("spanish", None)
        got = TAN.reference(lib, made)
        for name in want:
            ok = got.get(name) == want[name]
            result(ok, "speech from the generated states: %s" % name,
                   "%d samples %s" % want[name] if ok else "got %s, shipped %s" % (got.get(name), want[name]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("import: %d FAILED" % bad if bad else "import: every case as expected")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
