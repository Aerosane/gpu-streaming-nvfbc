#!/bin/bash
# Full installation script: patches NVIDIA driver for NvFBC, builds plugin, patches Selkies
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Step 1: Patch NVIDIA driver for NvFBC ==="
if [ -f "$SCRIPT_DIR/nvidia-patches/patch-fbc.sh" ]; then
    bash "$SCRIPT_DIR/nvidia-patches/patch-fbc.sh"
else
    echo "WARN: patch-fbc.sh not found. Clone https://github.com/keylase/nvidia-patch and run patch-fbc.sh"
fi

echo ""
echo "=== Step 2: Build NvFBC GStreamer plugin ==="
cd "$SCRIPT_DIR/gstreamer-plugin"
bash build.sh --install

echo ""
echo "=== Step 3: Verify plugin loads ==="
source /opt/gstreamer/gst-env
gst-inspect-1.0 nvfbcsrc && echo "Plugin loaded OK" || echo "WARN: Plugin failed to load"

echo ""
echo "=== Step 4: Install Selkies patches ==="
SELKIES_APP="/home/vscode/.local/lib/python3.12/site-packages/selkies_gstreamer/gstwebrtc_app.py"
if [ -f "$SELKIES_APP" ]; then
    cp "$SELKIES_APP" "${SELKIES_APP}.bak"
    cp "$SCRIPT_DIR/selkies-patches/gstwebrtc_app.py" "$SELKIES_APP"
    echo "Selkies patched (backup at ${SELKIES_APP}.bak)"
else
    echo "WARN: Selkies not found at expected path"
fi

echo ""
echo "=== Step 5: Install configs ==="
cp "$SCRIPT_DIR/configs/selkies_config.json" /tmp/selkies_config.json
echo "Config installed"

echo ""
echo "=== Step 6: Install Xorg config ==="
sudo cp "$SCRIPT_DIR/desktop-config/xorg.conf" /etc/X11/xorg.conf
echo "Xorg config installed"

echo ""
echo "=== Done! Restart the desktop with start-gpu-desktop ==="
