#!/bin/bash
LOG=/workspace/setup.log
exec > "$LOG" 2>&1
set -e

echo ">>> [0/5] Creating NVIDIA device nodes..."
mknod -m 666 /dev/nvidia0 c 195 0 2>/dev/null || true
mknod -m 666 /dev/nvidiactl c 195 255 2>/dev/null || true
mknod -m 666 /dev/nvidia-uvm c 235 0 2>/dev/null || true
mknod -m 666 /dev/nvidia-modeset c 195 254 2>/dev/null || true

echo ">>> [1/5] Installing NVIDIA 580 driver libs..."
cp /workspace/nvidia-580-libs/*.so.580.126.20 /usr/lib/x86_64-linux-gnu/
cp /workspace/nvidia-580-libs/nvidia-smi /usr/bin/
chmod +x /usr/bin/nvidia-smi
cd /usr/lib/x86_64-linux-gnu
for f in *.580.126.20; do
  base="${f%.580.126.20}"
  ln -sf "$f" "${base}.1"
  ln -sf "$f" "${base}"
done
ln -sf libcuda.so.580.126.20 libcuda.so.1
ln -sf libcuda.so.580.126.20 libcuda.so
ldconfig
nvidia-smi --query-gpu=name,driver_version --format=csv,noheader
echo "GPU OK"

echo ">>> [2/5] Enabling all repos..."
cat > /etc/apt/sources.list.d/ubuntu.sources << 'APT'
Types: deb
URIs: http://archive.ubuntu.com/ubuntu
Suites: resolute resolute-updates resolute-backports
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://security.ubuntu.com/ubuntu
Suites: resolute-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
APT
apt-get update -qq
echo "REPOS OK"

echo ">>> [3/5] Installing Wayland + KDE 6 + GStreamer 1.28..."
DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire \
  pipewire pipewire-pulse wireplumber pipewire-audio-client-libraries \
  vulkan-tools \
  python3 python3-pip python3-gi python3-gst-1.0 \
  kwin-wayland plasma-desktop plasma-workspace xwayland \
  xdg-desktop-portal xdg-desktop-portal-kde \
  dbus-x11 dbus-user-session \
  fastfetch curl wget git \
  libnvidia-egl-wayland1
echo "PACKAGES OK"

echo ">>> [4/5] Installing Selkies-GStreamer..."
pip3 install --break-system-packages selkies-gstreamer
echo "SELKIES OK"

echo ">>> [5/5] Verification..."
echo "GStreamer: $(gst-inspect-1.0 --version 2>/dev/null | head -1)"
echo "Vulkan elements:"
gst-inspect-1.0 2>/dev/null | grep -i vulkan || echo "(none)"
echo "PipeWire: $(pipewire --version 2>/dev/null | head -1)"
echo "KWin: $(kwin_wayland --version 2>/dev/null)"
nvidia-smi --query-gpu=name --format=csv,noheader
fastfetch 2>/dev/null | head -20
echo ">>> ALL DONE"
