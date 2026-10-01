"""The wheel, installed into a fresh virtual environment, speaks as the libraries it was built from do.

    python python/test_wheel.py WHEEL [FIRMWARE_DIR]
    python python/test_wheel.py --build [FIRMWARE_DIR]     build this platform's wheel from the built libraries first
                                                           (Windows: nvda/dist/blazie-lib + src/ssi263/_bin, for the
                                                           running Python's bitness; Linux: build/linux)

- the chip alone (no firmware): HF EH L OU from the installed package is audible, and two renders are
  byte-identical (the chip is deterministic);
- with FIRMWARE_DIR: the installed BrailleLite speaks the Android test's sentence byte for byte as bl.dll driven
  directly (src/platforms/android/test/test_android_native.py's reference, the way sd_ssi263 drives it).
WHEEL_TEST_BREAK=1 (the control): the installed voice is asked for rate 70 instead of 50 -- the comparison must fail.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "src", "platforms", "android", "test"))
import test_android_native as T  # noqa: E402

TEXT = T.HELLO
CHILD = r'''
import hashlib, json, os, sys
import ssi263speech as S
fw = sys.argv[1] if len(sys.argv) > 1 else ""
pcm = S.Chip(rate=22050).say_phonemes(["HF", "EH", "L", "OU"])
again = S.Chip(rate=22050).say_phonemes(["HF", "EH", "L", "OU"])
peak = max(abs(int.from_bytes(pcm[i:i + 2], "little", signed=True)) for i in range(0, len(pcm), 2))
out = {"where": os.path.dirname(S.__file__), "chip": [len(pcm) // 2, peak, pcm == again]}
if fw:
    v = S.BrailleLite(fw, rate=22050)
    v.set(rate=70 if os.environ.get("WHEEL_TEST_BREAK") else 50)
    b = v.speak(sys.argv[2])
    out["voice"] = len(b) // 2
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    out["voice_fnv"] = "%016x" % h
print(json.dumps(out))
'''


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--build":
        out = tempfile.mkdtemp(prefix="ssi263wheel")
        if os.name == "nt":
            arch = "x64" if sys.maxsize > 2 ** 32 else "x86"
            args = ["--lib-dir", os.path.join(REPO, "nvda", "dist", "blazie-lib", arch),
                    "--chip-dir", os.path.join(REPO, "src", "ssi263", "_bin", arch),
                    "--plat", "win_amd64" if arch == "x64" else "win32"]
        else:
            import platform
            args = ["--lib-dir", os.path.join(REPO, "build", "linux"), "--plat", "linux_" + platform.machine()]
        subprocess.run([sys.executable, os.path.join(HERE, "build_wheel.py"), "--out", out] + args, check=True)
        sys.argv[1] = os.path.join(out, os.listdir(out)[0])
    wheel = os.path.abspath(sys.argv[1])
    fw = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ""
    bad = 0
    keep = {"ignore_cleanup_errors": True} if sys.version_info >= (3, 10) else {}   # 3.10+ (Debian 11 has 3.9)
    with tempfile.TemporaryDirectory(**keep) as tmp:   # this process holds its bl.dll
        venv = os.path.join(tmp, "v")
        subprocess.run([sys.executable, "-m", "venv", venv], check=True)
        py = os.path.join(venv, "Scripts" if os.name == "nt" else "bin", "python")
        subprocess.run([py, "-m", "pip", "install", "-q", "--disable-pip-version-check", wheel], check=True)
        child = os.path.join(tmp, "child.py")
        open(child, "w").write(CHILD)
        r = subprocess.run([py, child, fw, TEXT], capture_output=True, text=True, cwd=tmp)
        if r.returncode:
            print("FAIL the installed package: %s" % (r.stderr.strip().splitlines() or ["?"])[-1])
            return 1
        got = json.loads(r.stdout)
        inside = os.path.commonpath([got["where"], venv]) == venv
        print("%-4s installed in the venv, not the source tree (%s)" % ("ok" if inside else "FAIL", got["where"]))
        bad += not inside
        n, peak, same = got["chip"]
        ok = n > 0 and peak > 1000 and same
        print("%-4s the chip alone: %d samples, peak %d, two renders %s" % ("ok" if ok else "FAIL", n, peak,
                                                                           "identical" if same else "DIFFERENT"))
        bad += not ok
        if fw:
            data = T.data_folder(fw, tmp) if hasattr(T, "data_folder") else fw
            lib = T.load_reference(os.path.join(got["where"], "_libs", "bl.dll" if os.name == "nt"
                                                else "libssi263speech.so"),
                                   os.path.join(got["where"], "_libs", "ssi263.dll") if os.name == "nt" else None)
            ref = T.Ref(lib, data, False)
            want = ref.say(TEXT, 0, 0, None)
            ok = [got["voice"], got["voice_fnv"]] == [len(want) // 2, T.fnv(want)]
            print("%-4s the Braille Lite: %d samples %s, bl.dll directly %d %s" % (
                "ok" if ok else "FAIL", got["voice"], got["voice_fnv"], len(want) // 2, T.fnv(want)))
            bad += not ok
    print("wheel: %s" % ("PASS" if not bad else "%d FAILED" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
