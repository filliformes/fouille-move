#!/bin/bash
# Deploy Fouille (Overtake) to the Move by ATOMIC RENAME: writing over a
# mapped .so truncates it under the live mapping and takes Move's audio down.
# Close Fouille first (full exit: Shift + Volume + Jog), then run this.
set -euo pipefail
MODULE_ID="fouille"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/overtake/$MODULE_ID"
STAGE="/data/UserData/.fouille-stage"

[ -f "dist/$MODULE_ID/dsp.so" ] || { echo "no dist/$MODULE_ID/dsp.so: run ./scripts/build.sh first"; exit 1; }
echo "staging to $MOVE_HOST ..."
ssh -o ConnectTimeout=8 "root@$MOVE_HOST" "mkdir -p $STAGE"
scp -o ConnectTimeout=8 "dist/$MODULE_ID/dsp.so" "dist/$MODULE_ID/module.json" "dist/$MODULE_ID/ui.js" "root@$MOVE_HOST:$STAGE/"
ssh -o ConnectTimeout=8 "root@$MOVE_HOST" "set -e
  mkdir -p $DEST
  for f in dsp.so module.json ui.js; do mv -f $STAGE/\$f $DEST/\$f; done
  rmdir $STAGE
  # the Freesound key moves with the module (it stays out of the repo)
  OLD=/data/UserData/schwung/modules/sound_generators/$MODULE_ID/freesound.key
  [ -f $DEST/freesound.key ] || { [ -f \$OLD ] && cp \$OLD $DEST/freesound.key; } || true
  chown -R ableton:users $DEST
  chmod +x $DEST/dsp.so
  ls -l $DEST"
echo
echo "Installed. On the Move: open Fouille from the Overtake list."
echo "If it was already open, FULL-EXIT first (Shift + Volume + Jog):"
echo "Back only suspends, and would resume the old code."
