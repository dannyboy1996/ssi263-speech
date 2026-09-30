"""Assemble the Blazie (Braille Lite 2000 + SSI-263) NVDA add-on.

Carries the Braille Lite's firmware and a RAM snapshot containing it, which are not in the
repository: put your own copies in firmware/blazie/.  z180emu (GPLv2, with this project's
bns.c front end; set Z180EMU in paths.local) ships with its complete source in the add-on.
Output: nvda/dist/blazie-ssi263-<version>.nvda-addon

bns_live.exe is built here, 32-bit (i686, static), so the one binary runs on 32- and
64-bit Windows 7 and later; its output is identical to the x64 build's (verified).
"""
import os
import shutil
import subprocess
import sys
import zipfile

from build_common import read_manifest, check_native, copy_engine, repo_paths, rm, zip_build

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST, VERSION = read_manifest(os.path.join(HERE, "blazie"))   # nvda/blazie/manifest.ini
REPO = os.path.dirname(HERE)
ENGINE = os.path.join(REPO, "src")
Z180 = repo_paths.external("Z180EMU")
FIRMWARE = os.path.join(REPO, "firmware", "blazie", "BL2ENG.BNS")
STATE = os.path.join(REPO, "firmware", "blazie", "bl2_2003_warm.state")
GCC32 = repo_paths.bin_dir("W64DEVKIT_X86", path_fallback=False)   # never the PATH's x86_64 gcc
CFLAGS = ["-O3", "-fcommon", "-DSOCKETCONSOLE", "-std=gnu89"]
LINK = ["-O3", "-fcommon", "-std=gnu89", "-static", "-s"]

BUILD = os.path.join(HERE, "dist", "blazie-build")
OUT = os.path.join(HERE, "dist", "blazie-ssi263-%s.nvda-addon" % VERSION)


def build_bns32(out_exe):
    """i686 static bns_live.exe from the z180emu tree, as one translation unit (bns_unity.c): the compiler
    inlines bns.c's per-instruction hook into the core, ~1.6x faster than separate objects, same writes."""
    env = dict(os.environ, PATH=GCC32 + os.pathsep + os.environ["PATH"])
    gcc = os.path.join(GCC32, "gcc.exe")
    subprocess.run([gcc] + CFLAGS + LINK[3:] + ["-I.", "-Iz180", "-o", out_exe, "bns_unity.c"], cwd=Z180, env=env,
                   check=True, stderr=subprocess.DEVNULL)
    check_native(out_exe, "x86")


def main():
    for p in (FIRMWARE, STATE, os.path.join(Z180, "bns.c")):
        if not os.path.isfile(p):
            sys.exit("missing: %s" % p)
    if os.path.isdir(BUILD):
        rm(BUILD)
    sd = os.path.join(BUILD, "synthDrivers")
    eng = os.path.join(sd, "_ssi263_blazie")
    os.makedirs(eng)
    shutil.copy2(os.path.join(HERE, "blazie", "synthDrivers", "blazie.py"), sd)
    copy_engine(ENGINE, eng)
    shutil.copy2(os.path.join(ENGINE, "hosts", "blazie.py"), os.path.join(eng, "blazie_host.py"))
    # 0.7: the unit in-process (bl.dll: board + Z180 + host lockstep, per bitness), the pipe host the fallback
    shutil.copy2(os.path.join(ENGINE, "hosts", "native_blazie.py"), eng)
    # the unit's measured idle sounds (data only): the driver's click at the end of the open channel
    shutil.copy2(os.path.join(ENGINE, "hosts", "blazie_idle.py"), eng)
    sys.path.insert(0, os.path.join(ENGINE, "csrc", "blazie"))
    import build_board                                  # noqa: E402
    build_board.main()
    for arch in ("x86", "x64"):
        os.makedirs(os.path.join(eng, "bin", arch))
        shutil.copy2(os.path.join(build_board.OUT, arch, "bl.dll"), os.path.join(eng, "bin", arch, "bl.dll"))
    shutil.copy2(os.path.join(HERE, "shared", "ssi263_numwords.py"), eng)
    shutil.copy2(os.path.join(HERE, "shared", "ssi263_rates.py"), eng)
    build_bns32(os.path.join(eng, "bns_live.exe"))
    shutil.copy2(FIRMWARE, os.path.join(eng, "BL2ENG.BNS"))
    shutil.copy2(STATE, eng)
    # the Spanish Braille Lite, when its files are here (firmware/blazie/spanish: never in the repository)
    spa = os.path.join(REPO, "firmware", "blazie", "spanish")
    for name in ("BL2SPA.BNS", "bl2spa_fresh.state"):
        if os.path.isfile(os.path.join(spa, name)):
            shutil.copy2(os.path.join(spa, name), eng)
    # GPLv2: the complete corresponding source of bns_live.exe
    with zipfile.ZipFile(os.path.join(eng, "z180emu-source.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for fn in ("bns.c", "bns_unity.c", "COPYING", "README.md", "Makefile", "sconsole.h", "z180dbg.h"):
            p = os.path.join(Z180, fn)
            if os.path.isfile(p):
                z.write(p, fn)
        for root, _, files in os.walk(os.path.join(Z180, "z180")):
            for fn in files:
                if fn.endswith((".c", ".h")):
                    p = os.path.join(root, fn)
                    z.write(p, os.path.relpath(p, Z180))
        # bl.dll's own sources beside the core they link, laid out as in the repository's src/csrc so every include
        # resolves: blazie/ (the board, host and voice, MIT), cpu/ (cpu.h, MIT; z180_legacy.c, the z180emu adapter),
        # ssi263.h at the top, z180emu's z180/ beside them
        for sub in ("blazie", "cpu"):
            d = os.path.join(ENGINE, "csrc", sub)
            for fn in sorted(os.listdir(d)):
                if fn.endswith((".c", ".h", ".py", ".md")):
                    z.write(os.path.join(d, fn), sub + "/" + fn)
        z.write(os.path.join(ENGINE, "csrc", "ssi263.h"), "ssi263.h")
        z.writestr("BUILD-bl.txt", "bl.dll (the in-process unit), from this folder:\n"
                   "  gcc %s -I. -Iz180 -o bl.dll blazie/bl_unity.c blazie/bl_host.c blazie/bl_voice.c "
                   "blazie/bl_firmware.c blazie/bl_state.c ssi263.dll\n"
                   "(32-bit: add -msse2 -mfpmath=sse.  ssi263.dll is the chip, in the add-on's ssi263/_bin.)\n"
                   % " ".join(build_board.DLL_FLAGS))
        z.writestr("BUILD.txt", "Built with w64devkit GCC 16.2, i686 (32-bit), as one translation unit:\n"
                   "  gcc %s -I. -Iz180 -o bns_live.exe bns_unity.c\n" % " ".join(CFLAGS + LINK[3:]))
    shutil.copy2(os.path.join(Z180, "COPYING"), os.path.join(eng, "COPYING.z180emu"))
    # NVDA 2024.4+: the add-on's symbol dictionary (manifest [symbolDictionaries]) sends stacked '?' to the unit
    shutil.copytree(os.path.join(HERE, "blazie", "locale"), os.path.join(BUILD, "locale"))
    with open(os.path.join(BUILD, "manifest.ini"), "w", encoding="utf-8") as f:
        f.write(MANIFEST)
    zip_build(BUILD, OUT)


if __name__ == "__main__":
    main()
