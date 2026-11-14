#!/bin/bash
# Fouille, step 0 — run from a computer on the same network as the Move.
# The Move must be on wifi WITH internet (USB alone gives no internet).
#
#   ./scripts/build.sh        # builds dist/fouille_probe (static ARM64)
#   ./scripts/netcheck.sh     # copies probe + script, runs them, brings the log back
#
# Optional: put your Freesound key in ./freesound.key to also test the GPS search.
set -e
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-root}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR=/data/UserData/fouille-check
PROBE="$ROOT/dist/fouille_probe"

[ -f "$PROBE" ] || { echo "missing $PROBE: run ./scripts/build.sh first"; exit 1; }

echo "copying to $MOVE_USER@$MOVE_HOST:$DIR ..."
ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $DIR"
FILES=("$PROBE" "$ROOT/scripts/device-netcheck.sh")
[ -f "$ROOT/freesound.key" ] && FILES+=("$ROOT/freesound.key")
scp "${FILES[@]}" "$MOVE_USER@$MOVE_HOST:$DIR/"
ssh "$MOVE_USER@$MOVE_HOST" "chmod +x $DIR/fouille_probe && sh $DIR/device-netcheck.sh"

OUT="$ROOT/netcheck-$(date +%Y%m%d-%H%M).log"
scp "$MOVE_USER@$MOVE_HOST:$DIR/netcheck.log" "$OUT"
# The key never stays on the Move from this script.
ssh "$MOVE_USER@$MOVE_HOST" "rm -f $DIR/freesound.key"
echo
echo "log saved: $OUT"
