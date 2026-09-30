#!/bin/sh
# The Linux gate (x86_64 or aarch64), run on the Linux box itself -- separate from nvda/tools/run_tests.py, which is
# Windows and NVDA.  Needs a build (./build_linux.sh) and the unit's firmware in a data folder: the built NVDA add-on's
# engine folder, or any folder with BL2ENG.BNS + bl2_2003_warm.state (and BL2SPA.BNS + bl2spa_fresh.state).
#
#   tools/linux_tests.sh [data folder]      (default: nvda/dist/blazie-build/synthDrivers/_ssi263_blazie)
#
# The goldens compare with those made on Windows, byte for byte (writes, serial and the audio hash); the module
# harness checks speech-dispatcher's protocol and every message's audio, and its control must fail.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$(cd "${1:-$ROOT/nvda/dist/blazie-build/synthDrivers/_ssi263_blazie}" && pwd)"
PLAT="$(python3 -c 'import sys, platform; print("%s-%s" % (sys.platform, platform.machine()))')"
LIB="$ROOT/src/ssi263/_bin/$PLAT/libssi263speech.so"
export SSI263_LIB="$LIB" PYTHONDONTWRITEBYTECODE=1
cd "$ROOT" || exit 1
fail=0
check() {                          # name, then the command
    name="$1"; shift
    out="$("$@" 2>&1)"; got=$?
    if [ $got -eq 0 ]; then echo "ok    $name: $(echo "$out" | tail -1 | cut -c1-90)"
    else echo "FAIL  $name: $(echo "$out" | tail -1 | cut -c1-90)"; fail=1; fi
}
# A must-fail control: it passes only by failing the way it names (as run_tests.py's, after Astra, Reply 97) --
# exit 1, the extended regex MARK in its output, and no Python traceback.  A crash or a missing build is not that.
control() {                        # name, MARK, then the command
    name="$1"; mark="$2"; shift 2
    out="$("$@" 2>&1)"; got=$?
    why=""
    if echo "$out" | grep -q "Traceback (most recent call last)"; then why="control CRASHED instead of failing"
    elif [ $got -ne 1 ]; then why="control exit $got, not its expected 1"
    elif ! echo "$out" | grep -E -q "$mark"; then why="control's own failure not shown (missing $mark)"; fi
    if [ -z "$why" ]; then echo "ok    $name: $(echo "$out" | tail -1 | cut -c1-90)"
    else echo "FAIL  $name: $why"; fail=1; fi
}
[ -f "$DATA/BL2ENG.BNS" ] || { echo "no firmware in $DATA"; exit 1; }
# bns_equiv finds the firmware where the add-on build puts it
if [ "$DATA" != "$ROOT/nvda/dist/blazie-build/synthDrivers/_ssi263_blazie" ]; then
    mkdir -p "$ROOT/nvda/dist/blazie-build/synthDrivers"
    ln -sfn "$DATA" "$ROOT/nvda/dist/blazie-build/synthDrivers/_ssi263_blazie"
fi
check "two units in one process" ./build/linux/test_bl_board "$DATA/BL2ENG.BNS" "$DATA/bl2_2003_warm.state" \
    "$DATA/BL2SPA.BNS" "$DATA/bl2spa_fresh.state"
check "CPU contract tests (MAME Z180 core)" ./build/linux/test_z180_contract
check "white-box tests (MAME Z180 core)" ./build/linux/test_z180_whitebox
check "CPU contract tests (MAME 8085 core)" ./build/linux/test_i8085_contract
check "legacy path exceptions (z180emu)" ./build/linux/test_z180_legacy
check "two units in one process (MAME Z180 core)" ./build/linux/test_bl_board_mame "$DATA/BL2ENG.BNS" \
    "$DATA/bl2_2003_warm.state" "$DATA/BL2SPA.BNS" "$DATA/bl2spa_fresh.state"
check "golden (en)" python3 nvda/tools/bns_equiv.py --native "$LIB" --against=nvda/tools/golden/blazie_en.txt
[ -f "$DATA/BL2SPA.BNS" ] && check "golden (es)" python3 nvda/tools/bns_equiv.py --native --es "$LIB" \
    --against=nvda/tools/golden/blazie_es.txt
check "chip defaults" python3 src/csrc/gen_chip_defaults.py --check
check "speech-dispatcher module" python3 src/platforms/speechd/test_sd_ssi263.py build/linux/sd_ssi263 "$LIB" "$DATA"
control "module CONTROL (no cancel, must fail)" "^7 of 10 checks passed" env SD_SSI263_TEST_NO_CANCEL=1 \
    python3 src/platforms/speechd/test_sd_ssi263.py build/linux/sd_ssi263 "$LIB" "$DATA"
# the Python wheel: built from build/linux, installed into a fresh venv, the Braille Lite as the library directly
check "Python wheel" python3 python/test_wheel.py --build "$DATA"
control "Python wheel CONTROL (rate 70, must fail)" "^wheel: 1 FAILED$" \
    env WHEEL_TEST_BREAK=1 python3 python/test_wheel.py --build "$DATA"
[ $fail -eq 0 ] && echo "all Linux checks passed ($PLAT)" || echo "Linux checks FAILED ($PLAT)"
exit $fail
