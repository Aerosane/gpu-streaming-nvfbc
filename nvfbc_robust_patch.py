#!/usr/bin/env python3
"""
Robust NvFBC + 144Hz patch for selkies-gstreamer.

Fixes:
  1. gstwebrtc_app.py: Use nvfbcsrc (GPU zero-copy capture) instead of ximagesrc (CPU)
     - Auto-detects nvfbcsrc availability, falls back to ximagesrc
     - Skips cudaupload (nvfbcsrc outputs CUDA memory directly)
     - Uses unfixed caps (no width/height constraint) so resize renegotiates cleanly
     - Guards ximagesrc-only properties (show-pointer, remote, blocksize, use-damage, endx, endy)
     - Preserves CUDA memory caps in set_framerate() runtime updates

  2. resize.py: Always create+select 144Hz modes on resize
     - Discovers existing _144 modes from xrandr (regex includes suffixed names)
     - Prefers existing 144Hz mode; generates new 144Hz via cvt if needed
     - Falls back to 60Hz if cvt can't produce 144Hz modeline
     - curr_res detection strips _144 suffix for comparison with browser-requested WxH
"""
import re, sys

def patch_gstwebrtc_app(path):
    with open(path) as f:
        src = f.read()

    # ---------- 1. init: add using_nvfbc flag ----------
    old = "        self.ximagesrc = None\n        self.ximagesrc_caps = None"
    new = "        self.ximagesrc = None\n        self.ximagesrc_caps = None\n        self.using_nvfbc = False"
    assert old in src, "PATCH1: init block not found"
    src = src.replace(old, new, 1)

    # ---------- 2. stop_ximagesrc / start_ximagesrc: guard ximagesrc-only props ----------
    old = '            self.ximagesrc.set_property("endx", 0)\n            self.ximagesrc.set_property("endy", 0)'
    new = ('            if not self.using_nvfbc:\n'
           '                self.ximagesrc.set_property("endx", 0)\n'
           '                self.ximagesrc.set_property("endy", 0)')
    assert old in src, "PATCH2: endx/endy not found"
    src = src.replace(old, new, 1)

    # ---------- 3. build_video_pipeline: swap ximagesrc → nvfbcsrc ----------
    old = ('        self.ximagesrc = Gst.ElementFactory.make("ximagesrc", "x11")\n'
           '        ximagesrc = self.ximagesrc')
    new = ('        # NvFBC zero-copy GPU capture → CUDA memory output (zero CPU)\n'
           '        # Falls back to ximagesrc if nvfbcsrc plugin not available\n'
           '        _nvfbc = Gst.ElementFactory.make("nvfbcsrc", "x11")\n'
           '        if _nvfbc is not None:\n'
           '            self.ximagesrc = _nvfbc\n'
           '            self.using_nvfbc = True\n'
           '            logger.info("NvFBC: zero-copy CUDA screen capture active")\n'
           '        else:\n'
           '            self.ximagesrc = Gst.ElementFactory.make("ximagesrc", "x11")\n'
           '            logger.info("NvFBC unavailable, falling back to ximagesrc")\n'
           '        ximagesrc = self.ximagesrc')
    assert old in src, "PATCH3: ximagesrc creation not found"
    src = src.replace(old, new, 1)

    # ---------- 4. Guard ximagesrc-only properties ----------
    for prop in ["show-pointer", "remote", "blocksize", "use-damage"]:
        old = f'        ximagesrc.set_property("{prop}",'
        idx = src.find(old)
        assert idx != -1, f"PATCH4: property '{prop}' not found"
        eol = src.index('\n', idx)
        full_line = src[idx:eol]
        # Wrap with guard
        guarded = f'        if not self.using_nvfbc:\n    {full_line}'
        src = src[:idx] + guarded + src[eol:]

    # ---------- 5. Caps: use unfixed CUDA caps for nvfbcsrc (no width/height → resize OK) ----------
    old = '        self.ximagesrc_caps = Gst.caps_from_string("video/x-raw")\n'
    # Find the FIRST occurrence (in build_video_pipeline)
    idx = src.find(old)
    assert idx != -1, "PATCH5: ximagesrc_caps creation not found"
    new = ('        if self.using_nvfbc:\n'
           '            # No width/height constraint — nvfbcsrc auto-negotiates on screen resize\n'
           '            self.ximagesrc_caps = Gst.caps_from_string("video/x-raw(memory:CUDAMemory),format=BGRA")\n'
           '            logger.info("NvFBC caps: CUDA BGRA (skips cudaupload)")\n'
           '        else:\n'
           '            self.ximagesrc_caps = Gst.caps_from_string("video/x-raw")\n')
    src = src[:idx] + new + src[idx+len(old):]

    # ---------- 6. Skip cudaupload in pipeline elements when using NvFBC ----------
    for enc_name in ["nvh264enc", "nvh265enc", "nvav1enc"]:
        old_line = f"            pipeline_elements += [cudaupload, cudaconvert, cudaconvert_capsfilter, {enc_name},"
        idx = src.find(old_line)
        if idx == -1:
            continue
        eol = src.index('\n', idx)
        full = src[idx:eol]
        # Extract the tail part (encoder caps, rtp pay, rtp caps)
        after_upload = full.replace("            pipeline_elements += [cudaupload, ", "            pipeline_elements += [")
        new_line = (f"            if self.using_nvfbc:\n"
                    f"    {after_upload}\n"
                    f"            else:\n"
                    f"    {full}")
        src = src[:idx] + new_line + src[eol:]

    # ---------- 7. set_framerate: preserve CUDA memory caps on runtime update ----------
    old_fr = ('            self.ximagesrc_caps = Gst.caps_from_string("video/x-raw")\n'
              '            self.ximagesrc_caps.set_value("framerate", Gst.Fraction(self.framerate, 1))\n'
              '            self.ximagesrc_capsfilter.set_property("caps", self.ximagesrc_caps)')
    new_fr = ('            if self.using_nvfbc:\n'
              '                self.ximagesrc_caps = Gst.caps_from_string("video/x-raw(memory:CUDAMemory),format=BGRA")\n'
              '            else:\n'
              '                self.ximagesrc_caps = Gst.caps_from_string("video/x-raw")\n'
              '            self.ximagesrc_caps.set_value("framerate", Gst.Fraction(self.framerate, 1))\n'
              '            self.ximagesrc_capsfilter.set_property("caps", self.ximagesrc_caps)')
    assert old_fr in src, "PATCH7: set_framerate caps not found"
    src = src.replace(old_fr, new_fr, 1)

    # ---------- 8. Cap framerate at 90 fps — 144 saturates WebRTC jitter buffer ----------
    old_cap = ('        if self.pipeline:\n'
               '            self.framerate = framerate\n'
               '            # ADD_ENCODER: GOP/IDR Keyframe distance to keep the stream from freezing')
    new_cap = ('        if self.pipeline:\n'
               '            if framerate > 90:\n'
               '                logger.info("clamping requested framerate %d to 90" % framerate)\n'
               '                framerate = 90\n'
               '            self.framerate = framerate\n'
               '            # ADD_ENCODER: GOP/IDR Keyframe distance to keep the stream from freezing')
    if old_cap in src:
        src = src.replace(old_cap, new_cap, 1)
        print("  ✓ gstwebrtc_app.py: framerate clamp (90 fps) applied")

    with open(path, 'w') as f:
        f.write(src)
    print(f"  ✓ gstwebrtc_app.py: 8 patches applied")


def patch_resize(path):
    with open(path) as f:
        src = f.read()

    # ---------- 1. get_new_res: capture _NNN suffixed mode names too ----------
    old = r"    res_pat = re.compile(r'^(\d+x\d+)\s.*$')"
    new = r"    res_pat = re.compile(r'^(\d+x\d+(?:_\d+)?)\s.*$')"
    assert old in src, "RPATCH1: res_pat not found"
    src = src.replace(old, new, 1)

    # ---------- 2. get_new_res: strip _NNN suffix from curr_res for WxH comparison ----------
    old2 = '    resolutions.sort()\n    return curr_res, new_res, resolutions, max_res, screen_name'
    new2 = ('    # Strip refresh suffix from curr_res for clean WxH comparison\n'
            '    curr_res = re.sub(r"_\\d+$", "", curr_res)\n'
            '    resolutions.sort()\n'
            '    return curr_res, new_res, resolutions, max_res, screen_name')
    assert old2 in src, "RPATCH2: return block not found"
    src = src.replace(old2, new2, 1)

    # ---------- 3. resize_display: prefer 144Hz mode, generate 144Hz, fallback 60Hz ----------
    old3 = (
        '    logger.info("resizing display to %s" % res)\n'
        '    if res not in resolutions:\n'
        '        logger.info("adding mode %s to xrandr screen \'%s\'" % (res, screen_name))\n'
        '\n'
        '        mode, modeline = generate_xrandr_gtf_modeline(res)\n'
        '\n'
        '        # Create new mode from modeline\n'
        '        logger.info("creating new xrandr mode: %s %s" % (mode, modeline))\n'
        '        cmd = [\'xrandr\', \'--newmode\', mode, *re.split(\'\\s+\', modeline)]\n'
        '        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)\n'
        '        stdout, stderr = p.communicate()\n'
        '        if p.returncode != 0:\n'
        '            logger.error("failed to create new xrandr mode: \'%s %s\': %s%s" % (mode, modeline, str(stdout), str(stderr)))\n'
        '            return False\n'
        '\n'
        '        # Add the mode to the screen.\n'
        '        logger.info("adding xrandr mode \'%s\' to screen \'%s\'" % (mode, screen_name))\n'
        '        cmd = [\'xrandr\', \'--addmode\', screen_name, mode]\n'
        '        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)\n'
        '        stdout, stderr = p.communicate()\n'
        '        if p.returncode != 0:\n'
        '            logger.error("failed to add mode \'%s\' using xrandr: %s%s" % (mode, str(stdout), str(stderr)))\n'
        '            return False\n'
    )
    new3 = (
        '    logger.info("resizing display to %s" % res)\n'
        '\n'
        '    # Strategy: prefer existing 144Hz mode → generate 144Hz → generate 60Hz → use base\n'
        '    mode_144 = res + "_144"\n'
        '    if mode_144 in resolutions:\n'
        '        mode = mode_144\n'
        '        logger.info("found existing 144Hz mode: %s" % mode)\n'
        '    elif res not in resolutions:\n'
        '        # Try generating a 144Hz mode first, fall back to 60Hz\n'
        '        mode_hi, modeline_hi = generate_xrandr_gtf_modeline(res, refresh=144)\n'
        '        if not modeline_hi:\n'
        '            logger.warning("cvt failed for 144Hz, trying 60Hz")\n'
        '            mode_hi, modeline_hi = generate_xrandr_gtf_modeline(res, refresh=60)\n'
        '        if not modeline_hi:\n'
        '            logger.error("failed to generate any modeline for %s" % res)\n'
        '            return False\n'
        '        mode = mode_hi\n'
        '        modeline = modeline_hi\n'
        '\n'
        '        logger.info("creating new xrandr mode: %s %s" % (mode, modeline))\n'
        '        cmd = [\'xrandr\', \'--newmode\', mode, *re.split(r\'\\s+\', modeline)]\n'
        '        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)\n'
        '        stdout, stderr = p.communicate()\n'
        '        if p.returncode != 0:\n'
        '            logger.error("failed to create new xrandr mode: \'%s %s\': %s%s" % (mode, modeline, str(stdout), str(stderr)))\n'
        '            return False\n'
        '\n'
        '        logger.info("adding xrandr mode \'%s\' to screen \'%s\'" % (mode, screen_name))\n'
        '        cmd = [\'xrandr\', \'--addmode\', screen_name, mode]\n'
        '        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)\n'
        '        stdout, stderr = p.communicate()\n'
        '        if p.returncode != 0:\n'
        '            logger.error("failed to add mode \'%s\' using xrandr: %s%s" % (mode, str(stdout), str(stderr)))\n'
        '            return False\n'
    )
    assert old3 in src, "RPATCH3: resize_display body not found"
    src = src.replace(old3, new3, 1)

    # ---------- 4. generate_xrandr_gtf_modeline: accept refresh parameter ----------
    old4 = 'def generate_xrandr_gtf_modeline(res):'
    new4 = 'def generate_xrandr_gtf_modeline(res, refresh=144):'
    assert old4 in src, "RPATCH4: function signature not found"
    src = src.replace(old4, new4, 1)

    # Replace hardcoded "60" with the refresh parameter in both WxH and W H formats
    # WxH format
    old5a = '        gtf_res = "{} {} 60".format(toks[0], toks[1])\n        mode = res'
    new5a = '        gtf_res = "{} {} {}".format(toks[0], toks[1], refresh)\n        mode = res if refresh == 60 else "{}_{}" .format(res, refresh)'
    count = src.count(old5a)
    assert count >= 1, "RPATCH5a: WxH gtf_res not found"
    # Replace first occurrence (WxH)
    src = src.replace(old5a, new5a, 1)

    # W H format (second occurrence of the pattern, but with different mode= line)
    old5b = ('        gtf_res = "{} {} 60".format(toks[0], toks[1])\n'
             '        mode = "{}x{}".format(toks[0], toks[1])')
    new5b = ('        gtf_res = "{} {} {}".format(toks[0], toks[1], refresh)\n'
             '        base = "{}x{}".format(toks[0], toks[1])\n'
             '        mode = base if refresh == 60 else "{}_{}" .format(base, refresh)')
    if old5b in src:
        src = src.replace(old5b, new5b, 1)

    with open(path, 'w') as f:
        f.write(src)
    print(f"  ✓ resize.py: 5 patches applied")


if __name__ == "__main__":
    base = "/home/vscode/.local/lib/python3.12/site-packages/selkies_gstreamer"
    print("Applying robust NvFBC + 144Hz patches...")
    patch_gstwebrtc_app(f"{base}/gstwebrtc_app.py")
    patch_resize(f"{base}/resize.py")
    print("Done.")
