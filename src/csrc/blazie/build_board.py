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


def main():
    z180 = repo_paths.external("Z180EMU")
    gcc = os.path.join(repo_paths.bin_dir("W64DEVKIT_X86", path_fallback=False), "gcc.exe")
    env = dict(os.environ, PATH=os.path.dirname(gcc) + os.pathsep + os.environ["PATH"])
    os.makedirs(OUT, exist_ok=True)
    inc = ["-I" + HERE, "-I" + z180, "-I" + os.path.join(z180, "z180")]
    for exe, main_c in (("bl_live.exe", "bl_live.c"), ("test_bl_board.exe", "test_bl_board.c")):
        subprocess.run([gcc] + FLAGS + inc + ["-o", os.path.join(OUT, exe), os.path.join(HERE, "bl_unity.c"),
                                               os.path.join(HERE, main_c)], env=env, check=True)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
