#!/bin/bash
set -e
MODULE_ID="fouille"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."

# Build Docker image with sources COPYed in (Windows volume mount workaround)
docker build -t schwung-fouille-builder -f "$SCRIPT_DIR/Dockerfile" "$ROOT"

mkdir -p "$ROOT/dist/$MODULE_ID"

# Compile inside container, then extract with docker cp.
# fouille_probe (step 0 network check) is static, so it runs whatever glibc the Move has.
CONTAINER_ID=$(docker create schwung-fouille-builder \
  bash -c "mkdir -p /build/dist/$MODULE_ID && \
    aarch64-linux-gnu-gcc \
      -O2 -shared -fPIC \
      -o /build/dist/$MODULE_ID/dsp.so \
      /build/src/dsp/$MODULE_ID.c \
      -lm -lpthread && \
    aarch64-linux-gnu-gcc \
      -O2 -static \
      -o /build/dist/fouille_probe \
      /build/tools/fouille_probe.c \
      -lm && \
    cp /build/src/module.json /build/dist/$MODULE_ID/ && \
    tar -czf /build/dist/$MODULE_ID-module.tar.gz -C /build/dist $MODULE_ID/")

docker start -a "$CONTAINER_ID"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/dsp.so" "$ROOT/dist/$MODULE_ID/"
docker cp "$CONTAINER_ID:/build/src/module.json" "$ROOT/dist/$MODULE_ID/"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID-module.tar.gz" "$ROOT/dist/"
docker cp "$CONTAINER_ID:/build/dist/fouille_probe" "$ROOT/dist/"
docker rm "$CONTAINER_ID" > /dev/null

echo "Built: dist/$MODULE_ID-module.tar.gz + dist/fouille_probe"
