#!/bin/bash
# Build the NvFBC zero-copy GStreamer source plugin
# Requires: GStreamer 1.24+ (with gst_cuda_allocator_alloc_wrapped), CUDA toolkit, NvFBC.h

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/gstnvfbcsrc.c"
OUT="libgstnvfbcsrc.so"
INSTALL_DIR="/usr/lib/x86_64-linux-gnu/gstreamer-1.0"

# Source GStreamer environment
source /opt/gstreamer/gst-env

export PKG_CONFIG_PATH=/opt/gstreamer/lib/x86_64-linux-gnu/pkgconfig

gcc -shared -fPIC -o "$OUT" "$SRC" \
    '-DPACKAGE="nvfbcsrc"' '-DVERSION="1.0"' -DGST_USE_UNSTABLE_API \
    $(pkg-config --cflags --libs gstreamer-cuda-1.0 gstreamer-base-1.0 gstreamer-video-1.0) \
    -I/usr/local/cuda/include -L/usr/local/cuda/lib64 -lcuda -ldl

echo "Built $OUT successfully"

if [ "$1" = "--install" ]; then
    sudo cp "$OUT" "$INSTALL_DIR/"
    echo "Installed to $INSTALL_DIR/$OUT"
fi
