FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    libc6-dev-arm64-cross \
    binutils-aarch64-linux-gnu \
    gcc \
    libc6-dev \
    dos2unix \
    make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
# Sources are COPYed in, not mounted (Windows volume mount workaround).
COPY engine/ /build/engine/
COPY move/ /build/move/
COPY tests/ /build/tests/
COPY tools/ /build/tools/
