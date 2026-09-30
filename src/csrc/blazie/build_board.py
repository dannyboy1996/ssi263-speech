"""Build the Braille Lite library board's test programs (0.7), into nvda/dist/blazie-lib/:

  bl_live.exe        bns_live.exe's pipe protocol on bl_board.c -- today's Python host drives it, so
                     nvda/tools/bns_equiv.py --against the golden vectors gates the board
  test_bl_board.exe  two units in one process, alone and interleaved: identical event streams

w64devkit gcc, i686 (as bns_live.exe), one translation unit (bl_unity.c) with the Z180 core from the z180emu tree
(paths.local Z180EMU).  python src/csrc/blazie/build_board.py

The same two programs on MAME's Z180 core (../cpu/z180_mame.cpp, the corrected path; not yet accepted, see
../cpu/README.md), for comparing the cores:
  bl_live_mame.exe, test_bl_board_mame.exe
and the CPU contract's own tests on that core, test_z180_contract.exe; and those of MAME's 8085 core (the Accent
SA's, ../cpu/i8085_mame.cpp), test_i8085_contract.exe, and of MAME's 8086 (the Accent-mini's PC,
../cpu/i86_mame.cpp), test_i86_contract.exe, with x64/ and x86/pc86.dll (../pc86) for the Accent-mini host's opt-in
CPU (src/hosts/pc86.py, SSI263_ACCENT_CORE=mame).
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


# MAME's Z180 core: C++17 without exceptions or RTTI, libstdc++ and libgcc linked statically (one .exe, no DLLs)
MAME_CXX = ["-O3", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-msse2", "-mfpmath=sse", "-Wall"]
MAME_LINK = ["-static", "-static-libstdc++", "-static-libgcc", "-s"]


def build_mame(gcc, env):
    gxx = os.path.join(os.path.dirname(gcc), "g++.exe")
    cpu = os.path.join(os.path.dirname(HERE), "cpu")
    obj = os.path.join(OUT, "obj_mame")
    os.makedirs(obj, exist_ok=True)
    inc = ["-I" + HERE, "-I" + cpu, "-I" + os.path.dirname(HERE)]
    objs = {}
    for name, src, cc, flags in (
            ("z180_mame", os.path.join(cpu, "z180_mame.cpp"), gxx, MAME_CXX),
            ("z180_asci", os.path.join(cpu, "z180_asci.cpp"), gxx, MAME_CXX),
            ("bl_board", os.path.join(HERE, "bl_board.c"), gcc, ["-O3", "-std=gnu89", "-DBL_Z180_MAME"]),
            ("flash29", os.path.join(HERE, "flash29.c"), gcc, ["-O3", "-std=gnu89"]),
            ("bl_serial", os.path.join(HERE, "bl_serial.c"), gcc, ["-O3", "-std=gnu89"]),
            ("bl_live", os.path.join(HERE, "bl_live.c"), gcc, ["-O3", "-std=gnu89"]),
            ("test_bl_board", os.path.join(HERE, "test_bl_board.c"), gcc, ["-O3", "-std=gnu89"]),
            ("test_z180_contract", os.path.join(cpu, "test_z180_contract.c"), gcc, ["-O2", "-std=gnu89"]),
            ("i8085_mame", os.path.join(cpu, "i8085_mame.cpp"), gxx, MAME_CXX + ["-Wno-sign-compare"]),
            ("test_i8085_contract", os.path.join(cpu, "test_i8085_contract.c"), gcc, ["-O2", "-std=gnu89"]),
            ("i86_mame", os.path.join(cpu, "i86_mame.cpp"), gxx, MAME_CXX + ["-Wno-sign-compare"]),
            ("test_i86_contract", os.path.join(cpu, "test_i86_contract.c"), gcc, ["-O2", "-std=gnu89"])):
        objs[name] = os.path.join(obj, name + ".o")
        subprocess.run([cc] + flags + inc + ["-c", "-o", objs[name], src], env=env, check=True)
    core = [objs["bl_board"], objs["flash29"], objs["bl_serial"], objs["z180_mame"], objs["z180_asci"]]
    for exe, main_o in (("bl_live_mame.exe", "bl_live"), ("test_bl_board_mame.exe", "test_bl_board")):
        subprocess.run([gxx] + MAME_LINK + ["-o", os.path.join(OUT, exe), objs[main_o]] + core, env=env, check=True)
    # the CPU contract's tests (no board): ../cpu/test_z180_contract.c
    subprocess.run([gxx] + MAME_LINK + ["-o", os.path.join(OUT, "test_z180_contract.exe"), objs["test_z180_contract"],
                                        objs["z180_mame"], objs["z180_asci"]], env=env, check=True)
    # the white-box tests (they include the core): ../cpu/test_z180_whitebox.cpp
    subprocess.run([gxx] + MAME_CXX + MAME_LINK + inc + ["-o", os.path.join(OUT, "test_z180_whitebox.exe"),
                                                         os.path.join(cpu, "test_z180_whitebox.cpp"), objs["z180_asci"]],
                   env=env, check=True)
    # MAME's 8085 core (the Accent SA's; no board yet): the CPU contract's tests, ../cpu/test_i8085_contract.c
    subprocess.run([gxx] + MAME_LINK + ["-o", os.path.join(OUT, "test_i8085_contract.exe"),
                                        objs["test_i8085_contract"], objs["i8085_mame"]], env=env, check=True)
    # MAME's 8086 core (the Accent-mini's PC): the CPU contract's tests, ../cpu/test_i86_contract.c
    subprocess.run([gxx] + MAME_LINK + ["-o", os.path.join(OUT, "test_i86_contract.exe"),
                                        objs["test_i86_contract"], objs["i86_mame"]], env=env, check=True)


def build_pc86(arch, bindir, env, extra):
    """pc86.dll (../pc86: MAME's 8086 with 1 MB of flat memory) for src/hosts/pc86.py -- the Accent-mini host's
    opt-in CPU (SSI263_ACCENT_CORE=mame).  Static: KERNEL32 and msvcrt only, as the other DLLs."""
    cpu = os.path.join(os.path.dirname(HERE), "cpu")
    pc86 = os.path.join(os.path.dirname(HERE), "pc86")
    obj = os.path.join(OUT, "obj_pc86_" + arch)
    os.makedirs(obj, exist_ok=True)
    core_o, shim_o = os.path.join(obj, "i86_mame.o"), os.path.join(obj, "pc86.o")
    subprocess.run([os.path.join(bindir, "g++.exe"), "-O3", "-std=c++17", "-fno-exceptions", "-fno-rtti"] + extra
                   + ["-Wall", "-Wno-sign-compare", "-I" + cpu, "-c", "-o", core_o, os.path.join(cpu, "i86_mame.cpp")],
                   env=env, check=True)
    subprocess.run([os.path.join(bindir, "gcc.exe"), "-O3", "-std=gnu89", "-Wall", "-I" + cpu] + extra
                   + ["-c", "-o", shim_o, os.path.join(pc86, "pc86.c")], env=env, check=True)
    subprocess.run([os.path.join(bindir, "g++.exe"), "-shared"] + MAME_LINK
                   + ["-o", os.path.join(OUT, arch, "pc86.dll"), shim_o, core_o], env=env, check=True)


def main():
    z180 = repo_paths.Z180_CORE
    gcc = os.path.join(repo_paths.bin_dir("W64DEVKIT_X86", path_fallback=False), "gcc.exe")
    env = dict(os.environ, PATH=os.path.dirname(gcc) + os.pathsep + os.environ["PATH"])
    os.makedirs(OUT, exist_ok=True)
    # z180.c keeps __FILE__ (its CPUINFO source-file string): map the checkout's path to "." so no machine path is
    # built into the binaries (tools/check_binary_paths.py checks them)
    inc = ["-I" + HERE, "-I" + z180, "-I" + os.path.join(z180, "z180"), "-fmacro-prefix-map=%s=." % z180]
    for exe, main_c in (("bl_live.exe", "bl_live.c"), ("test_bl_board.exe", "test_bl_board.c")):
        subprocess.run([gcc] + FLAGS + inc + ["-o", os.path.join(OUT, exe), os.path.join(HERE, "bl_unity.c"),
                                               os.path.join(HERE, main_c)], env=env, check=True)
    # run_ahead.h's contract on a synthetic board and chip (no firmware): test_run_ahead.c includes run_ahead.c
    subprocess.run([gcc] + FLAGS + ["-Wall", "-I" + HERE, "-o", os.path.join(OUT, "test_run_ahead.exe"),
                                    os.path.join(HERE, "test_run_ahead.c")], env=env, check=True)
    # the legacy path's exceptions (CONTRACT.md 3) on z180emu: ../cpu/test_z180_legacy.c
    cpu = os.path.join(os.path.dirname(HERE), "cpu")
    subprocess.run([gcc] + FLAGS + inc + ["-I" + cpu, "-w", "-o", os.path.join(OUT, "test_z180_legacy.exe"),
                                           os.path.join(cpu, "test_z180_legacy.c"), os.path.join(cpu, "z180_legacy.c")],
                   env=env, check=True)
    build_mame(gcc, env)
    engine = os.path.dirname(os.path.dirname(HERE))
    for arch, (key, extra) in ARCHES.items():
        bindir = repo_paths.bin_dir(key, path_fallback=(arch == "x64"))
        env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
        out_dir = os.path.join(OUT, arch)
        os.makedirs(out_dir, exist_ok=True)
        chip_dll = os.path.join(engine, "ssi263", "_bin", arch, "ssi263.dll")
        subprocess.run([os.path.join(bindir, "gcc.exe")] + DLL_FLAGS + extra + inc
                       + ["-o", os.path.join(out_dir, "bl.dll"), os.path.join(HERE, "bl_unity.c"),
                          os.path.join(HERE, "bl_host.c"), os.path.join(HERE, "bl_voice.c"),
                          os.path.join(HERE, "bl_firmware.c"), os.path.join(HERE, "bl_state.c"), chip_dll], env=env, check=True)
        build_pc86(arch, bindir, env, extra)
    print("built %s" % OUT)


if __name__ == "__main__":
    main()
