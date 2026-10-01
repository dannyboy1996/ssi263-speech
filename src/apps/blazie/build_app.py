"""Build the Blazie emulator for Windows, into nvda/dist/blazie-emu/:

  blazie_emu.exe   the app (one static program: the chip, the board with z180emu, the host, the shell)
  test_chords.exe  the chord logic's tests (run_tests runs it)
  test_emu_unit.exe  the unit, headless: boot speech, a chord answered, real-time speed
  test_clock.exe     the clock controller and the keys held while the unit starts (Jayson), headless
  test_flash.exe     the file flash: the ID check, the erase's time and chirps, files kept; _break: its control
  test_files.exe     files in and out (bl_files.c, the FAT image) against the units' own commands
  blazie_files.exe   the same on a saved state, from the command line
  test_serial.exe    the serial port plugged in, headless: the storage handshake answered from the far end
  test_serial_cut.exe  the same with the receive path cut: its "ACK answered" must FAIL (run_tests' control)
  test_serial_win.exe  the Windows COM side (serial_win.c) end to end through a named pipe; _cut: its control
  test_idle.exe    the idle channel against Tomi's unit (its board built with bl_idle.c's test hooks)

w64devkit gcc, x64 (paths.local W64DEVKIT), z180emu from third_party/z180emu.  The firmware is NOT copied: a release
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
    z180 = repo_paths.Z180_CORE
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
        (os.path.join(HERE, "serial_win.c"), APP),
    ]
    objs = []
    for src, flags in units:
        o = os.path.join(obj, os.path.basename(src) + ".o")
        subprocess.run([gcc] + flags + ["-c", src, "-o", o], env=env, check=True)
        objs.append(o)
    # the units' files in and out (Tomi): their file systems, the FAT image, the state files
    files = []
    for name in ("bl_files.c", "bl_files_state.c", "bl_files_xfer.c", "fat_img.c"):
        o = os.path.join(obj, name + ".o")
        subprocess.run([gcc] + APP + ["-c", os.path.join(CSRC, "blazie", name), "-o", o], env=env, check=True)
        files.append(o)
    subprocess.run([gcc, "-static", "-s", "-mwindows", "-o", os.path.join(OUT, "blazie_emu.exe")] + objs + files
                   + ["-lwinmm", "-lsetupapi", "-lcomdlg32", "-lm"], env=env, check=True)
    # the same from the command line, on a saved state (no unit runs: Linux and the BTSpeak build it alone)
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "blazie_files.exe"),
                                  os.path.join(HERE, "blazie_files.c")] + files, env=env, check=True)
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_chords.exe"),
                                  os.path.join(HERE, "test_chords.c"), os.path.join(HERE, "chords.c")],
                   env=env, check=True)
    # the unit, headless (run_tests passes it the firmware)
    unit = [o for o in objs if not o.endswith(("main_win.c.o", "tns_keymap_win.c.o", "serial_win.c.o"))]
    subprocess.run([gcc, "-static", "-s", "-o", os.path.join(OUT, "test_emu_unit.exe"),
                    os.path.join(HERE, "test_emu_unit.c")] + unit + ["-lm"], env=env, check=True)
    # the clock controller and the keys held while the unit starts (Jayson), headless
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_clock.exe"),
                                  os.path.join(HERE, "test_clock.c")] + unit + ["-lm"], env=env, check=True)
    # the file flash (test_flash.c), and its control: the boards built with BLAZIE_FLASH_BREAK (the Type 'n Speak's
    # chip answers a 29F040's ID; the Braille Lite's banks all on one 512 KB)
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_flash.exe"),
                                  os.path.join(HERE, "test_flash.c")] + unit + ["-lm"], env=env, check=True)
    brk_bl = os.path.join(obj, "bl_unity_flash_break.o")
    brk_tns = os.path.join(obj, "tns_board_flash_break.o")
    subprocess.run([gcc] + BOARD + zinc + ["-DBLAZIE_FLASH_BREAK", "-c", os.path.join(CSRC, "blazie", "bl_unity.c"),
                                           "-o", brk_bl], env=env, check=True)
    subprocess.run([gcc] + BOARD + ["-DBLAZIE_FLASH_BREAK", "-c", os.path.join(CSRC, "blazie", "tns_board.c"),
                                    "-o", brk_tns], env=env, check=True)
    unit_brk = [brk_bl if o.endswith("bl_unity.c.o") else brk_tns if o.endswith("tns_board.c.o") else o for o in unit]
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_flash_break.exe"),
                                  os.path.join(HERE, "test_flash.c")] + unit_brk + ["-lm"], env=env, check=True)
    # files in and out against the units' own commands (test_files.c; its controls: --break=N)
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_files.exe"),
                                  os.path.join(HERE, "test_files.c")] + unit + files + ["-lm"], env=env, check=True)
    # the serial port plugged in, and its control: the same board with the receive path cut (bl_serial.c)
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_serial.exe"),
                                  os.path.join(HERE, "test_serial.c")] + unit + ["-lm"], env=env, check=True)
    cut = os.path.join(obj, "bl_unity_cut_rx.o")
    subprocess.run([gcc] + BOARD + zinc + ["-DBL_SERIAL_CUT_RX", "-c", os.path.join(CSRC, "blazie", "bl_unity.c"),
                                           "-o", cut], env=env, check=True)
    unit_cut = [cut if o.endswith("bl_unity.c.o") else o for o in unit]
    subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, "test_serial_cut.exe"),
                                  os.path.join(HERE, "test_serial.c")] + unit_cut + ["-lm"], env=env, check=True)
    # the Windows side end to end through a named pipe (serial_win.c), and its control on the cut board
    win = os.path.join(obj, "serial_win.c.o")
    for exe, board in (("test_serial_win.exe", unit), ("test_serial_win_cut.exe", unit_cut)):
        subprocess.run([gcc] + APP + ["-static", "-s", "-o", os.path.join(OUT, exe),
                                      os.path.join(HERE, "test_serial_win.c"), win] + board
                       + ["-lsetupapi", "-lm"], env=env, check=True)
    # the idle channel's sounds against the unit's (run_tests passes the firmware); its board built with the test hooks
    # its must-fail controls use (bl_idle.c, BLI_TEST_HOOKS)
    hooks = os.path.join(obj, "bl_unity_hooks.o")
    subprocess.run([gcc] + BOARD + zinc + ["-DBLI_TEST_HOOKS", "-c", os.path.join(CSRC, "blazie", "bl_unity.c"), "-o", hooks],
                   env=env, check=True)
    subprocess.run([gcc, "-static", "-s", "-o", os.path.join(OUT, "test_idle.exe"), os.path.join(HERE, "test_idle.c"), hooks]
                   + [o for o in objs if o.endswith(("ssi263.c.o", "ssi263dsp.c.o", "bl_host.c.o"))] + ["-lm"],
                   env=env, check=True)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
