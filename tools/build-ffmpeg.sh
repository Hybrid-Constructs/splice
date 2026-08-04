#!/usr/bin/env bash
#
# Builds the static FFmpeg that Splice links against.
#
# Stock FFmpeg builds (including Homebrew's) ship the Hap *decoder* but not the
# *encoder*: hap_encoder depends on external libsnappy, which most builds leave
# out. So we build our own, with just the Hap encoder and MOV muxer enabled to
# keep it small.
#
# Result: external/ffmpeg-install/{lib,include}. Only needs running once.

set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-6.1.1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$ROOT/external/ffmpeg-src"
INSTALL_DIR="$ROOT/external/ffmpeg-install"
MIN_MACOS="${MIN_MACOS:-13.0}"

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "pkg-config not found. Install it with: brew install pkg-config" >&2
    exit 1
fi

if ! pkg-config --exists snappy 2>/dev/null \
   && [ ! -f /opt/homebrew/lib/libsnappy.a ] \
   && [ ! -f /usr/local/lib/libsnappy.a ]; then
    echo "libsnappy not found. Install it with: brew install snappy" >&2
    exit 1
fi

if [ ! -f /opt/homebrew/lib/libx264.a ] && [ ! -f /usr/local/lib/libx264.a ]; then
    echo "libx264 not found. Install it with: brew install x264" >&2
    exit 1
fi

# Homebrew prefix differs between Apple Silicon and Intel.
if [ -d /opt/homebrew ]; then
    BREW_PREFIX=/opt/homebrew
else
    BREW_PREFIX=/usr/local
fi

if [ ! -d "$SRC_DIR" ]; then
    echo "==> Fetching FFmpeg $FFMPEG_VERSION"
    mkdir -p "$ROOT/external"
    curl -L "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
        -o "$ROOT/external/ffmpeg.tar.xz"
    mkdir -p "$SRC_DIR"
    tar xf "$ROOT/external/ffmpeg.tar.xz" -C "$SRC_DIR" --strip-components=1
    rm -f "$ROOT/external/ffmpeg.tar.xz"
fi

cd "$SRC_DIR"

echo "==> Configuring"
./configure \
    --prefix="$INSTALL_DIR" \
    --disable-shared --enable-static --enable-pic \
    --disable-programs --disable-doc --disable-x86asm \
    --disable-network --disable-encoders --disable-muxers \
    --disable-devices --disable-filters --disable-postproc \
    --disable-avdevice --disable-avfilter \
    --enable-swscale --enable-swresample \
    --enable-libsnappy --enable-encoder=hap --enable-muxer=mov \
    --enable-gpl --enable-libx264 --enable-encoder=libx264 --enable-muxer=mp4 \
    --extra-cflags="-mmacosx-version-min=${MIN_MACOS} -I${BREW_PREFIX}/include" \
    --extra-ldflags="-mmacosx-version-min=${MIN_MACOS} -L${BREW_PREFIX}/lib"

if ! grep -q "^#define CONFIG_HAP_ENCODER 1" config_components.h; then
    echo "Hap encoder did not get enabled — check that libsnappy is installed." >&2
    exit 1
fi

if ! grep -q "^#define CONFIG_LIBX264_ENCODER 1" config_components.h; then
    echo "libx264 encoder did not get enabled — check that x264 is installed." >&2
    exit 1
fi

echo "==> Building"
make -j"$(sysctl -n hw.ncpu)"
make install

echo "==> Done: $INSTALL_DIR"
