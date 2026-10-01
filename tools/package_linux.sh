#!/bin/sh
# Package the Linux release for this machine's architecture, as the NVDA add-on is packaged: the speech-dispatcher
# module, the library, the Braille Lite's firmware (shared with permission, never in the repository: from a folder
# you pass), the installer, and the licences: the project's MIT, Casso's MIT (third_party/casso: the chip model draws
# on it) and MAME's BSD-3-Clause notice for the Z180 core (src/csrc/cpu/mame_z180).  No GPL code is inside (tools/check_no_gpl.py checks the package).
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
STAGE="$ROOT/build/package/$NAME"

for f in "$ROOT/build/linux/sd_ssi263" "$ROOT/build/linux/libssi263speech.so" "$FW/BL2ENG.BNS" "$FW/bl2_2003_warm.state"; do
    [ -f "$f" ] || { echo "missing: $f"; exit 1; }
done
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/ssi263-speech/speech-dispatcher" "$STAGE/licenses"
cp "$ROOT/build/linux/sd_ssi263" "$STAGE/bin/"
cp "$ROOT/build/linux/libssi263speech.so" "$STAGE/lib/"
cp "$FW/BL2ENG.BNS" "$FW/bl2_2003_warm.state" "$STAGE/share/ssi263-speech/"
if [ -f "$FW/BL2SPA.BNS" ] && [ -f "$FW/bl2spa_fresh.state" ]; then
    cp "$FW/BL2SPA.BNS" "$FW/bl2spa_fresh.state" "$STAGE/share/ssi263-speech/"
fi
# the Blazie emulator (src/apps/blazie/README-linux.md): bin/blazie_emu finds the firmware in ../share/ssi263-speech,
# the Type 'n Speak's in its tns folder (from the firmware folder's tns/, or beside the Braille Lite's)
if [ -f "$ROOT/build/linux/blazie_emu" ]; then
    cp "$ROOT/build/linux/blazie_emu" "$STAGE/bin/"
    chmod +x "$STAGE/bin/blazie_emu"
    cp "$ROOT/src/apps/blazie/README-linux.md" "$STAGE/README-blazie-emu.md"
    for t in TNSENG.TNS TNSSPA.TNS; do
        for src in "$FW/tns/$t" "$FW/$t"; do
            if [ -f "$src" ]; then
                mkdir -p "$STAGE/share/ssi263-speech/tns"
                cp "$src" "$STAGE/share/ssi263-speech/tns/"
                break
            fi
        done
    done
else
    echo "note: build/linux/blazie_emu not built, so not packaged"
fi
# the same emulator in a GTK window, for Orca (README-blazie-emu.md, "The desktop app"), when it was built (GTK 3's
# headers there), with a menu entry for the desktop; GTK is the system's own library, not packaged
if [ -f "$ROOT/build/linux/blazie_emu_gtk" ]; then
    cp "$ROOT/build/linux/blazie_emu_gtk" "$STAGE/bin/"
    chmod +x "$STAGE/bin/blazie_emu_gtk"
    mkdir -p "$STAGE/share/applications"
    cat > "$STAGE/share/applications/ssi263-blazie-emu.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Blazie emulator
GenericName=Braille Lite 2000 and Type 'n Speak
Comment=Blazie's Braille Lite 2000 or Type 'n Speak running its own firmware, with its SSI-263 voice
Exec=blazie_emu_gtk
Terminal=false
Categories=Utility;Accessibility;
Keywords=braille;notetaker;speech;Blazie;
EOF
else
    echo "note: build/linux/blazie_emu_gtk not built (no GTK 3 headers), so not packaged"
fi
# blazie_files: a saved unit's files from the command line (README-blazie-emu.md, "Files in and out"); no sound needed
if [ -f "$ROOT/build/linux/blazie_files" ]; then
    cp "$ROOT/build/linux/blazie_files" "$STAGE/bin/"
    chmod +x "$STAGE/bin/blazie_files"
fi
cat > "$STAGE/share/ssi263-speech/speech-dispatcher/ssi263.conf" <<'EOF'
# The Braille Lite 2000 voice (sd_ssi263): the unit's own firmware speaking through an emulated SSI-263.
#
# This file:      speech-dispatcher's modules folder (install.sh put it there and wrote SSI263DataDir below).
# Your own copy:  ~/.config/ssi263-speech/sd_ssi263.conf -- any line there wins over this file, needs no root,
#                 and survives reinstalling.  Same keys, same format.
#
# Uncomment a line and change it to override the default.
# After editing:  killall speech-dispatcher     (Orca reconnects by itself)

# Output sample rate in Hz: 11025, 22050 or 44100 (default 22050).  22 kHz keeps everything the chip produces;
# 44 kHz also keeps the clock images and the brightest hiss; 11 kHz sounds like the unit's own speaker.
# SSI263SampleRate 22050

# The unit's voice inflection, its own status-menu setting: 1 on (default), 0 off (questions stay flat).
# SSI263Inflection 1

# The faint sound a real unit makes under its speech: off (default), hiss (even volumes, the factory setting),
# or whine (odd volumes).
# SSI263Whine off

# The unit's tone, 0-26 (factory 7).
# SSI263Tone 7

# Sentences packed onto one line from the second on, for shorter pauses (1, default) or not (0).
# SSI263ShortPauses 1
EOF
cp "$ROOT/src/platforms/speechd/install.sh" "$ROOT/src/platforms/speechd/uninstall.sh" "$STAGE/"
cp "$ROOT/src/platforms/speechd/README-linux.md" "$STAGE/README.md"
cp "$ROOT/LICENSE" "$STAGE/LICENSE"
cp "$ROOT/src/csrc/cpu/mame_z180/LICENSE-BSD-3-Clause.txt" "$STAGE/licenses/MAME-Z180-core-BSD-3-Clause.txt"
cp "$ROOT/third_party/casso/LICENSE" "$STAGE/licenses/Casso-MIT.txt"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh" "$STAGE/bin/sd_ssi263"
(cd "$ROOT/build/package" && tar -czf "$ROOT/build/$NAME.tar.gz" "$NAME")
echo "packaged build/$NAME.tar.gz"
