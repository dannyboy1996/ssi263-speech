#!/bin/sh
# Remove what install.sh added: the module, its settings, the firmware folder and the library, and the voice's lines
# in speechd.conf (a DefaultModule install.sh commented out is restored).
#
#   sudo ./uninstall.sh            (PREFIX as at install time)
set -e
PREFIX="${PREFIX:-/usr/local}"
USER_HOME="$HOME"
[ -n "$SUDO_USER" ] && USER_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
for CONF in "$USER_HOME/.config/speech-dispatcher/speechd.conf" /etc/speech-dispatcher/speechd.conf; do
    [ -f "$CONF" ] || continue
    if grep -q 'ssi263' "$CONF"; then
        # the blank line an earlier installer put before the comment line, then the comment and module lines
        sed -i -e '/^$/{N;s/^\n\(# --- the Braille Lite 2000 voice (ssi263-speech install.sh) ---\)$/\1/}' "$CONF"
        sed -i -e '/^# --- the Braille Lite 2000 voice (ssi263-speech install.sh) ---$/d' \
               -e '/^AddModule "ssi263"/d' -e '/^DefaultModule ssi263$/d' \
               -e 's/^# (before ssi263) DefaultModule/DefaultModule/' "$CONF"
        echo "cleaned $CONF"
    fi
    rm -f "$(dirname "$CONF")/modules/ssi263.conf"
done
rm -f /usr/lib/speech-dispatcher-modules/sd_ssi263 "$PREFIX/lib/libssi263speech.so"
rm -rf "$PREFIX/share/ssi263-speech"
echo "Removed.  Restart speech-dispatcher: killall speech-dispatcher"
