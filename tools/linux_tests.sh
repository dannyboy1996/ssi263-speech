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
. "$ROOT/tools/linux_control.sh"          # control(): a must-fail control, judged by its marks
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
control "module CONTROL (no cancel, must fail)" "^speak +module .*identical" "^stop +module .*identical" \
    "^after +module .*DIFFER" "^set +module .*DIFFER" "^key +module .*DIFFER" "^spanish +module .*identical" \
    "^7 of 10 checks passed" -- env SD_SSI263_TEST_NO_CANCEL=1 \
    python3 src/platforms/speechd/test_sd_ssi263.py build/linux/sd_ssi263 "$LIB" "$DATA"
# the Python wheel: built from build/linux, installed into a fresh venv, the Braille Lite as the library directly
check "Python wheel" python3 python/test_wheel.py --build "$DATA"
control "Python wheel CONTROL (rate 70, must fail)" "^ok +the chip alone:" "^FAIL +the Braille Lite:" \
    "^wheel: 1 FAILED$" -- env WHEEL_TEST_BREAK=1 python3 python/test_wheel.py --build "$DATA"
# and the judgement itself: wrong failures, crashes and silent exits never pass as controls
check "control guard" sh tools/linux_control_guard.sh
[ $fail -eq 0 ] && echo "all Linux checks passed ($PLAT)" || echo "Linux checks FAILED ($PLAT)"
exit $fail
