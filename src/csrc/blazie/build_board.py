"""Build the Braille Lite library board's test programs (0.7), into nvda/dist/blazie-lib/:

  bl_live.exe        bns_live.exe's pipe protocol on bl_board.c -- today's Python host drives it, so
                     nvda/tools/bns_equiv.py --against the golden vectors gates the board
  test_bl_board.exe  two units in one process, alone and interleaved: identical event streams

w64devkit gcc, i686 (as bns_live.exe), one translation unit (bl_unity.c) with the Z180 core from the z180emu tree
(paths.local Z180EMU).  python src/csrc/blazie/build_board.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

OUT = os.path.join(REPO, "nvda", "dist", "blazie-lib")
FLAGS = ["-O3", "-fcommon", "-std=gnu89", "-static", "-s"]


# bl.dll: the board, the Z180 core and the host (bl_host.c), in-process; the chip comes from ssi263.dll (linked
# against it, so Windows reuses the copy native.py already loaded: one chip, the one SSI263C made).  Floating point
# as ssi263.dll's: SSE2 doubles on 32-bit, no fused multiply-adds -- the host's time arithmetic must be Python's.
DLL_FLAGS = ["-O3", "-fcommon", "-std=gnu89", "-ffp-contract=off", "-shared", "-static", "-static-libgcc", "-s"]
ARCHES = {"x86": ("W64DEVKIT_X86", ["-msse2", "-mfpmath=sse"]), "x64": ("W64DEVKIT", [])}


def main():
    z180 = repo_paths.external("Z180EMU")
    gcc = os.path.join(repo_paths.bin_dir("W64DEVKIT_X86", path_fallback=False), "gcc.exe")
    env = dict(os.environ, PATH=os.path.dirname(gcc) + os.pathsep + os.environ["PATH"])
    os.makedirs(OUT, exist_ok=True)
    # z180.c keeps __FILE__ (its CPUINFO source-file string): map the checkout's path to "." so no machine path is
    # built into the binaries (tools/check_binary_paths.py checks them)
    inc = ["-I" + HERE, "-I" + z180, "-I" + os.path.join(z180, "z180"), "-fmacro-prefix-map=%s=." % z180]
    for exe, main_c in (("bl_live.exe", "bl_live.c"), ("test_bl_board.exe", "test_bl_board.c")):
        subprocess.run([gcc] + FLAGS + inc + ["-o", os.path.join(OUT, exe), os.path.join(HERE, "bl_unity.c"),
                                               os.path.join(HERE, main_c)], env=env, check=True)
    engine = os.path.dirname(os.path.dirname(HERE))
    for arch, (key, extra) in ARCHES.items():
        bindir = repo_paths.bin_dir(key, path_fallback=(arch == "x64"))
        env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
        out_dir = os.path.join(OUT, arch)
        os.makedirs(out_dir, exist_ok=True)
        chip_dll = os.path.join(engine, "ssi263", "_bin", arch, "ssi263.dll")
        subprocess.run([os.path.join(bindir, "gcc.exe")] + DLL_FLAGS + extra + inc
                       + ["-o", os.path.join(out_dir, "bl.dll"), os.path.join(HERE, "bl_unity.c"),
                          os.path.join(HERE, "bl_host.c"), os.path.join(HERE, "bl_voice.c"), chip_dll], env=env, check=True)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
