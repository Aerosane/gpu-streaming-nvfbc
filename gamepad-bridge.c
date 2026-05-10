/*
 * gamepad-bridge.c — Direct uinput Xbox 360 controller for Selkies WebRTC.
 *
 * Listens on a Unix DGRAM socket for 4-byte gamepad packets sent directly
 * from webrtc_input.py, bypassing Selkies' gamepad.py async pipeline.
 * Each packet is: { uint8 type, uint8 number, int16 value }
 * type: 1=button, 2=axis
 *
 * Also supports legacy Selkies stream socket (auto-discovered) as fallback.
 *
 * Build:  gcc -O2 -o gamepad-bridge gamepad-bridge.c
 * Usage:  ./gamepad-bridge
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <glob.h>
#include <time.h>
#include <stdint.h>
#include <poll.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <linux/uinput.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#define TAG "[gamepad-bridge] "

#define DGRAM_SOCK_PATH "/tmp/gamepad-bridge.sock"

/* Fast-path packet: type(1) + number(1) + value(2) = 4 bytes */
#pragma pack(push, 1)
struct gp_packet {
    uint8_t  type;    /* 1=button, 2=axis */
    uint8_t  number;
    int16_t  value;
};
#pragma pack(pop)

#define GP_PACKET_SIZE sizeof(struct gp_packet)
#define GP_TYPE_BUTTON 1
#define GP_TYPE_AXIS   2

/* Legacy Selkies js_event: uint32 time, int16 value, uint8 type, uint8 number */
#pragma pack(push, 1)
struct js_event {
    uint32_t time;
    int16_t  value;
    uint8_t  type;
    uint8_t  number;
};
#pragma pack(pop)

#define JS_EVENT_SIZE   sizeof(struct js_event)
#define JS_EVENT_BUTTON 0x01
#define JS_EVENT_AXIS   0x02
#define JS_EVENT_INIT   0x80

/* Selkies config packet (for legacy path) */
#define MAX_BTNS 64
#define MAX_AXES 8

#pragma pack(push, 1)
struct selkies_config {
    char     name[255];
    uint16_t num_btns;
    uint16_t num_axes;
    uint16_t btn_codes[MAX_BTNS];
    uint8_t  axis_codes[MAX_AXES];
};
#pragma pack(pop)

#define CONFIG_SIZE sizeof(struct selkies_config)

/* Xbox 360 controller */
#define XBOX_VENDOR  0x045e
#define XBOX_PRODUCT 0x028e
#define XBOX_VERSION 0x0110

static const int btn_map[] = {
    BTN_A, BTN_B, BTN_X, BTN_Y,
    BTN_TL, BTN_TR,
    BTN_SELECT, BTN_START, BTN_MODE,
    BTN_THUMBL, BTN_THUMBR,
};
#define NUM_BTNS (sizeof(btn_map) / sizeof(btn_map[0]))

static const int axis_map[] = {
    ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y,
};
#define NUM_AXES (sizeof(axis_map) / sizeof(axis_map[0]))

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── uinput ─────────────────────────────────────────────────────────── */

static int uinput_create(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror(TAG "open /dev/uinput");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; i < NUM_BTNS; i++)
        ioctl(fd, UI_SET_KEYBIT, btn_map[i]);

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (size_t i = 0; i < NUM_AXES; i++)
        ioctl(fd, UI_SET_ABSBIT, axis_map[i]);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "Xbox 360 Controller");
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = XBOX_VENDOR;
    setup.id.product = XBOX_PRODUCT;
    setup.id.version = XBOX_VERSION;

    struct uinput_abs_setup abs_setup;
    for (size_t i = 0; i < NUM_AXES; i++) {
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code = axis_map[i];
        switch (axis_map[i]) {
        case ABS_Z: case ABS_RZ:
            abs_setup.absinfo.minimum = 0;
            abs_setup.absinfo.maximum = 255;
            break;
        case ABS_HAT0X: case ABS_HAT0Y:
            abs_setup.absinfo.minimum = -1;
            abs_setup.absinfo.maximum =  1;
            break;
        default:
            abs_setup.absinfo.minimum = -32768;
            abs_setup.absinfo.maximum =  32767;
            abs_setup.absinfo.fuzz    = 16;
            abs_setup.absinfo.flat    = 128;
            break;
        }
        if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
            perror(TAG "UI_ABS_SETUP");
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        perror(TAG "UI_DEV_SETUP");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror(TAG "UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    printf(TAG "Created virtual Xbox 360 controller\n");
    return fd;
}

static void uinput_destroy(int fd)
{
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
    }
}

static inline void uinput_emit(int fd, int type, int code, int value)
{
    struct input_event ev = {0};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    (void)!write(fd, &ev, sizeof(ev));
}

static inline void uinput_syn(int fd)
{
    uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* Process a button/axis event and write to uinput */
static inline void process_event(int ui_fd, int type, int number, int value,
                                 int *event_count)
{
    if (type == GP_TYPE_BUTTON && (size_t)number < NUM_BTNS) {
        uinput_emit(ui_fd, EV_KEY, btn_map[number], value);
        uinput_syn(ui_fd);
        if (value && *event_count < 5) {
            printf(TAG "Button %d pressed (code %d)\n", number, btn_map[number]);
            (*event_count)++;
        }
    } else if (type == GP_TYPE_AXIS && (size_t)number < NUM_AXES) {
        uinput_emit(ui_fd, EV_ABS, axis_map[number], value);
        uinput_syn(ui_fd);
        if ((value > 1000 || value < -1000) && *event_count < 5) {
            printf(TAG "Axis %d = %d (code %d)\n", number, value, axis_map[number]);
            (*event_count)++;
        }
    }
}

/* ── DGRAM listener (fast path) ────────────────────────────────────── */

static int dgram_create(void)
{
    unlink(DGRAM_SOCK_PATH);

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror(TAG "dgram socket");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DGRAM_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror(TAG "dgram bind");
        close(fd);
        return -1;
    }

    chmod(DGRAM_SOCK_PATH, 0666);
    printf(TAG "Fast-path DGRAM listening on %s\n", DGRAM_SOCK_PATH);
    return fd;
}

/* ── legacy stream socket (Selkies gamepad.py) ─────────────────────── */

static int legacy_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int recv_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len && g_running) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

static const char *find_legacy_socket(void)
{
    static char path_buf[256];
    glob_t g = {0};
    if (glob("/tmp/selkies_js*.sock", 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        return NULL;
    }
    strncpy(path_buf, g.gl_pathv[0], sizeof(path_buf) - 1);
    globfree(&g);
    return path_buf;
}

/* ── main loop: poll both DGRAM + legacy ────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int ui_fd = uinput_create();
    if (ui_fd < 0) return 1;

    int dgram_fd = dgram_create();
    if (dgram_fd < 0) {
        uinput_destroy(ui_fd);
        return 1;
    }

    int legacy_fd = -1;
    int legacy_configured = 0;
    int event_count = 0;

    /* Stream buffer for legacy path */
    uint8_t stream_buf[4096];
    size_t stream_len = 0;

    printf(TAG "Ready — waiting for events on fast-path + legacy\n");

    while (g_running) {
        /* Try to connect legacy if not connected */
        if (legacy_fd < 0) {
            const char *lp = find_legacy_socket();
            if (lp) {
                legacy_fd = legacy_connect(lp);
                if (legacy_fd >= 0) {
                    printf(TAG "Legacy connected: %s\n", lp);
                    legacy_configured = 0;
                    stream_len = 0;
                }
            }
        }

        struct pollfd fds[2];
        int nfds = 0;

        fds[nfds].fd = dgram_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        if (legacy_fd >= 0) {
            fds[nfds].fd = legacy_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int ret = poll(fds, nfds, 1000);  /* 1s timeout to retry legacy */
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror(TAG "poll");
            break;
        }
        if (ret == 0) continue;

        /* ── Fast-path DGRAM ──────────────────────────────────────── */
        if (fds[0].revents & POLLIN) {
            /* Drain all pending datagrams */
            for (;;) {
                struct gp_packet pkt;
                ssize_t n = recv(dgram_fd, &pkt, sizeof(pkt), MSG_DONTWAIT);
                if (n <= 0) break;
                if (n < (ssize_t)GP_PACKET_SIZE) {
                    fprintf(stderr, TAG "DGRAM short read: %zd bytes\n", n);
                    break;
                }
                process_event(ui_fd, pkt.type, pkt.number, pkt.value,
                              &event_count);
            }
        }

        /* ── Legacy stream ────────────────────────────────────────── */
        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            ssize_t n = recv(legacy_fd, stream_buf + stream_len,
                             sizeof(stream_buf) - stream_len, MSG_DONTWAIT);
            if (n <= 0) {
                printf(TAG "Legacy disconnected\n");
                close(legacy_fd);
                legacy_fd = -1;
            } else {
                stream_len += n;

                /* Skip config packet on first connect */
                if (!legacy_configured && stream_len >= CONFIG_SIZE) {
                    struct selkies_config *cfg = (struct selkies_config *)stream_buf;
                    cfg->name[254] = '\0';
                    printf(TAG "Legacy controller: '%s'\n", cfg->name);
                    memmove(stream_buf, stream_buf + CONFIG_SIZE,
                            stream_len - CONFIG_SIZE);
                    stream_len -= CONFIG_SIZE;
                    legacy_configured = 1;
                }

                if (legacy_configured) {
                    while (stream_len >= JS_EVENT_SIZE) {
                        struct js_event ev;
                        memcpy(&ev, stream_buf, JS_EVENT_SIZE);
                        memmove(stream_buf, stream_buf + JS_EVENT_SIZE,
                                stream_len - JS_EVENT_SIZE);
                        stream_len -= JS_EVENT_SIZE;

                        uint8_t raw = ev.type & ~JS_EVENT_INIT;
                        process_event(ui_fd, raw, ev.number, ev.value,
                                      &event_count);
                    }
                }
            }
        }

        /* Handle legacy errors */
        if (nfds > 1 && (fds[1].revents & (POLLHUP | POLLERR))) {
            close(legacy_fd);
            legacy_fd = -1;
        }
    }

    if (legacy_fd >= 0) close(legacy_fd);
    close(dgram_fd);
    unlink(DGRAM_SOCK_PATH);
    uinput_destroy(ui_fd);
    printf(TAG "Exiting\n");
    return 0;
}
