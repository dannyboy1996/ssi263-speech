"""Build the Speak-Out board on MAME's V40 core, its host and its voice in C, into nvda/dist/speakout-lib/:

  test_v40_contract.exe   CONTRACT.md 12's clauses on the core (../cpu/test_v40_contract.c)
  test_so_board.exe       the board's own rules on small programs (test_so_board.c); no firmware needed
  x64/speakout_v40.dll    the board for src/hosts/speakout_v40.py (the add-on's default core, mame-steps)
  x86/speakout_v40.dll    the same for 32-bit Python (NVDA 2021-2023)
  so_render.exe           the voice's C API alone (so_voice.h): SPEAKOUT.HEX and a text into a WAV, the chip built in
  x64/so_voice.dll        the board, the host and the voice in C (so_host.h, so_voice.h), for so_voice_equiv.py; it
  x86/so_voice.dll        links src/ssi263/_bin/<arch>/ssi263.dll, so the voice runs the chip library the driver runs

The add-on ships only speakout_v40.dll (nvda/build_speakout.py); so_voice.dll and so_render.exe are for the tests and
for front ends without Python (build_linux.sh and build_android.sh compile the same sources).

w64devkit gcc/g++ (paths.local W64DEVKIT, W64DEVKIT_X86); C++17 without exceptions or RTTI, libstdc++ and libgcc
linked statically (no DLLs beside KERNEL32, msvcrt and, for so_voice.dll, ssi263.dll).
python src/csrc/speakout/build_board.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CSRC = os.path.dirname(HERE)
CPU = os.path.join(CSRC, "cpu")
REPO = os.path.dirname(os.path.dirname(CSRC))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

OUT = os.path.join(REPO, "nvda", "dist", "speakout-lib")
CXX = ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-ffp-contract=off", "-Wall", "-Wno-sign-compare"]
CC = ["-O2", "-std=gnu89", "-ffp-contract=off", "-Wall"]
FRONT = ["-O2", "-std=c99", "-ffp-contract=off", "-Wall"]
CHIP = ["-O2", "-std=c99", "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-unused-parameter"]
LINK = ["-static", "-static-libstdc++", "-static-libgcc", "-s"]
BOARD = ["so_board.c", "so_icu.c", "so_scu.c", "so_hex.c"]
# the host and the voice, with the number words they share with the other voices: [(source, flags)]
VOICE = [(os.path.join(HERE, "so_host.c"), CC), (os.path.join(HERE, "so_voice.c"), FRONT),
         (os.path.join(CSRC, "numwords.c"), FRONT)]
ARCHES = {"x64": ("W64DEVKIT", []), "x86": ("W64DEVKIT_X86", ["-msse2", "-mfpmath=sse"])}


def objects(bindir, extra, obj, more=()):
    """The core and the board's objects, and `more` ([(source, flags)], C), compiled into obj/; returns their paths."""
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    os.makedirs(obj, exist_ok=True)
    inc = ["-I" + CPU, "-I" + HERE]
    out = []
    for src, tool, flags in [(os.path.join(CPU, "v40_mame.cpp"), "g++", CXX)] + [
            (os.path.join(HERE, f), "gcc", CC) for f in BOARD] + [(s, "gcc", f) for s, f in more]:
        o = os.path.join(obj, os.path.splitext(os.path.basename(src))[0] + ".o")
        subprocess.run([os.path.join(bindir, tool)] + flags + extra + inc + ["-c", "-o", o, src], env=env, check=True)
        out.append(o)
    return out, env


def main():
    os.makedirs(OUT, exist_ok=True)
    # the test programs (64-bit)
    bindir = repo_paths.bin_dir("W64DEVKIT", path_fallback=True)
    objs, env = objects(bindir, [], os.path.join(OUT, "obj"))
    gcc, gxx = os.path.join(bindir, "gcc"), os.path.join(bindir, "g++")
    for exe, src, deps in (("test_v40_contract.exe", os.path.join(CPU, "test_v40_contract.c"), objs[:1]),
                           ("test_so_board.exe", os.path.join(HERE, "test_so_board.c"), objs)):
        o = os.path.join(OUT, "obj", os.path.basename(src) + ".o")
        subprocess.run([gcc] + CC + ["-I" + CPU, "-I" + HERE, "-c", "-o", o, src], env=env, check=True)
        subprocess.run([gxx] + LINK + ["-o", os.path.join(OUT, exe), o] + deps, env=env, check=True)
    # the voice's C API alone, the chip compiled in
    objs, env = objects(bindir, [], os.path.join(OUT, "obj_render"), VOICE + [
        (os.path.join(HERE, "so_render.c"), FRONT), (os.path.join(CSRC, "ssi263.c"), CHIP),
        (os.path.join(CSRC, "ssi263dsp.c"), CHIP)])
    subprocess.run([gxx] + LINK + ["-o", os.path.join(OUT, "so_render.exe")] + objs, env=env, check=True)
    # per architecture: the board as a DLL (what the add-on ships), and the board, host and voice importing ssi263.dll
    for arch, (key, extra) in ARCHES.items():
        bindir = repo_paths.bin_dir(key, path_fallback=(arch == "x64"))
        objs, env = objects(bindir, extra, os.path.join(OUT, "obj_" + arch))
        os.makedirs(os.path.join(OUT, arch), exist_ok=True)
        subprocess.run([os.path.join(bindir, "g++"), "-shared"] + LINK
                       + ["-o", os.path.join(OUT, arch, "speakout_v40.dll")] + objs, env=env, check=True)
        chip_dll = os.path.join(REPO, "src", "ssi263", "_bin", arch, "ssi263.dll")
        if not os.path.isfile(chip_dll):
            sys.exit("ssi263.dll not found for %s: build it first (python src/csrc/build_native.py)" % arch)
        objs, env = objects(bindir, extra, os.path.join(OUT, "obj_voice_" + arch), VOICE)
        subprocess.run([os.path.join(bindir, "g++"), "-shared"] + LINK
                       + ["-o", os.path.join(OUT, arch, "so_voice.dll")] + objs + [chip_dll], env=env, check=True)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
