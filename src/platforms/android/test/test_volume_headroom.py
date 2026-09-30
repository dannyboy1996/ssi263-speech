"""The Android engine's default volume leaves headroom: no clipped sample, the loudest peak under HEADROOM_DBFS.

Tomi (0.7, on the phone): the Blazie voice sat under TalkBack's own sounds.  At the desktop level (volume 100: bl_voice's
MAKEUP x 1) the voice peaks near -5 dBFS at most with its voiced RMS near -22.5 dBFS, so Android's engine volume now
runs 0-200 with a default of SsiSettings.DEFAULT_VOLUME (read from the app's source, one place): louder, still short
of clipping on these English and Spanish lines (some of them the loudest found in a 67-line sweep).

    python test_volume_headroom.py        SSI263_VOLUME_TEST_BREAK=1: volume 250 -- the test must fail (it clips)
"""
import math
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_android_native as T  # noqa: E402

HEADROOM_DBFS = -0.5
SETTINGS = os.path.join(T.REPO, "src", "platforms", "android", "app", "src", "main", "kotlin", "com", "ssi263speech",
                        "tts", "SsiSettings.kt")
EN = ["Hello there. This is the Braille Lite, speaking on a phone.",
      "One, two, three, four. Custom number processing, check box, checked.",
      "Settings dialog, press tab for more options.",
      "Wow! That was loud.", "WOW! LOUD CAPITALS!", "Yes? No! 1,234,567.", "Ahhhh, ooooh, eeeee.",
      "The quick brown fox jumps over the lazy dog, again and again."]
ES = ["La lluvia en Sevilla es una maravilla.", "Hola, ¿qué tal? Él está aquí, mañana a las tres y media.",
      "¡Atención! El número es 1.234.567.", "Buenos días, señoras y señores."]


def default_volume():
    m = re.search(r"const val DEFAULT_VOLUME = (\d+)", open(SETTINGS, encoding="utf-8").read())
    if not m:
        sys.exit("DEFAULT_VOLUME not found in %s" % SETTINGS)
    return int(m.group(1))


def main():
    arch = "x64" if sys.maxsize > 2 ** 32 else "x86"
    lib_path = os.path.join(T.REPO, "nvda", "dist", "blazie-lib", arch, "bl.dll")
    chip = os.path.join(T.REPO, "src", "ssi263", "_bin", arch, "ssi263.dll")
    firmware = os.environ.get("SSI263_FIRMWARE") or os.path.join(T.REPO, "firmware", "blazie")
    volume = 250 if os.environ.get("SSI263_VOLUME_TEST_BREAK") == "1" else default_volume()
    lib = T.load_reference(lib_path, chip)
    worst, clipped, lines = (0.0, ""), 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        data = T.data_folder(firmware, tmp)
        for spanish, texts in ((False, EN), (True, ES)):
            ref = T.Ref(lib, data, spanish)
            for text in texts:
                lib.blv_set(ref.v, 50, 50, 7, volume, 1)
                lib.blv_speak(ref.v, text.encode("utf-8"))
                pcm, done, peak = T.ctypes.POINTER(T.ctypes.c_short)(), T.ctypes.c_int(0), 0
                while not done.value:
                    n = lib.blv_render(ref.v, T.ctypes.byref(pcm), T.ctypes.byref(done))
                    for i in range(n):
                        v = abs(pcm[i])
                        peak = max(peak, v)
                        clipped += v >= 32767
                lines += 1
                if peak / 32768.0 > worst[0]:
                    worst = (peak / 32768.0, text)
            lib.blv_destroy(ref.v)
    db = 20 * math.log10(worst[0]) if worst[0] else -120.0
    ok = clipped == 0 and db <= HEADROOM_DBFS
    print("volume %d: %d lines, loudest peak %.2f dBFS (%r), %d clipped samples -- %s" % (
        volume, lines, db, worst[1][:40], clipped, "ok" if ok else "FAILED (limit %.1f dBFS, no clipping)" % HEADROOM_DBFS))
    print("volume headroom: %s" % ("PASS" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
