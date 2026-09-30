"""Build the Blazie emulator for Windows, into nvda/dist/blazie-emu/:

  blazie_emu.exe   the app (one static program: the chip, the board with z180emu, the host, the shell)
  test_chords.exe  the chord logic's tests (run_tests runs it)
  test_emu_unit.exe  the unit, headless: boot speech, a chord answered, real-time speed

w64devkit gcc, x64 (paths.local W64DEVKIT), z180emu from paths.local Z180EMU.  The firmware is NOT copied: a release
puts firmware\\ beside the program; run from the source tree, the program finds firmware/blazie/ itself.

    python src/apps/blazie/build_app.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
from tools import repo_paths  # noqa: E402

CSRC = os.path.join(REPO, "src", "csrc")
OUT = os.path.join(REPO, "nvda", "dist", "blazie-emu")
CHIP = ["-O2", "-std=c99", "-ffp-contract=off", "-Wall"]
BOARD = ["-O3", "-fcommon", "-std=gnu89", "-ffp-contract=off", "-w"]
APP = ["-O2", "-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-format-truncation"]


def main():
    z180 = repo_paths.external("Z180EMU")
    bindir = repo_paths.bin_dir("W64DEVKIT", path_fallback=True)
    gcc = os.path.join(bindir, "gcc.exe")
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    obj = os.path.join(OUT, "obj")
    os.makedirs(obj, exist_ok=True)
    zinc = ["-I" + z180, "-I" + os.path.join(z180, "z180"), "-fmacro-prefix-map=%s=." % z180]
    units = [
        (os.path.join(CSRC, "ssi263.c"), CHIP),
        (os.path.join(CSRC, "ssi263dsp.c"), CHIP),
        (os.path.join(CSRC, "blazie", "bl_unity.c"), BOARD + zinc),
        (os.path.join(CSRC, "blazie", "bl_host.c"), BOARD),
        (os.path.join(CSRC, "blazie", "tns_board.c"), BOARD),
        (os.path.join(HERE, "emu_unit.c"), APP),
        (os.path.join(HERE, "chords.c"), APP),
        (os.path.join(HERE, "main_win.c"), APP),
        (os.path.join(HERE, "tns_keymap_win.c"), APP),
    ]
    objs = []
    for src, flags in units:
        o = os.path.join(obj, os.path.basename(src) + ".o")
        subprocess.run([gcc] + flags + ["-c", src, "-o", o], env=env, check=True)
        objs.append(o)
    subprocess.run([gcc, "-static", "-s", "-mwindows", "-o", os.path.join(OUT, "blazie_emu.exe")] + objs
                   + ["-lwinmm", "-lm"], env=env, check=True)
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_chords.exe"),
                                  os.path.join(HERE, "test_chords.c"), os.path.join(HERE, "chords.c")],
                   env=env, check=True)
    # the unit, headless (run_tests passes it the firmware)
    subprocess.run([gcc, "-static", "-s", "-o", os.path.join(OUT, "test_emu_unit.exe"),
                    os.path.join(HERE, "test_emu_unit.c")] + [o for o in objs if not o.endswith(("main_win.c.o", "tns_keymap_win.c.o"))]
                   + ["-lm"], env=env, check=True)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
