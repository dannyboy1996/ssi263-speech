#!/bin/sh
# Package the Linux release for this machine's architecture, as the NVDA add-on is packaged: the speech-dispatcher
# module, the library, the Braille Lite's firmware (shared with permission, never in the repository: from a folder
# you pass), the installer, and -- for z180emu's GPL -- the complete corresponding source of these binaries.
#
#   ./build_linux.sh && tools/package_linux.sh <firmware folder> [version]
#
# The firmware folder holds BL2ENG.BNS + bl2_2003_warm.state, and BL2SPA.BNS + bl2spa_fresh.state when the Spanish
# unit is to ship (the NVDA add-on's engine folder has them).  Output: build/ssi263-speech-<version>-linux-<arch>.tar.gz
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$(cd "$1" && pwd)"
VERSION="${2:-0.7.0}"
ARCH="$(uname -m)"
NAME="ssi263-speech-$VERSION-linux-$ARCH"
Z180="${Z180EMU:-$ROOT/third_party/z180emu}"
STAGE="$ROOT/build/package/$NAME"

for f in "$ROOT/build/linux/sd_ssi263" "$ROOT/build/linux/libssi263speech.so" "$FW/BL2ENG.BNS" "$FW/bl2_2003_warm.state"; do
    [ -f "$f" ] || { echo "missing: $f"; exit 1; }
done
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/ssi263-speech/speech-dispatcher" "$STAGE/source"
cp "$ROOT/build/linux/sd_ssi263" "$STAGE/bin/"
cp "$ROOT/build/linux/libssi263speech.so" "$STAGE/lib/"
cp "$FW/BL2ENG.BNS" "$FW/bl2_2003_warm.state" "$STAGE/share/ssi263-speech/"
if [ -f "$FW/BL2SPA.BNS" ] && [ -f "$FW/bl2spa_fresh.state" ]; then
    cp "$FW/BL2SPA.BNS" "$FW/bl2spa_fresh.state" "$STAGE/share/ssi263-speech/"
fi
cat > "$STAGE/share/ssi263-speech/speech-dispatcher/ssi263.conf" <<'EOF'
# sd_ssi263: the Braille Lite 2000 through an emulated SSI-263.  install.sh writes SSI263DataDir.
# SSI263Inflection 1          the unit's voice inflection (0: off)
# SSI263Whine "off"           off | hiss | whine: the unit's idle sound
# SSI263Tone 7                0-26, the unit's tone (factory 7)
# SSI263ShortPauses 1         sentences packed onto one line from the second on
# SSI263SampleRate 22050
EOF
cp "$ROOT/src/platforms/speechd/install.sh" "$ROOT/src/platforms/speechd/uninstall.sh" "$STAGE/"
cp "$ROOT/src/platforms/speechd/README-linux.md" "$STAGE/README.md"
cp "$ROOT/LICENSE" "$STAGE/LICENSE"
cp "$Z180/COPYING" "$STAGE/COPYING.z180emu"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh" "$STAGE/bin/sd_ssi263"
# GPLv2 (z180emu): the complete source these binaries were built from, and how
(cd "$ROOT" && tar -czf "$STAGE/source/ssi263-speech-$VERSION-source.tar.gz" build_linux.sh LICENSE src/csrc \
    src/platforms -C "$(dirname "$Z180")" "$(basename "$Z180")/z180" "$(basename "$Z180")/COPYING")
cat > "$STAGE/source/BUILD.txt" <<EOF
Built on $(uname -srm) with $(${CC:-cc} --version | head -1).
Unpack ssi263-speech-$VERSION-source.tar.gz, move its z180emu folder to third_party/z180emu, then ./build_linux.sh
EOF
(cd "$ROOT/build/package" && tar -czf "$ROOT/build/$NAME.tar.gz" "$NAME")
echo "packaged build/$NAME.tar.gz"
