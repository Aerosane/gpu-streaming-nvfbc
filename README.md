# gpu-streaming

GStreamer plugins and patches for GPU-only remote desktop streaming. The main piece is `nvfbcenc` — a single GStreamer element that does NvFBC capture + NVENC encode without frames ever touching CPU memory.

## nvfbcenc

`gstreamer-plugin/` — combined capture+encode element.

NvFBC grabs the X11 framebuffer as a CUDA surface, NVENC encodes it in-place. No `cudadownload`, no CPU copies. Supports H264 and HEVC, dynamic resolution changes, cursor compositing.

```
gst-launch-1.0 nvfbcenc display=:0 preset=p7 bitrate=25000 \
  ! video/x-h265,framerate=144/1 ! rtph265pay ! webrtcbin
```

Build with meson:
```
cd gstreamer-plugin && meson setup build && ninja -C build
cp build/libgstnvfbcenc.so /usr/lib/x86_64-linux-gnu/gstreamer-1.0/
```

## other stuff

- `gamepad-bridge.c` — uinput virtual gamepad, receives from browser over DGRAM unix socket
- `nvidia-patches/` — NvFBC unlock for consumer GPUs + container fixes
- `selkies-patches/` — HEVC support, resize handling, WebRTC fixes for selkies-gstreamer
- `ntsync.c` — out-of-tree ntsync kernel module (Linux 6.14 backport for Wine/Proton)

Needs NVIDIA GPU with NvFBC (or patched driver), CUDA 12.x, GStreamer 1.20+.
