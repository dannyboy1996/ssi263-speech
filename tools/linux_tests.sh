#!/bin/sh
# The Linux gate (x86_64 or aarch64), run on the Linux box itself -- separate from nvda/tools/run_tests.py, which is
# Windows and NVDA.  Needs a build (./build_linux.sh; LEGACY=1 adds the z180emu reference checks and the audit's
# real-GPL control) and the unit's firmware in a data folder: the built NVDA add-on's engine folder, or any folder
# with BL2ENG.BNS + bl2_2003_warm.state (and BL2SPA.BNS + bl2spa_fresh.state; the emulator's checks also need the
# Type 'n Speak's TNSENG.TNS, in its tns/ folder or beside them).
#
#   tools/linux_tests.sh [data folder]      (default: nvda/dist/blazie-build/synthDrivers/_ssi263_blazie)
#
# Everything here runs the shipping library: the Braille Lite on MAME's Z180, through the NATIVE host (bl_host.c, with
# its default cancel protection, cancel_settle 3: Astra, Replies 124-129).  The goldens (nvda/tools/golden/
# blazie_{en,es}.txt: the native host's, made on Windows with the MAME bl.dll) compare byte for byte: writes and their
# times, serial and the audio hash.  The pipe host (bl_live) has its own cancel path and baseline, not checked here.
# The z180emu goldens (blazie_*_legacy.txt, made before the cancel protection) are REFERENCE checks only, labelled so:
# English's spoken values still match them write for write; Spanish's do not, by the two writes the protection adds
# after the 3.9 s cancel (R1=40, R0=C0), so it is not compared; the times legitimately differ.  The module
# harness checks speech-dispatcher's protocol and every message's audio, and its control must fail; the no-GPL audit
# (tools/check_no_gpl.py) searches the library, the module, the package and the wheel, and its controls must fail.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$(cd "${1:-$ROOT/nvda/dist/blazie-build/synthDrivers/_ssi263_blazie}" && pwd)"
PLAT="$(python3 -c 'import sys, platform; print("%s-%s" % (sys.platform, platform.machine()))')"
LIB="$ROOT/src/ssi263/_bin/$PLAT/libssi263speech.so"
LEGACY="$ROOT/build/linux/legacy"
export SSI263_LIB="$LIB" PYTHONDONTWRITEBYTECODE=1 PYTHON_COLORS=0
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
check "two units in one process (MAME Z180)" ./build/linux/test_bl_board "$DATA/BL2ENG.BNS" \
    "$DATA/bl2_2003_warm.state" "$DATA/BL2SPA.BNS" "$DATA/bl2spa_fresh.state"
check "CPU contract tests (MAME Z180 core)" ./build/linux/test_z180_contract
check "white-box tests (MAME Z180 core)" ./build/linux/test_z180_whitebox
check "CPU contract tests (MAME 8085 core)" ./build/linux/test_i8085_contract
check "CPU contract tests (MAME 8086 core)" ./build/linux/test_i86_contract
check "golden, native host (en)" python3 nvda/tools/bns_equiv.py --native "$LIB" \
    --against=nvda/tools/golden/blazie_en.txt
[ -f "$DATA/BL2SPA.BNS" ] && check "golden, native host (es)" python3 nvda/tools/bns_equiv.py --native --es "$LIB" \
    --against=nvda/tools/golden/blazie_es.txt
# the full comparison sees a different core: the shipping library against z180emu's golden must fail
control "golden CONTROL (the z180emu golden, must fail)" "^DIFFERS from blazie_en_legacy\.txt at line [0-9]+" \
    -- python3 nvda/tools/bns_equiv.py --native "$LIB" --against=nvda/tools/golden/blazie_en_legacy.txt
# REFERENCE (z180emu, not shipped): what English speaks, write for write, is z180emu's; only the times moved.  (Not
# Spanish: the cancel protection adds two writes after its 3.9 s cancel, R1=40 and R0=C0 -- Astra, Reply 126.)
check "reference: spoken values = z180emu's (en)" python3 nvda/tools/bns_equiv.py --native --values-only "$LIB" \
    --against=nvda/tools/golden/blazie_en_legacy.txt
control "reference CONTROL (one value flipped, must fail)" \
    "^write values DIFFER from blazie_en_legacy\.txt at write [0-9]+ of" \
    -- env BNS_EQUIV_FLIP=1 python3 nvda/tools/bns_equiv.py --native --values-only "$LIB" \
    --against=nvda/tools/golden/blazie_en_legacy.txt
if [ -x "$LEGACY/test_z180_legacy" ]; then
    check "reference: z180emu legacy path exceptions (development build)" "$LEGACY/test_z180_legacy"
else
    echo "skip  reference: the z180emu development build (LEGACY=1 ./build_linux.sh)"
fi
check "chip defaults" python3 src/csrc/gen_chip_defaults.py --check
check "speech-dispatcher module" python3 src/platforms/speechd/test_sd_ssi263.py build/linux/sd_ssi263 "$LIB" "$DATA"
control "module CONTROL (no cancel, must fail)" "^speak +module .*identical" "^stop +module .*identical" \
    "^after +module .*DIFFER" "^set +module .*DIFFER" "^key +module .*DIFFER" "^spanish +module .*identical" \
    "^7 of 10 checks passed" -- env SD_SSI263_TEST_NO_CANCEL=1 \
    python3 src/platforms/speechd/test_sd_ssi263.py build/linux/sd_ssi263 "$LIB" "$DATA"
# The Blazie emulator in a terminal (src/apps/blazie/README-linux.md), on MAME's Z180: its keyboard without a unit
# (test_keys), the unit headless as on Windows (test_emu_unit, test_clock), and the whole program headless
# (test_emu_linux.py: boot and a chord answered, the clock from the system time typed as keys and as computer-braille
# letters, the Type 'n Speak from cold, its memory saved and started from, a chord held through a restart).  The
# Type 'n Speak's firmware: $DATA/tns/TNSENG.TNS or $DATA/TNSENG.TNS.  The controls swap dots 1 and 4
# (BLAZIE_KEYS_BREAK) and drop the held keys (TEST_CLOCK_HOLD_BREAK): each must fail its own checks.
EMU=build/linux/blazie_emu
TNS="$DATA/tns/TNSENG.TNS"; [ -f "$TNS" ] || TNS="$DATA/TNSENG.TNS"
check "emulator: the keyboard (terminal, chords, letters, hold, Type 'n Speak)" ./build/linux/test_keys build
control "emulator: keyboard CONTROL (dots 1 and 4 swapped, must fail)" "^FAIL +keys mode: o-chord, then t" \
    "^FAIL +letters mode: computer braille" "^ok +tns: y " "^ok +terminal: F12" "^test_keys: [0-9]+ of [0-9]+ FAILED$" \
    -- env BLAZIE_KEYS_BREAK=1 ./build/linux/test_keys build
check "emulator: the unit headless (Braille Lite)" ./build/linux/test_emu_unit bl "$DATA/BL2ENG.BNS" \
    "$DATA/bl2_2003_warm.state"
check "emulator: the unit headless (Type 'n Speak)" ./build/linux/test_emu_unit tns "$TNS" -
check "emulator: the clock controller" ./build/linux/test_clock unit
check "emulator: the clock and keys held at a restart (Braille Lite)" ./build/linux/test_clock bl "$DATA/BL2ENG.BNS" \
    "$DATA/bl2_2003_warm.state"
check "emulator: the clock (Type 'n Speak)" ./build/linux/test_clock tns "$TNS"
control "emulator: held keys CONTROL (never reported held, must fail)" "^FAIL i-chord held through the restart" \
    "^FAILED$" -- env TEST_CLOCK_HOLD_BREAK=1 ./build/linux/test_clock bl "$DATA/BL2ENG.BNS" \
    "$DATA/bl2_2003_warm.state" restart
if [ -x "$EMU" ]; then
    check "emulator: the program headless" python3 src/apps/blazie/test_emu_linux.py "$EMU" "$DATA"
    control "emulator: program CONTROL (dots 1 and 4 swapped, must fail)" \
        "^ok +boot greeting" "^FAIL +the clock, from the system time \(keys\)" \
        "^FAIL +the clock, from the system time \(letters\)" "^emulator: [23] of 4 FAILED$" \
        -- env BLAZIE_KEYS_BREAK=1 python3 src/apps/blazie/test_emu_linux.py "$EMU" "$DATA" \
        --only boot,clock-keys,clock-letters
    check "emulator: libraries needed (libc, libm, libasound/libpulse; libstdc++ inside)" sh -c "! ldd $EMU | \
        grep -v -E 'linux-vdso|ld-linux|libc\.so|libm\.so|libpthread|libasound|libpulse|libdl' | grep -q . && \
        ! ldd $EMU | grep -q -E 'libstdc|libgcc_s' && echo 'only the C library and the sound library'"
else
    echo "FAIL  emulator: build/linux/blazie_emu not built (sudo apt install libasound2-dev, then ./build_linux.sh)"
    fail=1
fi
# the Python wheel: built from build/linux, installed into a fresh venv, the Braille Lite as the library directly
check "Python wheel" python3 python/test_wheel.py --build "$DATA"
control "Python wheel CONTROL (rate 70, must fail)" "^ok +the chip alone:" "^FAIL +the Braille Lite:" \
    "^wheel: 1 FAILED$" -- env WHEEL_TEST_BREAK=1 python3 python/test_wheel.py --build "$DATA"
# the no-GPL audit: what ships -- the library, the module, the package (built here, firmware included) and a wheel
rm -rf build/audit && mkdir -p build/audit
check "package" sh tools/package_linux.sh "$DATA"
check "wheel for the audit" python3 python/build_wheel.py --lib-dir build/linux --plat "linux_$(uname -m)" \
    --out build/audit
check "no z180emu or Unicorn engine, no GPL notice in what ships" python3 tools/check_no_gpl.py "$LIB" \
    build/linux/sd_ssi263 build/linux/blazie_emu build/ssi263-speech-*-linux-"$(uname -m)".tar.gz \
    build/audit/ssi263speech-*.whl
# the licences that must ship: MIT (ours, Casso's) and MAME's BSD-3-Clause for the Z180, in the package and the wheel
check "licences in the package and the wheel" sh -c "tar -tzf build/ssi263-speech-*-linux-$(uname -m).tar.gz | \
    grep -c -E '/(LICENSE|licenses/Casso-MIT\.txt|licenses/MAME-Z180-core-BSD-3-Clause\.txt)\$' | grep -qx 3 && \
    python3 -c 'import sys, zipfile; n = zipfile.ZipFile(sys.argv[1]).namelist(); sys.exit(sum(x.endswith(( \
    \".dist-info/LICENSE\", \".dist-info/Casso-MIT.txt\", \".dist-info/MAME-Z180-core-BSD-3-Clause.txt\")) \
    for x in n) != 3)' build/audit/ssi263speech-*.whl && echo 'MIT, Casso MIT, MAME Z180 BSD-3-Clause: in both'"
check "no-GPL audit: comments and MAME compatibility names are not evidence" python3 tools/check_no_gpl.py \
    --clean-sample
check "no build path in what ships" sh -c "! grep -a -q -F '$ROOT' '$LIB' build/linux/sd_ssi263 build/linux/blazie_emu"
control "no-GPL audit CONTROL (genuine legacy payloads, must fail)" \
    "^FAIL control\.apk: control\.apk!lib/arm64-v8a/libssi263speech\.so: z180emu engine" \
    "^FAIL control\.apk: control\.apk!lib/arm64-v8a/libssi263speech\.so: Unicorn engine" \
    "^FAIL control\.apk: control\.apk!lib/x86/unicorn\.dll: a legacy payload by name" \
    "^FAIL control\.apk: control\.apk!hosts/ucmini\.py: Unicorn import" \
    "^FAIL control\.apk: control\.apk!hosts/i8085\.py: a legacy payload by name" \
    "^FAIL control\.apk: control\.apk!assets/licenses/third-party\.txt: GPL notice" "^no-GPL audit: 1 of 1 FAILED$" \
    -- python3 tools/check_no_gpl.py --control
if [ -f "$LEGACY/libssi263speech_legacy.so" ]; then
    # the real thing, stripped as the APK's library is: what stripping keeps must still give z180emu away
    strip --strip-unneeded -o build/audit/libssi263speech_legacy.so "$LEGACY/libssi263speech_legacy.so"
    control "no-GPL audit CONTROL (the stripped z180emu library, must fail)" \
        "^FAIL libssi263speech_legacy\.so: libssi263speech_legacy\.so: z180emu engine" \
        "^no-GPL audit: 1 of 1 FAILED$" \
        -- python3 tools/check_no_gpl.py build/audit/libssi263speech_legacy.so
else
    echo "skip  no-GPL audit CONTROL on the real z180emu library (LEGACY=1 ./build_linux.sh)"
fi
# and the judgement itself: wrong failures, crashes and silent exits never pass as controls
check "control guard" sh tools/linux_control_guard.sh
[ $fail -eq 0 ] && echo "all Linux checks passed ($PLAT)" || echo "Linux checks FAILED ($PLAT)"
exit $fail
