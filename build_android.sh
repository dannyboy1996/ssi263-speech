#!/bin/sh
# Build the Android app's native library, libssi263speech.so: the SSI-263 chip, the Braille Lite board (on MAME's
# Z180 core), the Braille Lite host and voice, the Aicom Accent SA's board, host and voice on MAME's 8085 core (C++17,
# with a static libc++ inside the one .so) -- the same sources and flags as build_linux.sh -- plus the app's front end
# (src/platforms/android/app/src/main/cpp), cross-built with the NDK's clang and dropped where Gradle packages
# prebuilt libraries.  Then it stages what the APK carries besides code: the Accent SA's ROMs (Aicom's, the one
# firmware the app ships: Tomi, 2026-09-30) and the licences: the project's MIT, and MAME's BSD-3-Clause notices for
# the Z180 and 8085 cores.  No GPL code (src/platforms/android/test/check_apk_no_firmware.py audits the APK).
# Not the Braille Lite's firmware: the app's users import their own.
#
#   sh build_android.sh                  arm64-v8a, armeabi-v7a and x86_64
#   sh build_android.sh arm64-v8a        one ABI
#   sh build_android.sh --test arm64-v8a the host-side test program for that ABI (run over adb)
#
# Found from the environment first, then paths.local (the key of the same name), then the default:
#   SSI263_FIRMWARE    only with SSI263_ANDROID_BUNDLE_FIRMWARE=1 (a developer build that carries the firmware):
#                      the folder with BL2ENG.BNS + bl2_2003_warm.state, and BL2SPA.BNS + bl2spa_fresh.state for
#                      the Spanish unit, there or in its spanish/ folder (default firmware/blazie)
#   ANDROID_NDK_HOME   the NDK (default: the newest under $ANDROID_HOME/ndk or $ANDROID_SDK_ROOT/ndk)
#
# Output: src/platforms/android/app/src/main/jniLibs/<abi>/libssi263speech.so (gitignored) and build/android/assets
# (gitignored), which Gradle packages.  Nothing here is committed.
set -e

# A Windows path on MSYS (pwd -W), because the NDK's clang is a Windows program and path conversion is switched off
# below, so a POSIX /c/... path would reach it unconverted.  As outspoken's build_android.sh.
ROOT="$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))"
APP="$ROOT/src/platforms/android/app/src/main"
OUT="$ROOT/build/android"
SRC="$ROOT/src/csrc"
API=26                                  # the app's minSdk

# paths.local: "KEY = value" lines (tools/repo_paths.py's format); backslashes become slashes for the shell
local_path() {
    [ -f "$ROOT/paths.local" ] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$ROOT/paths.local" | tail -1 | tr '\\' '/' | tr -d '\r'
}
FW="${SSI263_FIRMWARE:-$(local_path SSI263_FIRMWARE)}"
FW="${FW:-$ROOT/firmware/blazie}"

newest() { for p in "$@"; do [ -e "$p" ] && echo "$p"; done | sort -V | tail -1; }
NDK="${ANDROID_NDK_HOME:-}"
[ -n "$NDK" ] || NDK="$(newest "${ANDROID_HOME:-/nonexistent}"/ndk/* "${ANDROID_SDK_ROOT:-/nonexistent}"/ndk/* 2>/dev/null)"
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "no Android NDK found; set ANDROID_NDK_HOME"; exit 1; }
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.exe; NDK="$(cd "$NDK" && (pwd -W 2>/dev/null || pwd))" ;;
    Darwin) HOST=darwin-x86_64; EXE= ;;
    *) HOST=linux-x86_64; EXE= ;;
esac
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
[ -x "$BIN/clang$EXE" ] || { echo "no clang under $BIN"; exit 1; }
echo "NDK: $NDK"

# As build_linux.sh.  The chip: plain C99.  The board and host: gnu89, the board on MAME's Z180 (-DBL_Z180_MAME).
# -ffp-contract=off keeps the Python reference's arithmetic (no fused multiply-adds, which arm64 would otherwise
# use), so the PCM is the other platforms' byte for byte.
CHIP="-O2 -std=c99 -ffp-contract=off -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter"
BOARD="-O3 -std=gnu89 -ffp-contract=off -fPIC -fvisibility=hidden -DBL_Z180_MAME -w -I$SRC/blazie -I$SRC/cpu -I$SRC"
FRONT="-O2 -std=c99 -ffp-contract=off -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -I$SRC"
# MAME's cores (the Braille Lite's Z180, the Accent SA's 8085), as build_linux.sh builds them: C++17 without
# exceptions or RTTI; the Accent SA's board and host in gnu89.  libc++ is linked statically into the one .so
# (-static-libstdc++) and its symbols kept inside it.
Z180CXX="-O3 -std=c++17 -fno-exceptions -fno-rtti -ffp-contract=off -fPIC -fvisibility=hidden -Wall -I$SRC/cpu -I$SRC"
MAME="-O2 -std=c++17 -fno-exceptions -fno-rtti -ffp-contract=off -fPIC -fvisibility=hidden -Wall -Wno-sign-compare -I$SRC/cpu -I$SRC"
ACCENT="-O2 -std=gnu89 -ffp-contract=off -fPIC -fvisibility=hidden -Wall -I$SRC/cpu -I$SRC/accentsa -I$SRC"
SPEAKOUT="-O2 -std=gnu89 -ffp-contract=off -fPIC -fvisibility=hidden -Wall -I$SRC/cpu -I$SRC/speakout -I$SRC"
CPP="$APP/cpp"
AICOM="$ROOT/firmware/aicom-accent-sa"

target() {
    case "$1" in
        arm64-v8a)   echo "aarch64-linux-android$API" ;;
        armeabi-v7a) echo "armv7a-linux-androideabi$API" ;;
        x86_64)      echo "x86_64-linux-android$API" ;;
        *) echo "unknown ABI $1" >&2; exit 1 ;;
    esac
}

# MSYS would rewrite --target's argument as a path; nothing here is one.
cc() { MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 "$BIN/clang$EXE" "$@"; }
cxx() { MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 "$BIN/clang++$EXE" "$@"; }

objects() {
    ABI="$1"; TARGET="$(target "$ABI")"; O="$OUT/$ABI/obj"
    # a fresh folder: the library links every object in it, and one left by an older build (z180emu's bl_unity.o)
    # must never come back in
    rm -rf "$O"
    mkdir -p "$O"
    cc --target="$TARGET" $CHIP -c -o "$O/ssi263.o" "$SRC/ssi263.c"
    cc --target="$TARGET" $CHIP -c -o "$O/ssi263dsp.o" "$SRC/ssi263dsp.c"
    cxx --target="$TARGET" $Z180CXX -c -o "$O/z180_mame.o" "$SRC/cpu/z180_mame.cpp"
    cxx --target="$TARGET" $Z180CXX -c -o "$O/z180_asci.o" "$SRC/cpu/z180_asci.cpp"
    for f in bl_board flash29 bl_serial bl_idle bl_clock bl_host bl_voice bl_firmware bl_state; do
        cc --target="$TARGET" $BOARD -c -o "$O/$f.o" "$SRC/blazie/$f.c"
    done
    cxx --target="$TARGET" $MAME -c -o "$O/i8085_mame.o" "$SRC/cpu/i8085_mame.cpp"
    for f in as_board as_usart as_host; do
        cc --target="$TARGET" $ACCENT -c -o "$O/$f.o" "$SRC/accentsa/$f.c"
    done
    cc --target="$TARGET" $FRONT -c -o "$O/as_voice.o" "$SRC/accentsa/as_voice.c"
    cc --target="$TARGET" $FRONT -c -o "$O/numwords.o" "$SRC/numwords.c"
    cc --target="$TARGET" $FRONT -c -o "$O/accent_text.o" "$SRC/accent_text.c"
    cc --target="$TARGET" $FRONT -c -o "$O/ssa_map.o" "$CPP/ssa_map.c"
    cc --target="$TARGET" $FRONT -c -o "$O/ssa_engine.o" "$CPP/ssa_engine.c"
    # The Speak-Out (MAME's V40, src/csrc/speakout: its board, host and voice), compiled only, into its own folder so
    # that the library above (every object in obj/) does not take it in yet: a front end links these with obj/'s
    # numwords.o and chip objects (so_voice.h lists them).
    S="$OUT/$ABI/obj_speakout"
    rm -rf "$S"
    mkdir -p "$S"
    cxx --target="$TARGET" $MAME -c -o "$S/v40_mame.o" "$SRC/cpu/v40_mame.cpp"
    for f in so_board so_icu so_scu so_hex so_host; do
        cc --target="$TARGET" $SPEAKOUT -c -o "$S/$f.o" "$SRC/speakout/$f.c"
    done
    cc --target="$TARGET" $FRONT -c -o "$S/so_voice.o" "$SRC/speakout/so_voice.c"
    # the Accent-mini (src/csrc/accentmini: am_voice on MAME's 8086), compiled only, in its own folder: not linked
    # into the library yet (it needs accent_text.o and numwords.o above)
    rm -rf "$OUT/$ABI/obj_am"; mkdir -p "$OUT/$ABI/obj_am"
    cxx --target="$TARGET" $MAME -c -o "$OUT/$ABI/obj_am/i86_mame.o" "$SRC/cpu/i86_mame.cpp"
    for f in pc86/pc86 accentmini/am_host accentmini/am_voice; do
        cc --target="$TARGET" $ACCENT -I$SRC/pc86 -c -o "$OUT/$ABI/obj_am/${f##*/}.o" "$SRC/$f.c"
    done
}
ACCENT_OBJS="i8085_mame.o as_board.o as_usart.o as_host.o as_voice.o numwords.o accent_text.o"
BL_OBJS="z180_mame.o z180_asci.o bl_board.o flash29.o bl_serial.o bl_idle.o bl_clock.o bl_host.o bl_voice.o"

build_abi() {
    ABI="$1"; TARGET="$(target "$ABI")"; O="$OUT/$ABI/obj"
    echo "=== $ABI ==="
    objects "$ABI"
    cc --target="$TARGET" $FRONT -c -o "$O/ssa_jni.o" "$CPP/ssa_jni.c"
    mkdir -p "$APP/jniLibs/$ABI"
    rm -f "$O/test_android_native.o"
    # only the API and the JNI entry points are exported; the Z180 core's globals and libc++ stay inside
    cxx --target="$TARGET" -shared -static-libstdc++ -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 \
        -o "$OUT/$ABI/libssi263speech.so" "$O"/*.o -lm -llog
    # self-contained: nothing beyond Bionic's own libraries (no libc++_shared.so: the C++ runtime is inside)
    if "$BIN/llvm-readelf$EXE" -d "$OUT/$ABI/libssi263speech.so" | grep NEEDED | grep -v -E 'lib(c|m|dl|log)[.]so'; then
        echo "libssi263speech.so needs a library the APK does not carry"
        exit 1
    fi
    "$BIN/llvm-strip$EXE" --strip-unneeded -o "$APP/jniLibs/$ABI/libssi263speech.so" "$OUT/$ABI/libssi263speech.so"
    ls -l "$APP/jniLibs/$ABI/libssi263speech.so"
}

# The host-side test (src/platforms/android/test) for a device: a plain executable, run from /data/local/tmp over
# adb with no app installed (not -static: Bionic refuses a static executable whose TLS segment is under-aligned).
build_test() {
    ABI="$1"; TARGET="$(target "$ABI")"; O="$OUT/$ABI/obj"
    objects "$ABI"
    cc --target="$TARGET" $FRONT -I"$CPP" -c -o "$O/test_android_native.o" \
        "$ROOT/src/platforms/android/test/test_android_native.c"
    cxx --target="$TARGET" -static-libstdc++ -o "$OUT/$ABI/test_android_native" "$O/test_android_native.o" \
        "$O/ssa_engine.o" "$O/ssa_map.o" "$O/ssi263.o" "$O/ssi263dsp.o" \
        $(for f in $BL_OBJS $ACCENT_OBJS; do echo "$O/$f"; done) -lm
    rm -f "$O/test_android_native.o"
    echo "  -> build/android/$ABI/test_android_native"
}

# What the APK carries besides code, into build/android/assets (Gradle's assets folder for it)
stage_assets() {
    A="$OUT/assets"
    rm -rf "$A"
    mkdir -p "$A/licenses" "$A/aicom"
    # The Accent SA's ROMs, the built-in voice: Aicom's, in the repository with their notice (firmware/AICOM.txt).
    # check_apk_no_firmware.py lets exactly these three through, by their sha256.
    for f in u2.BIN u3.BIN u4.BIN; do
        [ -f "$AICOM/$f" ] || { echo "missing $AICOM/$f"; exit 1; }
        cp "$AICOM/$f" "$A/aicom/"
    done
    # No firmware: the app is where it can NOT ship, so its users import their own (SettingsActivity).  A developer
    # build may carry it, by asking: SSI263_ANDROID_BUNDLE_FIRMWARE=1 (never a release).
    if [ "${SSI263_ANDROID_BUNDLE_FIRMWARE:-0}" = 1 ]; then
        mkdir -p "$A/firmware"
        for f in BL2ENG.BNS bl2_2003_warm.state; do
            [ -f "$FW/$f" ] || { echo "missing firmware: $FW/$f (set SSI263_FIRMWARE)"; exit 1; }
            cp "$FW/$f" "$A/firmware/"
        done
        # the Spanish unit, when both of its files are there
        for d in "$FW" "$FW/spanish"; do
            if [ -f "$d/BL2SPA.BNS" ] && [ -f "$d/bl2spa_fresh.state" ]; then
                cp "$d/BL2SPA.BNS" "$d/bl2spa_fresh.state" "$A/firmware/"
                break
            fi
        done
        echo "DEVELOPER BUILD: the firmware is bundled; do not distribute this APK"
    fi
    cp "$ROOT/src/platforms/android/licenses/"*.txt "$A/licenses/"
    cp "$ROOT/LICENSE" "$A/licenses/ssi263-speech-MIT.txt"
    cp "$ROOT/firmware/AICOM.txt" "$A/licenses/Aicom-Accent-SA-notice.txt"
    cp "$SRC/cpu/mame_z180/LICENSE-BSD-3-Clause.txt" "$A/licenses/MAME-Z180-core-BSD-3-Clause.txt"
    cp "$SRC/cpu/mame_i8085/LICENSE-BSD-3-Clause.txt" "$A/licenses/MAME-8085-core-BSD-3-Clause.txt"
    cp "$ROOT/third_party/casso/LICENSE" "$A/licenses/Casso-MIT.txt"
    ls -R "$A" | head -20
}

if [ "$1" = "--test" ]; then
    build_test "${2:-arm64-v8a}"
    exit 0
fi
if [ -n "$1" ]; then build_abi "$1"; else build_abi arm64-v8a; build_abi armeabi-v7a; build_abi x86_64; fi
stage_assets
echo "done."
