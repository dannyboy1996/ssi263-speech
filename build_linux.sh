#!/bin/sh
# Build the one native library for Linux: the SSI-263 chip, the Braille Lite board (z180emu's Z180 core) and the
# Braille Lite host, as libssi263speech.so -- plus the two-unit isolation test.
#
#   ./build_linux.sh                 # build/linux/libssi263speech.so + test_bl_board
#   Z180EMU=/path ./build_linux.sh   # z180emu's tree (default: third_party/z180emu)
#   CC=clang ./build_linux.sh
#
# On Windows the same sources make ssi263.dll and bl.dll (src/csrc/build_native.py, src/csrc/blazie/build_board.py);
# the flags here are theirs: -ffp-contract=off keeps the Python reference's arithmetic (no fused multiply-adds),
# so the golden vectors (nvda/tools/golden) hold on every platform.  The .so is also copied to
# src/ssi263/_bin/<platform>-<machine>/, where ssi263/dsp.py loads it.
#
# No firmware is built, fetched or shipped by this script.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
Z180="${Z180EMU:-$ROOT/third_party/z180emu}"
OUT="$ROOT/build/linux"
CC="${CC:-cc}"
SRC="$ROOT/src/csrc"

[ -f "$Z180/z180/z180.c" ] || { echo "z180emu not found at $Z180 (set Z180EMU)"; exit 1; }
mkdir -p "$OUT/obj"

# the chip: plain C99, as build_native.py
CHIP="-O2 -std=c99 -ffp-contract=off -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter"
# the board and host: gnu89 and -fcommon for z180emu's MAME-era C, as build_board.py.  initial-exec TLS: the z180emu
# adapter's current-instance pointer (src/csrc/cpu/z180_legacy.c) is read on every emulated instruction and memory
# access, and a shared library's default model makes each read a __tls_get_addr call (perf: ~5%).  The prefix map
# keeps the checkout's path out of z180.c's __FILE__ string.
BOARD="-O3 -ftls-model=initial-exec -fcommon -std=gnu89 -ffp-contract=off -fPIC -fvisibility=hidden -w -I$Z180 -I$Z180/z180 -fmacro-prefix-map=$Z180=."

$CC $CHIP -c -o "$OUT/obj/ssi263.o" "$SRC/ssi263.c"
$CC $CHIP -c -o "$OUT/obj/ssi263dsp.o" "$SRC/ssi263dsp.c"
$CC $BOARD -c -o "$OUT/obj/bl_unity.o" "$SRC/blazie/bl_unity.c"
$CC $BOARD -c -o "$OUT/obj/bl_host.o" "$SRC/blazie/bl_host.c"
$CC $BOARD -c -o "$OUT/obj/bl_voice.o" "$SRC/blazie/bl_voice.c"
$CC $BOARD -c -o "$OUT/obj/bl_firmware.o" "$SRC/blazie/bl_firmware.c"
$CC $BOARD -c -o "$OUT/obj/bl_state.o" "$SRC/blazie/bl_state.c"

# only the API is exported (SSI263_API / BL_API mark it); the Z180 core's globals stay inside
$CC -shared -o "$OUT/libssi263speech.so" "$OUT"/obj/*.o -lm
$CC $BOARD -o "$OUT/test_bl_board" "$SRC/blazie/test_bl_board.c" "$OUT/obj/bl_unity.o" -lm
# the legacy path's exceptions (CONTRACT.md 3) on z180emu
$CC $BOARD -I$SRC/cpu -o "$OUT/test_z180_legacy" "$SRC/cpu/test_z180_legacy.c" "$SRC/cpu/z180_legacy.c" -lm
# the speech-dispatcher module: one static-linked program (no .so to install beside it)
$CC $BOARD -o "$OUT/sd_ssi263" "$ROOT/src/platforms/speechd/sd_ssi263.c" "$OUT"/obj/*.o -lm

# MAME's Z180 core (src/csrc/cpu/z180_mame.cpp; not yet accepted, the .so above keeps z180emu): the two-unit test
# on it, so the C++ is built and checked on every platform.  Its own object folder: the .so's glob stays unchanged.
CXX="${CXX:-c++}"
MAME="-O3 -std=c++17 -fno-exceptions -fno-rtti -ffp-contract=off -Wall -I$SRC/cpu -I$SRC"
mkdir -p "$OUT/obj_mame"
$CXX $MAME -c -o "$OUT/obj_mame/z180_mame.o" "$SRC/cpu/z180_mame.cpp"
$CXX $MAME -c -o "$OUT/obj_mame/z180_asci.o" "$SRC/cpu/z180_asci.cpp"
$CC -O3 -std=gnu89 -ffp-contract=off -DBL_Z180_MAME -I$SRC/blazie -I$SRC/cpu -I$SRC -c -o "$OUT/obj_mame/bl_board.o" "$SRC/blazie/bl_board.c"
$CC -O3 -std=gnu89 -ffp-contract=off -I$SRC/blazie -c -o "$OUT/obj_mame/flash29.o" "$SRC/blazie/flash29.c"
$CC -O3 -std=gnu89 -I$SRC/blazie -I$SRC/cpu -I$SRC -c -o "$OUT/obj_mame/test_bl_board.o" "$SRC/blazie/test_bl_board.c"
$CXX -o "$OUT/test_bl_board_mame" "$OUT"/obj_mame/*.o -lm
$CC -O2 -std=gnu89 -I$SRC/cpu -I$SRC -c -o "$OUT/test_z180_contract.o" "$SRC/cpu/test_z180_contract.c"
$CXX -o "$OUT/test_z180_contract" "$OUT/test_z180_contract.o" "$OUT/obj_mame/z180_mame.o" "$OUT/obj_mame/z180_asci.o"
$CXX $MAME -o "$OUT/test_z180_whitebox" "$SRC/cpu/test_z180_whitebox.cpp" "$OUT/obj_mame/z180_asci.o"
# MAME's 8085 core (src/csrc/cpu/i8085_mame.cpp, the Accent SA's; no board yet): the CPU contract's tests.  Its own
# object folder, outside obj_mame's glob.
mkdir -p "$OUT/obj_i8085"
$CXX $MAME -Wno-sign-compare -c -o "$OUT/obj_i8085/i8085_mame.o" "$SRC/cpu/i8085_mame.cpp"
$CC -O2 -std=gnu89 -I$SRC/cpu -I$SRC -c -o "$OUT/obj_i8085/test_i8085_contract.o" "$SRC/cpu/test_i8085_contract.c"
$CXX -o "$OUT/test_i8085_contract" "$OUT/obj_i8085/test_i8085_contract.o" "$OUT/obj_i8085/i8085_mame.o"
# MAME's 8086 core (src/csrc/cpu/i86_mame.cpp, the Accent-mini's PC): the CPU contract's tests, and libpc86.so
# (src/csrc/pc86) for the Accent-mini host's opt-in CPU (src/hosts/pc86.py, SSI263_ACCENT_CORE=mame).  Own folder.
mkdir -p "$OUT/obj_i86"
$CXX $MAME -fPIC -Wno-sign-compare -c -o "$OUT/obj_i86/i86_mame.o" "$SRC/cpu/i86_mame.cpp"
$CC -O2 -std=gnu89 -I$SRC/cpu -I$SRC -c -o "$OUT/obj_i86/test_i86_contract.o" "$SRC/cpu/test_i86_contract.c"
$CXX -o "$OUT/test_i86_contract" "$OUT/obj_i86/test_i86_contract.o" "$OUT/obj_i86/i86_mame.o"
$CC -O3 -std=gnu89 -fPIC -I$SRC/cpu -c -o "$OUT/obj_i86/pc86.o" "$SRC/pc86/pc86.c"
$CXX -shared -o "$OUT/libpc86.so" "$OUT/obj_i86/pc86.o" "$OUT/obj_i86/i86_mame.o"

PLAT="$(python3 -c 'import sys, platform; print("%s-%s" % (sys.platform, platform.machine()))')"
mkdir -p "$ROOT/src/ssi263/_bin/$PLAT"
cp "$OUT/libssi263speech.so" "$ROOT/src/ssi263/_bin/$PLAT/"
echo "built $OUT/libssi263speech.so ($PLAT)"
