#!/bin/bash
# Fouille: cross-compile the Overtake module (dsp.so) and the on-device tools
# for the Move (ARM64), and run the offline engine tests natively, in Docker.
# docker create + docker cp + an explicit exit-code check: on Windows Git Bash
# `set -e` does not see a failed compile inside the container.
set -euo pipefail
MODULE_ID="fouille"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMAGE=schwung-fouille-builder

if command -v node >/dev/null 2>&1; then          # a QuickJS parse error = a dead surface on the Move
  echo "[0/4] syntax-check ui.js"
  cp move/ui.js move/.ui.check.mjs
  node --check move/.ui.check.mjs || { rm -f move/.ui.check.mjs; echo "ERROR: ui.js has a syntax error"; exit 1; }
  rm -f move/.ui.check.mjs
fi

echo "[1/4] toolchain image (sources copied in)"
docker build -q -t "$IMAGE" -f scripts/Dockerfile . >/dev/null

echo "[2/4] compile + offline tests"
CID=$(MSYS_NO_PATHCONV=1 docker create "$IMAGE" bash -c '
  set -e
  E="engine/fe_engine.c engine/fe_harvest.c engine/fe_atlas.c"
  W="-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
  O="-O3 -ffast-math -fno-finite-math-only -std=gnu11"
  mkdir -p dist/fouille dist/tests
  gcc $O $W -Iengine tests/render_test.c $E -lm -lpthread -o /tmp/render_test
  /tmp/render_test
  aarch64-linux-gnu-gcc $O $W -shared -fPIC -Iengine -Imove -o dist/fouille/dsp.so move/fouille_move.c $E -lm -lpthread
  aarch64-linux-gnu-gcc $O -static -Iengine -o dist/tests/render_test tests/render_test.c $E -lm -lpthread
  aarch64-linux-gnu-gcc $O -static -Iengine -o dist/tests/net_test tests/net_test.c $E -lm -lpthread
  aarch64-linux-gnu-gcc -O2 -static -o dist/fouille_probe tools/fouille_probe.c -lm
  cp move/module.json move/ui.js dist/fouille/
  tar -czf dist/fouille-module.tar.gz -C dist fouille/
  echo BUILD_OK
')
docker start -a "$CID" 2>&1 | grep -v "minimp3.h" || true
EXIT=$(docker inspect "$CID" --format='{{.State.ExitCode}}')
if [ "$EXIT" != "0" ]; then echo "ERROR: build or tests failed (exit $EXIT)"; docker rm "$CID" >/dev/null; exit 1; fi

echo "[3/4] extract"
mkdir -p dist
rm -rf dist/fouille dist/tests
docker cp "$CID:/build/dist/." dist/
docker rm "$CID" >/dev/null

echo "[4/4] done"
ls -l dist/fouille dist/fouille-module.tar.gz
