"""The installed app's TTS service on the attached device, end to end: Android's own TextToSpeech client, bound to
this engine by package name (the system's default engine is left alone), synthesizes a sentence to a file at the
rate asked for (TtsSelfTest.kt); the file's PCM must equal bl_voice's on the desktop for the same text, driven the
way the speech-dispatcher module maps SSIP (test_android_native.py's reference).  That covers what the host-side
test cannot: the Kotlin service, the JNI bridge and the framework's hand-over.

    python test_device_service.py [--rate 2.0] [--aloud]     (a debug build installed; ANDROID_SERIAL picks the device)

--aloud also speaks the sentence through the service on the device's speaker.  Options as test_android_native.py:
--firmware, --lib, --chip.
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_android_native as T  # noqa: E402

PKG = "com.ssi263speech.tts"
TEXT = "Hello there. This is the Braille Lite, speaking on a phone."


def adb(*args, **kw):
    exe = shutil.which("adb") or os.path.join(os.environ.get("ANDROID_HOME", ""), "platform-tools", "adb")
    return subprocess.run([exe] + list(args), capture_output=True, **kw)


def ssip_from_percent(p):                       # ssa_map.c's, for the reference side
    return 0 if p <= 0 or p == 100 else max(-100, min(100, int(math.floor(50.0 * math.log2(p / 100.0) + 0.5))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", type=float, default=2.0)
    ap.add_argument("--aloud", action="store_true")
    ap.add_argument("--firmware", default=os.environ.get("SSI263_FIRMWARE") or os.path.join(T.REPO, "firmware", "blazie"))
    ap.add_argument("--lib", default=None)
    ap.add_argument("--chip", default=None)
    a = ap.parse_args()
    arch = "x64" if T.struct.calcsize("P") == 8 else "x86"
    lib = a.lib or (os.path.join(T.REPO, "nvda", "dist", "blazie-lib", arch, "bl.dll") if T.WINDOWS
                    else os.path.join(T.REPO, "build", "linux", "libssi263speech.so"))
    chip = a.chip or (os.path.join(T.REPO, "src", "ssi263", "_bin", arch, "ssi263.dll") if T.WINDOWS else None)

    # a fresh process, so the unit's first utterance is this one on both sides
    adb("shell", "am", "force-stop", PKG, check=True)
    adb("shell", "run-as", PKG, "rm", "-f", "files/tts-test.wav")
    args = ["shell", "am", "start", "-n", PKG + "/.SettingsActivity", "--ez", "ttstest", "true",
            "--es", "text", "'%s'" % TEXT, "--ef", "rate", str(a.rate), "--ef", "pitch", "1.0"]
    if a.aloud:
        args += ["--ez", "aloud", "true"]
    adb(*args, check=True)                      # adb shell joins the arguments: the text is quoted for the device
    wav = b""
    for _ in range(60):
        time.sleep(0.5)
        r = adb("exec-out", "run-as", PKG, "cat", "files/tts-test.wav")
        if r.returncode == 0 and len(r.stdout) > 44:
            time.sleep(1.0)                     # let the file be finished
            wav = adb("exec-out", "run-as", PKG, "cat", "files/tts-test.wav").stdout
            break
    if len(wav) <= 44:
        sys.exit("no tts-test.wav from the device (is the debug build installed?)")
    pcm = wav[44:]
    got = (len(pcm) // 2, T.fnv(pcm))

    tmp = tempfile.mkdtemp(prefix="ssi263-device-test-")
    try:
        data = T.data_folder(a.firmware, tmp)
        ref = T.Ref(T.load_reference(lib, chip), data, False)
        want_pcm = ref.say(TEXT, ssip_from_percent(int(round(a.rate * 100))), 0, None)
        want = (len(want_pcm) // 2, T.fnv(want_pcm))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    ok = got == want
    print("%-5s device service, rate %.2f: %s" % ("ok" if ok else "FAIL", a.rate,
          "%d samples %s" % want if ok else "got %s, reference %s" % (got, want)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
