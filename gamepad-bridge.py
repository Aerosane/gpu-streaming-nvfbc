#!/usr/bin/env python3
"""Bridge Selkies gamepad socket to a real uinput Xbox controller device.

Selkies creates a UNIX socket server at /tmp/selkies_js0.sock that sends
Linux js_event structs. This bridge connects as a client, reads those events,
and writes them to a uinput virtual Xbox 360 controller that Wine/Proton can see.
"""

import socket
import struct
import sys
import time
import signal
import evdev
from evdev import UInput, AbsInfo, ecodes

JS_EVENT_BUTTON = 0x01
JS_EVENT_AXIS = 0x02
JS_EVENT_INIT = 0x80

# Config struct: "255sHH64H8B" (name, num_btns, num_axes, 64 btn codes, 8 axis codes)
MAX_BTNS = 64
MAX_AXES = 8
CONFIG_FMT = "255sHH%dH%dB" % (MAX_BTNS, MAX_AXES)
CONFIG_SIZE = struct.calcsize(CONFIG_FMT)

# js_event struct: uint32 time, int16 value, uint8 type, uint8 number
EVENT_FMT = 'IhBB'
EVENT_SIZE = struct.calcsize(EVENT_FMT)

# Xbox 360 controller capabilities
XBOX_CAPABILITIES = {
    ecodes.EV_KEY: [
        ecodes.BTN_A, ecodes.BTN_B, ecodes.BTN_X, ecodes.BTN_Y,
        ecodes.BTN_TL, ecodes.BTN_TR,
        ecodes.BTN_SELECT, ecodes.BTN_START, ecodes.BTN_MODE,
        ecodes.BTN_THUMBL, ecodes.BTN_THUMBR,
    ],
    ecodes.EV_ABS: [
        (ecodes.ABS_X, AbsInfo(0, -32768, 32767, 16, 128, 0)),
        (ecodes.ABS_Y, AbsInfo(0, -32768, 32767, 16, 128, 0)),
        (ecodes.ABS_Z, AbsInfo(0, 0, 255, 0, 0, 0)),        # LT
        (ecodes.ABS_RX, AbsInfo(0, -32768, 32767, 16, 128, 0)),
        (ecodes.ABS_RY, AbsInfo(0, -32768, 32767, 16, 128, 0)),
        (ecodes.ABS_RZ, AbsInfo(0, 0, 255, 0, 0, 0)),       # RT
        (ecodes.ABS_HAT0X, AbsInfo(0, -1, 1, 0, 0, 0)),     # D-pad X
        (ecodes.ABS_HAT0Y, AbsInfo(0, -1, 1, 0, 0, 0)),     # D-pad Y
    ],
}

# Standard Selkies xpad button mapping (index → evdev code)
BTN_MAP = [
    ecodes.BTN_A, ecodes.BTN_B, ecodes.BTN_X, ecodes.BTN_Y,
    ecodes.BTN_TL, ecodes.BTN_TR,
    ecodes.BTN_SELECT, ecodes.BTN_START, ecodes.BTN_MODE,
    ecodes.BTN_THUMBL, ecodes.BTN_THUMBR,
]

# Standard Selkies xpad axis mapping (index → evdev code)
AXIS_MAP = [
    ecodes.ABS_X, ecodes.ABS_Y,
    ecodes.ABS_Z, ecodes.ABS_RX,
    ecodes.ABS_RY, ecodes.ABS_RZ,
    ecodes.ABS_HAT0X, ecodes.ABS_HAT0Y,
]


def create_gamepad():
    """Create a virtual Xbox 360 controller via uinput."""
    ui = UInput(
        XBOX_CAPABILITIES,
        name='Xbox 360 Controller',
        vendor=0x045e,
        product=0x028e,
        version=0x0110,
        bustype=ecodes.BUS_USB,
    )
    print(f"[gamepad-bridge] Created virtual Xbox 360 controller")
    # Find the new device
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        if dev.name == 'Xbox 360 Controller' and 'uinput' in (dev.phys or ''):
            print(f"[gamepad-bridge] Device: {dev.path} ({dev.name})")
            break
    return ui


def connect_socket(socket_path):
    """Connect to Selkies gamepad socket."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_path)
    print(f"[gamepad-bridge] Connected to {socket_path}")
    return sock


def read_config(sock):
    """Read the initial config packet from Selkies."""
    data = b''
    while len(data) < CONFIG_SIZE:
        chunk = sock.recv(CONFIG_SIZE - len(data))
        if not chunk:
            raise ConnectionError("Socket closed during config read")
        data += chunk

    parsed = struct.unpack(CONFIG_FMT, data)
    name = parsed[0].split(b'\x00')[0].decode('utf-8', errors='replace')
    num_btns = parsed[1]
    num_axes = parsed[2]
    btn_codes = list(parsed[3:3 + MAX_BTNS])[:num_btns]
    axis_codes = list(parsed[3 + MAX_BTNS:])[:num_axes]
    print(f"[gamepad-bridge] Controller: '{name}', {num_btns} buttons, {num_axes} axes")
    print(f"[gamepad-bridge] Button codes: {btn_codes}")
    print(f"[gamepad-bridge] Axis codes: {axis_codes}")
    return name, num_btns, num_axes, btn_codes, axis_codes


def run_bridge(socket_path='/tmp/selkies_js0.sock'):
    ui = create_gamepad()
    
    try:
        sock = connect_socket(socket_path)
    except (ConnectionRefusedError, FileNotFoundError) as e:
        print(f"[gamepad-bridge] Cannot connect to {socket_path}: {e}")
        print("[gamepad-bridge] Make sure Selkies is running and a gamepad is connected in the browser")
        ui.close()
        return

    name, num_btns, num_axes, btn_codes, axis_codes = read_config(sock)

    running = True
    def handle_signal(sig, frame):
        nonlocal running
        running = False
    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    # Browser sends 17 buttons but Selkies maps to 11 xpad buttons + axes_to_btn for triggers/dpad
    # The webrtc_input maps browser buttons 11-16 to axis events (LT, RT, dpad)
    # So we receive btn events 0-10 and axis events 0-7

    print("[gamepad-bridge] Bridging events... (Ctrl+C to stop)")
    buf = b''
    event_count = 0
    while running:
        try:
            data = sock.recv(4096)
            if not data:
                print("[gamepad-bridge] Socket closed")
                break
            buf += data

            while len(buf) >= EVENT_SIZE:
                event_data = buf[:EVENT_SIZE]
                buf = buf[EVENT_SIZE:]
                ts, value, etype, number = struct.unpack(EVENT_FMT, event_data)

                raw_type = etype & ~JS_EVENT_INIT
                is_init = bool(etype & JS_EVENT_INIT)

                if raw_type == JS_EVENT_BUTTON:
                    if number < len(BTN_MAP):
                        ui.write(ecodes.EV_KEY, BTN_MAP[number], value)
                        ui.syn()
                        if not is_init and value:
                            event_count += 1
                            if event_count <= 5:
                                print(f"[gamepad-bridge] Button {number} pressed (code {BTN_MAP[number]})")
                elif raw_type == JS_EVENT_AXIS:
                    if number < len(AXIS_MAP):
                        ui.write(ecodes.EV_ABS, AXIS_MAP[number], value)
                        ui.syn()
                        if not is_init and abs(value) > 1000:
                            event_count += 1
                            if event_count <= 5:
                                print(f"[gamepad-bridge] Axis {number} = {value} (code {AXIS_MAP[number]})")
                # Skip unknown event types (0x00, 0x10 config events)

        except (BrokenPipeError, ConnectionResetError):
            print("[gamepad-bridge] Connection lost")
            break
        except OSError as e:
            if e.errno == 19:  # ENODEV - uinput device removed
                print("[gamepad-bridge] uinput device lost, recreating...")
                try:
                    ui.close()
                except:
                    pass
                ui = create_gamepad()
            else:
                print(f"[gamepad-bridge] OS Error: {e}")
                break
        except Exception as e:
            print(f"[gamepad-bridge] Error: {e}")
            break

    sock.close()
    ui.close()
    print("[gamepad-bridge] Stopped")


def find_socket():
    """Find first available Selkies gamepad socket (js0-js3)."""
    import glob
    socks = sorted(glob.glob('/tmp/selkies_js*.sock'))
    if socks:
        return socks[0]
    return None


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else None
    while True:
        if path is None:
            path = find_socket()
        if path is None:
            print("[gamepad-bridge] No selkies gamepad socket found, waiting...")
            time.sleep(3)
            continue
        try:
            print(f"[gamepad-bridge] Using socket: {path}")
            run_bridge(path)
        except Exception as e:
            print(f"[gamepad-bridge] Error: {e}")
        path = None  # re-scan on retry
        print("[gamepad-bridge] Retrying in 3s...")
        time.sleep(3)
