"""Build the Speak-Out board on MAME's V40 core (not yet accepted; opt-in only), into nvda/dist/speakout-lib/:

  test_v40_contract.exe   CONTRACT.md 12's clauses on the core (../cpu/test_v40_contract.c)
  test_so_board.exe       the board's own rules on small programs (test_so_board.c); no firmware needed
  x64/speakout_v40.dll    the board for src/hosts/speakout_v40.py (the Python host's SSI263_SPEAKOUT_CORE=mame)
  x86/speakout_v40.dll    the same for 32-bit Python (NVDA 2021-2023)

w64devkit gcc/g++ (paths.local W64DEVKIT, W64DEVKIT_X86); C++17 without exceptions or RTTI, libstdc++ and libgcc
linked statically (no DLLs beside KERNEL32 and msvcrt).  python src/csrc/speakout/build_board.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CPU = os.path.join(os.path.dirname(HERE), "cpu")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

OUT = os.path.join(REPO, "nvda", "dist", "speakout-lib")
CXX = ["-O2", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-ffp-contract=off", "-Wall", "-Wno-sign-compare"]
CC = ["-O2", "-std=gnu89", "-ffp-contract=off", "-Wall"]
LINK = ["-static", "-static-libstdc++", "-static-libgcc", "-s"]
BOARD = ["so_board.c", "so_icu.c", "so_scu.c", "so_hex.c"]
ARCHES = {"x64": ("W64DEVKIT", []), "x86": ("W64DEVKIT_X86", ["-msse2", "-mfpmath=sse"])}


def objects(bindir, extra, obj, pic=False):
    """The core and the board's objects, compiled into obj/; returns their paths."""
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    os.makedirs(obj, exist_ok=True)
    inc = ["-I" + CPU, "-I" + HERE]
    out = []
    for src, tool, flags in [(os.path.join(CPU, "v40_mame.cpp"), "g++", CXX)] + [
            (os.path.join(HERE, f), "gcc", CC) for f in BOARD]:
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
    # the board as a DLL, per architecture
    for arch, (key, extra) in ARCHES.items():
        bindir = repo_paths.bin_dir(key, path_fallback=(arch == "x64"))
        objs, env = objects(bindir, extra, os.path.join(OUT, "obj_" + arch))
        os.makedirs(os.path.join(OUT, arch), exist_ok=True)
        subprocess.run([os.path.join(bindir, "g++"), "-shared"] + LINK
                       + ["-o", os.path.join(OUT, arch, "speakout_v40.dll")] + objs, env=env, check=True)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
