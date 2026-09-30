"""The firmware import's native part, on the desktop: what the Android app does with a file a user brings
(src/csrc/blazie/bl_firmware.c: only the releases on its list) and the state it makes from the firmware (bl_state.c),
through the desktop library (bl.dll + ssi263.dll on Windows, libssi263speech.so on Linux), loaded with ctypes.

    python test_import_native.py            every case; the generated states must be the listed ones, byte for byte
                                            (the shipped ones where they ship), and speak as they do
                                            (test_android_native.py's cases, through bl_voice); the progress
                                            blv_make_state reports must come steadily through each recipe
    SSI263_IMPORT_TEST_BREAK=1              control: the English warm reset holds the wrong chord (bl_state.h), so
                                            the English state and its speech must differ -- this run must FAIL
                                            (English only: Spanish takes 1150 million instructions)
    SSI263_IMPORT_TEST_BREAK=hash           control: the list dropped (bl_firmware.h's blv_firmware_break), so an
                                            image not on it is taken -- the unknown-release case must FAIL

Every fixture is made here, at test time, from the unit's files in the firmware folder (--firmware, default
$SSI263_FIRMWARE, else firmware/blazie): BL2ENG.BNS + bl2_2003_warm.state, spanish/BL2SPA.BNS + bl2spa_fresh.state,
tns/TNSENG.TNS (a Type 'n Speak image: Blazie's notice, but not a Braille Lite the voice can run), and
once2000/BL2ENG.BNS (ONCE's September 2000 English; its cases are skipped, and said so, when it is not there).
Nothing is kept.
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
BREAK = os.environ.get("SSI263_IMPORT_TEST_BREAK", "")
BREAK_STATE = bool(BREAK) and BREAK != "hash"
BREAK_HASH = BREAK == "hash"
ENGLISH, SPANISH, NONE, REFUSED, WRITE, UNKNOWN = 0, 1, -1, -2, -3, -4
NAMES = {ENGLISH: "English", SPANISH: "Spanish", NONE: "none", REFUSED: "refused", WRITE: "write",
         UNKNOWN: "unknown"}
LABELS = ["Braille Lite English: the June 5, 2003 revision",
          "Braille Lite English: ONCE's September 20, 2000 revision",
          "Braille Lite Spanish: ONCE's September 20, 2000 revision"]
CHUNK = 10 * 1000 * 1000                     # bl_state.c's: instructions between progress reports
RECIPE = {ENGLISH: 150 * 1000 * 1000, SPANISH: 1150 * 1000 * 1000}
PROGRESS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double)

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
    lib.blv_firmware_label.restype = ctypes.c_char_p
    lib.blv_firmware_label.argtypes = [ctypes.c_int]
    lib.blv_firmware_language.argtypes = [ctypes.c_int]
    lib.blv_state_check.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.blv_sha256.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_char_p]
    lib.blv_make_state.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, PROGRESS, ctypes.c_void_p,
                                   ctypes.c_char_p, ctypes.c_int]
    ctypes.c_int.in_dll(lib, "blv_state_break").value = 1 if BREAK_STATE else 0
    ctypes.c_int.in_dll(lib, "blv_firmware_break").value = 1 if BREAK_HASH else 0
    return lib


def import_bytes(lib, data, out):
    msg = ctypes.create_string_buffer(256)
    return lib.blv_import_firmware(data, len(data), out.encode(), msg, 256), msg.value.decode()


def check_sha256(lib):
    rng = random.Random(263)
    good = True
    for n in (0, 1, 55, 56, 63, 64, 65, 119, 120, 1000, 262074):
        data = bytes(rng.getrandbits(8) for _ in range(n))
        out = ctypes.create_string_buffer(32)
        lib.blv_sha256(data, n, out)
        good = good and out.raw == hashlib.sha256(data).digest()
    result(good, "sha256 = hashlib's, at every padding boundary")


def check_list(lib):
    got = [(lib.blv_firmware_label(k).decode(), lib.blv_firmware_language(k)) for k in range(lib.blv_firmware_count())]
    want = list(zip(LABELS, (ENGLISH, ENGLISH, SPANISH)))
    result(got == want, "the list: %d releases, labelled" % len(want), "" if got == want else "got %s" % got)
    result(lib.blv_firmware_label(len(want)) is None and lib.blv_firmware_language(-1) == -1, "past the list: none")


def spanish_dir(fw):
    return fw if os.path.isfile(os.path.join(fw, "BL2SPA.BNS")) else os.path.join(fw, "spanish")


def once_english(fw):
    path = os.path.join(fw, "once2000", "BL2ENG.BNS")
    return path if os.path.isfile(path) else None


def check_import(lib, fw, tmp):
    eng = open(os.path.join(fw, "BL2ENG.BNS"), "rb").read()
    spa = open(os.path.join(spanish_dir(fw), "BL2SPA.BNS"), "rb").read()
    tns = open(os.path.join(fw, "tns", "TNSENG.TNS"), "rb").read()
    out = os.path.join(tmp, "out.BNS")
    rng = random.Random(1)
    junk = lambda n: b"MZ" + bytes(rng.getrandbits(8) for _ in range(n - 2))  # noqa: E731

    def case(name, data, want, want_bytes=None, want_msg=None):
        if os.path.exists(out):
            os.remove(out)
        got, msg = import_bytes(lib, data, out)
        ok = got == want
        detail = "%s (%s)" % (NAMES.get(got, got), msg) if not ok else NAMES[got]
        if ok and want_bytes is not None:
            ok = os.path.isfile(out) and open(out, "rb").read() == want_bytes
            detail += "" if ok else ", but the file written is not the one expected"
        if ok and want < 0:
            ok = not os.path.exists(out)
            detail += "" if ok else ", but a file was left"
        if ok and want_msg is not None:
            ok = msg == want_msg
            detail += "" if ok else ", but said %r" % msg
        result(ok, name, detail)

    if not BREAK_HASH:
        case("BL2ENG.BNS: English, June 2003, written as it came", eng, ENGLISH, eng, LABELS[0])
        case("BL2SPA.BNS: Spanish, September 2000, written as it came", spa, SPANISH, spa, LABELS[2])
        once = once_english(fw)
        if once:
            data = open(once, "rb").read()
            case("once2000/BL2ENG.BNS: English, September 2000, written as it came", data, ENGLISH, data, LABELS[1])
        else:
            print("skip once2000/BL2ENG.BNS (ONCE's September 2000 English) is not in the firmware folder")
        image = eng[0x3000:]
        case("an update program with the .BNS inside it: English, only the image written, at 3000h",
             junk(0x1234) + eng + junk(5000), ENGLISH, bytes(0x3000) + image)
        case("the 1998 layout (image at 5000h): English", junk(0x5000) + image, ENGLISH, bytes(0x3000) + image)
        case("TNSENG.TNS (Type 'n Speak): refused", tns, REFUSED,
             want_msg="Braille Lite firmware, but not a release this voice can run")
        case("the English state (no image in it): none", open(os.path.join(fw, "bl2_2003_warm.state"), "rb").read(),
             NONE, want_msg="no Braille Lite firmware in it")
        case("random bytes: none", junk(300000), NONE, want_msg="no Braille Lite firmware in it")
        case("an empty file: none", b"", NONE)
        at = lib.blv_find_image(image[:9] + b"x" + image[10:], len(image))
        result(at == -1, "the notice must follow F3 C3 xx xx FF", "found at %d" % at if at != -1 else "")
    # The unknown releases: images the voice can start (bl_create's sites are there) that are not on the list, as
    # another country's release would be.  The hash control takes them, so these must fail under it.
    changed = bytearray(eng)
    changed[-1] ^= 0xFF
    unknown = "Braille Lite 2000 firmware, but not a release this app knows"
    case("a release not on the list (June 2003, its last byte changed): refused as unknown", bytes(changed), UNKNOWN,
         want_msg=unknown)
    case("June 2003 one byte short: refused as unknown", eng[:-1], UNKNOWN, want_msg=unknown)


def check_states(lib, fw, tmp):
    spa = spanish_dir(fw)
    pairs = {"English": (os.path.join(fw, "BL2ENG.BNS"), os.path.join(fw, "bl2_2003_warm.state")),
             "Spanish": (os.path.join(spa, "BL2SPA.BNS"), os.path.join(spa, "bl2spa_fresh.state"))}
    for name, (bns, state) in pairs.items():
        result(lib.blv_state_check(bns.encode(), state.encode()) == 1, "the shipped %s state is its release's" % name)
        data = bytearray(open(state, "rb").read())
        data[-1] ^= 1
        changed = os.path.join(tmp, "changed.state")
        open(changed, "wb").write(bytes(data))
        result(lib.blv_state_check(bns.encode(), changed.encode()) == 0, "... with one byte changed it is not")
    crossed = lib.blv_state_check(pairs["English"][0].encode(), pairs["Spanish"][1].encode())
    result(crossed == 0, "the Spanish state is not the English release's")


def make_state(lib, firmware, language, out):
    """blv_make_state with a progress callback: (ok, message, seconds, [(seconds, done), ...])."""
    err = ctypes.create_string_buffer(256)
    reports = []
    t = time.time()

    def progress(ctx, done):
        reports.append((time.time() - t, done))
        return 0
    ok = lib.blv_make_state(firmware.encode(), language, out.encode(), PROGRESS(progress), None, err, 256)
    return ok, err.value.decode(), time.time() - t, reports


def check_progress(label, language, secs, reports):
    """Every 10 million instructions, strictly rising to 1, and steady in time: the bar moves all through."""
    want = -(-RECIPE[language] // CHUNK)
    dones = [d for _, d in reports]
    rising = all(b > a for a, b in zip(dones, dones[1:])) and dones and abs(dones[-1] - 1.0) < 1e-9
    times = [0.0] + [s for s, _ in reports]
    gaps = [b - a for a, b in zip(times, times[1:])]
    steady = gaps and max(gaps) <= max(3 * secs / want, 0.25)
    ok = len(reports) == want and rising and steady
    result(ok, "%s: progress reported %d times, rising to 1, steadily" % (label, want),
           "longest gap %.2f s of %.1f s" % (max(gaps), secs) if ok else
           "%d reports, rising %s, longest gap %.2f s of %.1f s" % (len(reports), rising, max(gaps or [0]), secs))


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
    spa_dir = spanish_dir(fw)
    lib = load(a.lib, a.chip)
    tmp = tempfile.mkdtemp(prefix="ssi263-import-test-")
    try:
        if BREAK_HASH:
            check_import(lib, fw, tmp)
            return finish()
        if not BREAK_STATE:
            check_sha256(lib)
            check_list(lib)
            check_import(lib, fw, tmp)
            check_states(lib, fw, tmp)
        # the states, made from the firmware alone, into a folder laid out as the app's
        made = os.path.join(tmp, "made")
        os.makedirs(made)
        shutil.copy2(os.path.join(fw, "BL2ENG.BNS"), made)
        langs = [("English", ENGLISH, "BL2ENG.BNS", "bl2_2003_warm.state", fw)]
        if not BREAK_STATE:
            shutil.copy2(os.path.join(spa_dir, "BL2SPA.BNS"), made)
            langs.append(("Spanish", SPANISH, "BL2SPA.BNS", "bl2spa_fresh.state", spa_dir))
        for label, lang, bns, state, where in langs:
            ok, err, secs, reports = make_state(lib, os.path.join(made, bns), lang, os.path.join(made, state))
            same = ok and open(os.path.join(made, state), "rb").read() == open(os.path.join(where, state), "rb").read()
            result(same, "generated %s state = the shipped %s, byte for byte" % (label, state),
                   "%.1f s" % secs if same else (err or "different bytes"))
            if not BREAK_STATE:
                check_progress(label, lang, secs, reports)
        # ... and it speaks as the shipped one does, every case of the Android test
        shipped = os.path.join(tmp, "shipped")
        os.makedirs(shipped)
        want = TAN.reference(lib, TAN.data_folder(fw, shipped))
        if BREAK_STATE:
            want.pop("spanish", None)
        got = TAN.reference(lib, made)
        for name in want:
            ok = got.get(name) == want[name]
            result(ok, "speech from the generated states: %s" % name,
                   "%d samples %s" % want[name] if ok else "got %s, shipped %s" % (got.get(name), want[name]))
        if not BREAK_STATE:
            check_once_english(lib, fw, tmp, want)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return finish()


def check_once_english(lib, fw, tmp, june):
    """ONCE's September 2000 English: no state ships for it, so the list's hash is the reference -- made here with
    the English recipe, checked against it, then made to speak every English case, near June 2003's lengths."""
    once = once_english(fw)
    if not once:
        print("skip ONCE's September 2000 English state: once2000/BL2ENG.BNS is not in the firmware folder")
        return
    d = os.path.join(tmp, "once")
    os.makedirs(d)
    shutil.copy2(once, os.path.join(d, "BL2ENG.BNS"))
    state = os.path.join(d, "bl2_2003_warm.state")
    ok, err, secs, reports = make_state(lib, os.path.join(d, "BL2ENG.BNS"), ENGLISH, state)
    listed = ok and lib.blv_state_check(os.path.join(d, "BL2ENG.BNS").encode(), state.encode()) == 1
    result(listed, "generated September 2000 English state = the list's", "%.1f s" % secs if listed else
           (err or "a different state"))
    check_progress("September 2000 English", ENGLISH, secs, reports)
    got = TAN.reference(lib, d)
    for name in (n for n in june if n != "spanish"):
        n, want_n = got.get(name, (0, ""))[0], june[name][0]
        close = n > 0 and abs(n - want_n) <= max(0.02 * want_n, 2000)
        result(close, "September 2000 English speaks: %s" % name, "%d samples (June 2003: %d)" % (n, want_n))


def finish():
    print("import: %d FAILED" % bad if bad else "import: every case as expected")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
