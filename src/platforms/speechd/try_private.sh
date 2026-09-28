#!/bin/sh
# Try sd_ssi263 through a real speech-dispatcher without installing anything: a private server (its own config,
# module folder and socket) with sd_ssi263 as its only module.  The system's speech-dispatcher is left alone.
#
#   src/platforms/speechd/try_private.sh <data folder> ["text to say"] [out.wav]
#
# The data folder holds BL2ENG.BNS + bl2_2003_warm.state (and optionally BL2SPA.BNS + bl2spa_fresh.state): the
# unit's firmware, which is not part of this repository.  With out.wav, what the sound server plays is recorded from
# its monitor (PulseAudio / PipeWire: parec) while the text is spoken.
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DATA="$(cd "$1" && pwd)"
TEXT="${2:-Hello. This is the Braille Lite 2000, speaking through speech-dispatcher.}"
OUTWAV="$3"
MOD="$ROOT/build/linux/sd_ssi263"
[ -x "$MOD" ] || { echo "build it first: ./build_linux.sh"; exit 1; }

DIR="$(mktemp -d)"
SOCK="$DIR/speechd.sock"
mkdir -p "$DIR/conf/modules" "$DIR/modules" "$DIR/log"
cp "$MOD" "$DIR/modules/"
cat > "$DIR/conf/speechd.conf" <<EOF
CommunicationMethod "unix_socket"
AudioOutputMethod "pulse"
AddModule "ssi263" "sd_ssi263" "ssi263.conf"
DefaultModule ssi263
DefaultLanguage "en"
EOF
cat > "$DIR/conf/modules/ssi263.conf" <<EOF
SSI263DataDir "$DATA"
EOF

SERVER=""
REC=""
cleanup() {
    [ -n "$REC" ] && kill "$REC" 2>/dev/null
    [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null
    true
}
trap cleanup EXIT INT TERM
# its own pid file too: with the default one it refuses to start beside the desktop's ("already running")
speech-dispatcher -s -c unix_socket -S "$SOCK" -P "$DIR/speechd.pid" -C "$DIR/conf" -m "$DIR/modules" -L "$DIR/log" -t 20 &
SERVER=$!
i=0
while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$SOCK" ] || { echo "the private speech-dispatcher did not start (log: $DIR/log)"; exit 1; }

if [ -n "$OUTWAV" ]; then
    SINK="$(pactl get-default-sink 2>/dev/null || pactl info | sed -n 's/^Default Sink: //p')"
    parec --device="$SINK.monitor" --file-format=wav "$OUTWAV" &
    REC=$!
    sleep 0.3
fi
SPEECHD_ADDRESS="unix_socket:$SOCK" spd-say -w -o ssi263 "$TEXT"
if [ -n "$REC" ]; then
    sleep 0.5
    kill "$REC" 2>/dev/null || true
    wait "$REC" 2>/dev/null || true
    REC=""
    echo "recorded $OUTWAV"
fi
echo "server log: $DIR/log"
