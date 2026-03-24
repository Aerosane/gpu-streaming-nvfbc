#!/bin/bash
# Start Wayland desktop stack in Ubuntu 26.04 container
set -e

export HOME=/root
export LANG=C.UTF-8
export XDG_RUNTIME_DIR=/run/user/0
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE

mkdir -p $XDG_RUNTIME_DIR
chmod 700 $XDG_RUNTIME_DIR

# Create NVIDIA device nodes if needed
for dev in nvidia0:195:0 nvidiactl:195:255 nvidia-modeset:195:254 nvidia-uvm:235:0; do
    IFS=: read name major minor <<< "$dev"
    [ ! -e "/dev/$name" ] && mknod -m 666 "/dev/$name" c $major $minor 2>/dev/null || true
done
mkdir -p /dev/nvidia-caps
[ ! -e /dev/nvidia-caps/nvidia-cap1 ] && mknod -m 444 /dev/nvidia-caps/nvidia-cap1 c 238 1 2>/dev/null || true
[ ! -e /dev/nvidia-caps/nvidia-cap2 ] && mknod -m 444 /dev/nvidia-caps/nvidia-cap2 c 238 2 2>/dev/null || true

# NVIDIA Vulkan ICD
mkdir -p /etc/vulkan/icd.d
cat > /etc/vulkan/icd.d/nvidia_icd.json << 'EOF'
{"file_format_version":"1.0.0","ICD":{"library_path":"libGLX_nvidia.so.0","api_version":"1.3.275"}}
EOF
export VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json

# Start D-Bus
eval $(dbus-launch --sh-syntax)
export DBUS_SESSION_BUS_ADDRESS

# Start PipeWire
export PIPEWIRE_LATENCY=128/48000
pipewire &
sleep 1
wireplumber &
sleep 1
pipewire-pulse &
sleep 1

# Start KWin Wayland
export WAYLAND_DISPLAY=wayland-0
kwin_wayland --no-lockscreen --virtual --width 1920 --height 1080 > /tmp/kwin.log 2>&1 &
sleep 3

# Start XDG portals
/usr/libexec/xdg-desktop-portal > /tmp/portal.log 2>&1 &
sleep 1
/usr/lib/x86_64-linux-gnu/libexec/xdg-desktop-portal-kde > /tmp/portal-kde.log 2>&1 &
sleep 1

# Start Plasma shell
plasmashell --no-respawn > /tmp/plasma.log 2>&1 &
sleep 2

echo "Wayland desktop stack started"
echo "DBUS=$DBUS_SESSION_BUS_ADDRESS"
echo "WAYLAND=$WAYLAND_DISPLAY"

# Start Selkies-GStreamer
export GI_TYPELIB_PATH=/usr/lib/x86_64-linux-gnu/girepository-1.0
export SELKIES_ENCODER=nvh265enc
export SELKIES_FRAMERATE=144

python3 -m selkies \
  --framerate 144 \
  --encoder nvh265enc \
  --port 8888 \
  --wayland-socket-index 0 \
  > /tmp/selkies.log 2>&1 &

echo "Selkies started on port 8888"

# Keep alive
wait
