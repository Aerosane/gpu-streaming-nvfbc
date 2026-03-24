#!/bin/bash
# Build NvFBC GStreamer plugins: nvfbcsrc (raw capture) and nvfbcenc (capture+encode)
# Requires: GStreamer 1.24+, CUDA toolkit, NvFBC.h, nvEncodeAPI.h

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/opt/gstreamer/lib/x86_64-linux-gnu/gstreamer-1.0"

# Use bundled GStreamer's pkg-config
export PKG_CONFIG_PATH=/opt/gstreamer/lib/x86_64-linux-gnu/pkgconfig

# Build nvfbcsrc (raw CUDA capture source)
SRC="$SCRIPT_DIR/gstnvfbcsrc.c"
OUT="libgstnvfbcsrc.so"
CFLAGS=$(pkg-config --cflags gstreamer-cuda-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
LIBS=$(pkg-config --libs gstreamer-cuda-1.0 gstreamer-base-1.0 gstreamer-video-1.0)

gcc -shared -fPIC -O2 -o "$OUT" "$SRC" \
    -DPACKAGE=\"nvfbcsrc\" -DVERSION=\"1.0\" -DGST_USE_UNSTABLE_API \
    $CFLAGS $LIBS \
    -I/usr/local/cuda/include -L/usr/local/cuda/lib64 -lcuda -ldl

echo "Built $OUT"

# Build nvfbcenc (NvFBC capture + NVENC encode in one element)
SRC_ENC="$SCRIPT_DIR/gstnvfbcenc.c"
OUT_ENC="libgstnvfbcenc.so"
if [ -f "$SRC_ENC" ]; then
    CFLAGS_BASE=$(pkg-config --cflags gstreamer-base-1.0 gstreamer-video-1.0)
    LIBS_BASE=$(pkg-config --libs gstreamer-base-1.0 gstreamer-video-1.0)

    gcc -shared -fPIC -O2 -o "$OUT_ENC" "$SRC_ENC" \
        -DPACKAGE=\"nvfbcenc\" -DVERSION=\"1.0\" \
        $CFLAGS_BASE $LIBS_BASE \
        -I"$SCRIPT_DIR" -I/usr/local/cuda/include -L/usr/local/cuda/lib64 -lcuda -ldl

    echo "Built $OUT_ENC"
fi

if [ "$1" = "--install" ]; then
    sudo cp "$OUT" "$INSTALL_DIR/"
    echo "Installed $OUT → $INSTALL_DIR/"
    if [ -f "$OUT_ENC" ]; then
        sudo cp "$OUT_ENC" "$INSTALL_DIR/"
        echo "Installed $OUT_ENC → $INSTALL_DIR/"
    fi
fi
