/*
 * fw16-kbd-uleds
 * Copyright (c) 2026 paco3346
 * Licensed under the MIT License. See LICENSE file.
 */

// fw16-kbd-uleds.c
//
// Framework Laptop 16 backlight bridge for KDE/UPower.
// Default mode: unified (detect present modules, unified slider).
//
// Hotplug:
//   - Listens for kernel uevents (NETLINK_KOBJECT_UEVENT)
//   - On add/remove, re-scans /sys/bus/hid/devices and updates target list
//   - Newly-added targets are set to the current brightness level
//
// Debug levels (runtime):
//   FW16_KBD_ULEDS_DEBUG=0   (default) quiet
//   FW16_KBD_ULEDS_DEBUG=1   info: device discovery, hotplug changes, target list changes
//   FW16_KBD_ULEDS_DEBUG=2   verbose: also logs brightness events, apply details
//
// Build:
//   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -o fw16-kbd-uleds fw16-kbd-uleds.c
//
// Install:
//   sudo install -Dm0755 fw16-kbd-uleds /usr/local/bin/fw16-kbd-uleds
//
// Requires:
//   - kernel module: uleds
//   - libsystemd (for D-Bus synchronization)

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/netlink.h>
#include <linux/uleds.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>


// QMK/VIA HID protocol constants (gleaned from Framework's qmk_hid)
#define QMK_CMD_SET_VALUE 0x07
#define QMK_CMD_GET_VALUE 0x08
#define QMK_CH_BACKLIGHT 0x01
#define QMK_CH_RGB_MATRIX 0x03
#define QMK_ADDR_BRIGHTNESS 0x01

/* -------------------- Debug -------------------- */

static int g_debug_level = 0;

static void dbg(int lvl, const char *fmt, ...) {
    if (g_debug_level < lvl) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* -------------------- Brightness -------------------- */

static unsigned clamp_pct(unsigned v) {
    return (v > 100u) ? 100u : v;
}

static unsigned pct_to_level(unsigned pct) {
    pct = clamp_pct(pct);
    if (pct <= 16) return 0;
    if (pct <= 50) return 1;
    if (pct <= 83) return 2;
    return 3;
}

static unsigned level_to_qmk_pct(unsigned level) {
    switch (level) {
        case 0: return 0;
        case 1: return 35; // Use 35 instead of 33 to avoid 0% revert flakiness
        case 2: return 67;
        default: return 100;
    }
}

// uleds read format varies; handle 1-byte and 4-byte formats.
static unsigned decode_uleds(const unsigned char *buf, ssize_t r) {
    if (r == 1) return (unsigned)buf[0];
    if (r >= 4) {
        uint32_t v = 0;
        memcpy(&v, buf, sizeof(v));
        return (unsigned)v;
    }
    return 0;
}

/* -------------------- Targets -------------------- */

typedef struct {
    uint16_t vid;
    uint16_t pid;
    char hidraw[64];
} target_t;

typedef struct {
    int fd;
    char name[64];
    target_t targets[16];
    size_t targets_len;
    target_t master;
    unsigned last_level;
} uled_ctx_t;

static int target_eq(const target_t *a, const target_t *b) {
    return a->vid == b->vid && a->pid == b->pid;
}

static int target_in_list(const target_t *list, size_t len, const target_t *t) {
    for (size_t i = 0; i < len; i++) {
        if (target_eq(&list[i], t)) return 1;
    }
    return 0;
}


/* -------------------- qmk HIDRAW -------------------- */

static int qmk_hidraw_xfer(const char *hidraw, unsigned char cmd, unsigned char channel, unsigned char addr, unsigned char val, unsigned char *resp) {
    if (!hidraw || !*hidraw) return -1;
    char path[128];
    snprintf(path, sizeof(path), "/dev/%s", hidraw);
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    unsigned char buf[33];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00;
    buf[1] = cmd;
    buf[2] = channel;
    buf[3] = addr;
    buf[4] = val;

    if (write(fd, buf, 33) != 33) {
        close(fd);
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 200) <= 0) {
        close(fd);
        return -1;
    }

    unsigned char r[32];
    if (read(fd, r, 32) != 32) {
        close(fd);
        return -1;
    }
    close(fd);

    if (r[0] != cmd) return -1;
    if (resp) *resp = r[3];
    return 0;
}

static int qmk_set(const target_t *t, unsigned pct) {
    unsigned char val = (unsigned char)((pct * 255 + 50) / 100);
    // Try both white backlight and RGB matrix
    int r1 = qmk_hidraw_xfer(t->hidraw, QMK_CMD_SET_VALUE, QMK_CH_BACKLIGHT, QMK_ADDR_BRIGHTNESS, val, NULL);
    int r2 = qmk_hidraw_xfer(t->hidraw, QMK_CMD_SET_VALUE, QMK_CH_RGB_MATRIX, QMK_ADDR_BRIGHTNESS, val, NULL);
    return (r1 == 0 || r2 == 0) ? 0 : -1;
}

static void qmk_apply_all(const target_t *targets, size_t len, unsigned level, const target_t *skip) {
    unsigned pct = level_to_qmk_pct(level);
    dbg(2, "apply level=%u pct=%u to %zu targets\n", level, pct, len);
    for (size_t i = 0; i < len; i++) {
        if (skip && target_eq(&targets[i], skip)) continue;
        (void)qmk_set(&targets[i], pct);
    }
}

static int qmk_get(const target_t *t) {
    unsigned char val = 0;
    if (qmk_hidraw_xfer(t->hidraw, QMK_CMD_GET_VALUE, QMK_CH_BACKLIGHT, QMK_ADDR_BRIGHTNESS, 0, &val) == 0) {
        return (int)((val * 100 + 127) / 255);
    }
    if (qmk_hidraw_xfer(t->hidraw, QMK_CMD_GET_VALUE, QMK_CH_RGB_MATRIX, QMK_ADDR_BRIGHTNESS, 0, &val) == 0) {
        return (int)((val * 100 + 127) / 255);
    }
    return -1;
}


static void update_sysfs_brightness(const char *name, unsigned val) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);
    for (int i = 0; i < 10; i++) {
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "%u\n", val);
            (void)write(fd, buf, n);
            close(fd);

            // Trigger uevent so Powerdevil/UPower notice the change
            snprintf(path, sizeof(path), "/sys/class/leds/%s/uevent", name);
            fd = open(path, O_WRONLY);
            if (fd >= 0) {
                (void)write(fd, "change\n", 7);
                close(fd);
            }
            return;
        }
        if (errno != ENOENT) break;
        usleep(10000); // Wait 10ms for sysfs to catch up
    }
}

static void sync_ui(unsigned level) {
    // Synchronize UI via UPower (system bus) and KDE PowerDevil (session bus).
    dbg(1, "syncing UI to level %u (sd-bus)\n", level);

    // 1. System Bus (UPower)
    if (fork() == 0) {
        sd_bus *bus = NULL;
        int r = sd_bus_open_system(&bus);
        if (r >= 0) {
            sd_bus_message *m = NULL;
            r = sd_bus_call_method(bus, "org.freedesktop.UPower", "/org/freedesktop/UPower",
                                   "org.freedesktop.UPower", "EnumerateKbdBacklights", NULL, &m, "");
            if (r >= 0) {
                char **paths;
                if (sd_bus_message_read_strv(m, &paths) >= 0 && paths) {
                    for (char **p = paths; *p; p++) {
                        if (g_debug_level >= 3) dbg(3, "  UPower sync: %s\n", *p);
                        sd_bus_call_method(bus, "org.freedesktop.UPower", *p,
                                           "org.freedesktop.UPower.KbdBacklight", "SetBrightness", NULL, NULL, "i", (int32_t)level);
                    }
                }
                sd_bus_message_unref(m);
            }
            sd_bus_unref(bus);
        }
        exit(0);
    }

    // 2. Session Buses (PowerDevil)
    DIR *d = opendir("/run/user");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            uid_t uid = (uid_t)atoi(de->d_name);
            if (uid == 0) continue;

            char socket_path[512];
            snprintf(socket_path, sizeof(socket_path), "/run/user/%u/bus", uid);
            struct stat st;
            if (stat(socket_path, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;

            if (fork() == 0) {
                struct passwd *pw = getpwuid(uid);
                if (pw && setresuid(uid, uid, uid) == 0) {
                    setenv("HOME", pw->pw_dir, 1);
                    setenv("USER", pw->pw_name, 1);
                    char address[528];
                    snprintf(address, sizeof(address), "unix:path=%s", socket_path);
                    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);

                    sd_bus *sbus = NULL;
                    int r = sd_bus_open_user(&sbus);
                    if (r >= 0) {
                        if (g_debug_level >= 3) dbg(3, "  PowerDevil sync for user %u (%s)\n", uid, pw->pw_name);
                        sd_bus_error error = SD_BUS_ERROR_NULL;
                        r = sd_bus_call_method(sbus, "org.kde.org_kde_powerdevil",
                                               "/org/kde/Solid/PowerManagement/Actions/KeyboardBrightnessControl",
                                               "org.kde.Solid.PowerManagement.Actions.KeyboardBrightnessControl",
                                               "setKeyboardBrightness", &error, NULL, "i", (int32_t)level);
                        if (r < 0 && g_debug_level >= 3)
                            dbg(3, "    PowerDevil call failed for user %u: %s\n", uid, error.message);
                        sd_bus_error_free(&error);
                        sd_bus_flush(sbus);
                        sd_bus_unref(sbus);
                    } else if (g_debug_level >= 3) {
                        dbg(3, "    sd_bus_open_user failed for user %u: %s\n", uid, strerror(-r));
                    }
                }
                exit(0);
            }
        }
        closedir(d);
    }
}

/* -------------------- HID auto-detect via sysfs -------------------- */

static int find_raw_hidraw(uint16_t vid, uint16_t pid, char *out, size_t out_len) {
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) return -1;

    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[256];
        uint16_t v = 0, p = 0;
        int match = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "HID_ID=%*x:%hx:%hx", &v, &p) == 2) {
                if (v == vid && p == pid) match = 1;
                break;
            }
        }
        fclose(f);

        if (match) {
            // Check report descriptor for 0xFF60
            snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                int desc_size = 0;
                if (ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) >= 0) {
                    struct hidraw_report_descriptor rpt;
                    rpt.size = desc_size;
                    if (ioctl(fd, HIDIOCGRDESC, &rpt) >= 0) {
                        for (uint32_t i = 0; i < (uint32_t)rpt.size - 2; i++) {
                            if (rpt.value[i] == 0x06 && rpt.value[i+1] == 0x60 && rpt.value[i+2] == 0xFF) {
                                snprintf(out, out_len, "%s", ent->d_name);
                                found = 1;
                                break;
                            }
                        }
                    }
                }
                close(fd);
            }
        }
        if (found) break;
    }
    closedir(d);
    return found ? 0 : -1;
}

static void autodetect_targets(const uint16_t *vids, size_t num_vids, target_t *out, size_t *len, size_t cap) {
    const uint16_t pids[] = { 0x0012, 0x0018, 0x0019, 0x0014, 0x0013 };

    for (size_t v = 0; v < num_vids; v++) {
        for (size_t i = 0; i < sizeof(pids)/sizeof(pids[0]); i++) {
            char hidraw[64] = "";
            if (find_raw_hidraw(vids[v], pids[i], hidraw, sizeof(hidraw)) == 0) {
                if (*len < cap) {
                    target_t t = { .vid = vids[v], .pid = pids[i] };
                    snprintf(t.hidraw, sizeof(t.hidraw), "%s", hidraw);
                    if (!target_in_list(out, *len, &t)) {
                        out[*len] = t;
                        (*len)++;
                    }
                }
            }
        }
    }
}

/* -------------------- uleds LED creation -------------------- */

static int create_uleds_led(const char *name, unsigned max_brightness) {
    int fd = open("/dev/uleds", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/uleds");
        return -1;
    }

    struct uleds_user_dev u;
    memset(&u, 0, sizeof(u));
    snprintf(u.name, sizeof(u.name), "%s", name);
    u.max_brightness = max_brightness;

    if (write(fd, &u, sizeof(u)) != (ssize_t)sizeof(u)) {
        perror("write uleds");
        close(fd);
        return -1;
    }
    return fd;
}

/* -------------------- uevent hotplug -------------------- */

static int open_uevent_sock(void) {
    int s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (s < 0) return -1;

    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof(snl));
    snl.nl_family = AF_NETLINK;
    snl.nl_pid = (uint32_t)getpid();
    snl.nl_groups = 1; // receive broadcast uevents

    if (bind(s, (struct sockaddr *)&snl, sizeof(snl)) < 0) {
        close(s);
        return -1;
    }

    // Increase buffer to avoid drops under churn
    int rcvbuf = 1024 * 1024;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    return s;
}

// Quick filter: check if uevent message appears relevant to hid subsystem or contains "HID_ID="
// (Message is NUL-separated strings, e.g. "add@/devices/... \0 ACTION=add \0 SUBSYSTEM=hid \0 ...")
static int uevent_maybe_relevant(const char *buf, ssize_t len) {
    if (len <= 0) return 0;
    // Do a cheap substring scan; buffer is NUL-separated but still searchable.
    if (memmem(buf, (size_t)len, "SUBSYSTEM=hid", 13)) return 1;
    if (memmem(buf, (size_t)len, "SUBSYSTEM=hidraw", 16)) return 1;
    if (memmem(buf, (size_t)len, "HID_ID=", 7)) return 1;
    return 0;
}

/* -------------------- CLI -------------------- */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -m, --mode <mode>              Operation mode: 'unified' (default) or 'separate'\n");
    fprintf(stderr, "  -v, --vid <list>               Comma-separated VIDs or VID:PID (default: 32ac)\n");
    fprintf(stderr, "  -b, --max-brightness <val>     Maximum brightness value (default: 3)\n");
    fprintf(stderr, "  -p, --poll-ms <ms>             Hardware polling interval (default: 1000)\n");
    fprintf(stderr, "  -l, --list                     List auto-discovered devices and exit\n");
    fprintf(stderr, "  -h, --help                     Show this help message\n");
    fprintf(stderr, "\nEnvironment Variables:\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_DEBUG           Debug level: 0 (default), 1 (info), 2 (verbose), 3 (D-Bus)\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_MODE            Same as --mode\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_VID             Same as --vid\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_MAX_BRIGHTNESS  Same as --max-brightness\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_POLL_MS         Same as --poll-ms\n");
}

typedef enum {
    FW_MODE_UNIFIED = 0,
    FW_MODE_SEPARATE
} fw_mode_t;

static fw_mode_t parse_mode(const char *s) {
    if (!s) return FW_MODE_UNIFIED;
    if (!strcmp(s, "separate")) return FW_MODE_SEPARATE;
    if (!strcmp(s, "unified")) return FW_MODE_UNIFIED;
    return FW_MODE_UNIFIED;
}

static int get_type(uint16_t pid) {
    if (pid == 0x0012 || pid == 0x0018 || pid == 0x0019) return 0; // Kbd
    if (pid == 0x0014) return 1; // Numpad
    if (pid == 0x0013) return 2; // Macropad
    return 3; // Misc
}

static const char *type_names[] = {
    "framework::kbd_backlight",
    "framework::numpad_backlight",
    "framework::macropad_backlight",
    "framework::aux_backlight"
};

/* -------------------- Main -------------------- */

int main(int argc, char **argv) {
    const char *env_debug = getenv("FW16_KBD_ULEDS_DEBUG");
    if (env_debug) {
        g_debug_level = (int)strtol(env_debug, NULL, 10);
        if (g_debug_level < 0) g_debug_level = 0;
        if (g_debug_level > 3) g_debug_level = 3;
    }

    signal(SIGCHLD, SIG_IGN);
    fw_mode_t mode = FW_MODE_UNIFIED;
    uint16_t vids[8];
    size_t num_vids = 0;
    target_t manual_targets[16];
    size_t num_manual_targets = 0;
    unsigned max_brightness = 3;
    unsigned poll_ms = 1000;

    // Default VID
    vids[num_vids++] = 0x32ac;

    // Load from environment first
    const char *env_mode = getenv("FW16_KBD_ULEDS_MODE");
    if (env_mode) mode = parse_mode(env_mode);

    const char *env_vid = getenv("FW16_KBD_ULEDS_VID");
    if (env_vid) {
        num_vids = 0;
        char *dup = strdup(env_vid);
        char *saveptr;
        char *tok = strtok_r(dup, ",", &saveptr);
        while (tok && (num_vids < 8 || num_manual_targets < 16)) {
            if (strchr(tok, ':')) {
                uint16_t v = (uint16_t)strtoul(tok, NULL, 16);
                uint16_t p = (uint16_t)strtoul(strchr(tok, ':') + 1, NULL, 16);
                if (num_manual_targets < 16) manual_targets[num_manual_targets++] = (target_t){v, p, ""};
            } else if (num_vids < 8) {
                vids[num_vids++] = (uint16_t)strtoul(tok, NULL, 16);
            }
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(dup);
    }

    const char *env_max_brightness = getenv("FW16_KBD_ULEDS_MAX_BRIGHTNESS");
    if (env_max_brightness) max_brightness = (unsigned)strtoul(env_max_brightness, NULL, 10);

    const char *env_poll = getenv("FW16_KBD_ULEDS_POLL_MS");
    if (env_poll) poll_ms = (unsigned)strtoul(env_poll, NULL, 10);

    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"vid", required_argument, 0, 'v'},
        {"max-brightness", required_argument, 0, 'b'},
        {"poll-ms", required_argument, 0, 'p'},
        {"list", no_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int c;
    int do_list = 0;
    while ((c = getopt_long(argc, argv, "m:v:b:p:lh", opts, NULL)) != -1) {
        switch (c) {
            case 'm': mode = parse_mode(optarg); break;
            case 'v': {
                num_vids = 0;
                num_manual_targets = 0;
                char *dup = strdup(optarg);
                char *saveptr;
                char *tok = strtok_r(dup, ",", &saveptr);
                while (tok && (num_vids < 8 || num_manual_targets < 16)) {
                    if (strchr(tok, ':')) {
                        uint16_t v = (uint16_t)strtoul(tok, NULL, 16);
                        uint16_t p = (uint16_t)strtoul(strchr(tok, ':') + 1, NULL, 16);
                        if (num_manual_targets < 16) manual_targets[num_manual_targets++] = (target_t){v, p, ""};
                    } else if (num_vids < 8) {
                        vids[num_vids++] = (uint16_t)strtoul(tok, NULL, 16);
                    }
                    tok = strtok_r(NULL, ",", &saveptr);
                }
                free(dup);
                break;
            }
            case 'b': max_brightness = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'p': poll_ms = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'l': do_list = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    // Resolve manual targets hidraw nodes
    for (size_t i = 0; i < num_manual_targets; i++) {
        (void)find_raw_hidraw(manual_targets[i].vid, manual_targets[i].pid, manual_targets[i].hidraw, sizeof(manual_targets[i].hidraw));
    }

    if (do_list) {
        target_t disc[16];
        size_t disc_len = 0;
        autodetect_targets(vids, num_vids, disc, &disc_len, 16);
        
        if (disc_len == 0) {
            printf("No devices auto-discovered.\n");
        } else {
            printf("Auto-discovered devices:\n\n");
            char cli_arg[256] = "";
            size_t cli_pos = 0;

            for (size_t i = 0; i < disc_len; i++) {
                int type = get_type(disc[i].pid);
                printf("  [%zu] %04x:%04x (%s)\n", i + 1, disc[i].vid, disc[i].pid, type_names[type]);
                
                int n = snprintf(cli_arg + cli_pos, sizeof(cli_arg) - cli_pos, "%s%04x:%04x", (i == 0 ? "" : ","), disc[i].vid, disc[i].pid);
                if (n > 0) cli_pos += (size_t)n;
            }

            printf("\nTo target these specifically, use:\n");
            printf("  CLI:  -v %s\n", cli_arg);
            printf("  Conf: FW16_KBD_ULEDS_VID=%s\n", cli_arg);
        }
        return 0;
    }
    if (max_brightness == 0) max_brightness = 100;

    // Initial target discovery
    target_t discovered[16];
    size_t discovered_len = 0;
    autodetect_targets(vids, num_vids, discovered, &discovered_len, 16);

    // Merge manual and discovered
    target_t all_targets[32];
    size_t all_len = 0;
    for (size_t i = 0; i < num_manual_targets && all_len < 32; i++) {
        if (!target_in_list(all_targets, all_len, &manual_targets[i]))
            all_targets[all_len++] = manual_targets[i];
    }
    for (size_t i = 0; i < discovered_len && all_len < 32; i++) {
        if (!target_in_list(all_targets, all_len, &discovered[i]))
            all_targets[all_len++] = discovered[i];
    }

    if (all_len == 0) {
        fprintf(stderr, "No Framework HID targets detected\n");
        return 1;
    }

    // Initialize uleds contexts
    uled_ctx_t ctxs[4];
    size_t num_ctxs = 0;
    memset(ctxs, 0, sizeof(ctxs));

    if (mode == FW_MODE_SEPARATE) {
        for (size_t i = 0; i < all_len; i++) {
            int type = get_type(all_targets[i].pid);
            if (ctxs[type].targets_len == 0) {
                snprintf(ctxs[type].name, sizeof(ctxs[type].name), "%s", type_names[type]);
                ctxs[type].fd = -1;
            }
            if (ctxs[type].targets_len < 16) {
                ctxs[type].targets[ctxs[type].targets_len++] = all_targets[i];
            }
        }
    } else {
        // Unified mode
        snprintf(ctxs[0].name, sizeof(ctxs[0].name), "framework::kbd_backlight");
        ctxs[0].fd = -1;
        for (size_t i = 0; i < all_len && i < 16; i++) {
            ctxs[0].targets[ctxs[0].targets_len++] = all_targets[i];
        }
    }

    for (int i = 0; i < 4; i++) {
        if (ctxs[i].targets_len > 0) {
            ctxs[i].fd = create_uleds_led(ctxs[i].name, max_brightness);
            if (ctxs[i].fd < 0) return 1;
            num_ctxs++;

            // Find master target for polling (prefer keyboard)
            ctxs[i].master = ctxs[i].targets[0];
            for (size_t j = 0; j < ctxs[i].targets_len; j++) {
                if (get_type(ctxs[i].targets[j].pid) == 0) {
                    ctxs[i].master = ctxs[i].targets[j];
                    break;
                }
            }

            // Sync with current hardware state (with retry)
            int pct = -1;
            for (int r = 0; r < 5; r++) {
                pct = qmk_get(&ctxs[i].master);
                if (pct >= 0) break;
                usleep(200000); // 200ms
            }
            unsigned level = (pct >= 0) ? pct_to_level((unsigned)pct) : 0;
            ctxs[i].last_level = level;
            dbg(1, "initial state [%s]: %d%% (level %u) master=%04x:%04x\n", 
                ctxs[i].name, pct, level, ctxs[i].master.vid, ctxs[i].master.pid);
            
            // Immediately sync sysfs and other modules if needed
            update_sysfs_brightness(ctxs[i].name, (level * max_brightness) / 3);
            if (ctxs[i].targets_len > 1) {
                qmk_apply_all(ctxs[i].targets, ctxs[i].targets_len, level, NULL);
            }
            // Sync UPower state to match initial hardware level
            sync_ui(level);
        }
    }

    // Info logs
    dbg(1, "mode: %s, targets: %zu\n", (mode == FW_MODE_SEPARATE ? "separate" : "unified"), all_len);
    for (int i = 0; i < 4; i++) {
        if (ctxs[i].targets_len > 0) {
            dbg(1, "uleds: %s (%zu targets)\n", ctxs[i].name, ctxs[i].targets_len);
        }
    }

    // Open uevent socket for hotplug
    int uev_fd = open_uevent_sock();
    if (uev_fd < 0) {
        dbg(1, "warning: failed to open uevent socket; hotplug disabled (%s)\n", strerror(errno));
    } else {
        dbg(1, "hotplug: listening for uevents\n");
    }

    uint64_t next_hw_poll = now_ms() + 500;
    struct pollfd pfds[5]; // up to 4 uleds + 1 uevent
    for (;;) {
        uint64_t now = now_ms();
        int timeout = -1;

        int hw_t = (next_hw_poll <= now) ? 0 : (int)(next_hw_poll - now);
        timeout = hw_t;

        int pidx = 0;
        for (int i = 0; i < 4; i++) {
            if (ctxs[i].fd >= 0) {
                pfds[pidx].fd = ctxs[i].fd;
                pfds[pidx].events = POLLIN;
                pfds[pidx].revents = 0;
                pidx++;
            }
        }
        int uev_idx = -1;
        if (uev_fd >= 0) {
            uev_idx = pidx;
            pfds[pidx].fd = uev_fd;
            pfds[pidx].events = POLLIN;
            pfds[pidx].revents = 0;
            pidx++;
        }

        int pr = poll(pfds, pidx, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        now = now_ms();

        // Hardware polling
        if (now >= next_hw_poll) {
            for (int i = 0; i < 4; i++) {
                if (ctxs[i].fd >= 0) {
                    int pct = qmk_get(&ctxs[i].master);
                    if (pct >= 0) {
                        unsigned level = pct_to_level((unsigned)pct);
                        if (level != ctxs[i].last_level) {
                            dbg(1, "hardware change detected on [%s] (via %04x:%04x): %u -> %u\n", 
                                ctxs[i].name, ctxs[i].master.vid, ctxs[i].master.pid, ctxs[i].last_level, level);
                            ctxs[i].last_level = level;
                            // Apply to all OTHER targets in this context to keep them in sync
                            // We skip the master because it already changed at the hardware level
                            qmk_apply_all(ctxs[i].targets, ctxs[i].targets_len, level, &ctxs[i].master);
                            update_sysfs_brightness(ctxs[i].name, (level * max_brightness) / 3);
                            sync_ui(level);
                        }
                    }
                }
            }
            next_hw_poll = now + poll_ms;
        }

        // uleds events
        pidx = 0;
        for (int i = 0; i < 4; i++) {
            if (ctxs[i].fd >= 0) {
                if (pfds[pidx].revents & POLLIN) {
                    unsigned char buf[8];
                    ssize_t r = read(ctxs[i].fd, buf, sizeof(buf));
                    if (r > 0) {
                        unsigned raw = decode_uleds(buf, r);
                        unsigned level = pct_to_level((raw * 100) / max_brightness);
                        dbg(2, "event [%s]: raw=%u max=%u level=%u last=%u\n", 
                            ctxs[i].name, raw, max_brightness, level, ctxs[i].last_level);
                        if (level != ctxs[i].last_level) {
                            qmk_apply_all(ctxs[i].targets, ctxs[i].targets_len, level, NULL);
                            ctxs[i].last_level = level;
                        }
                    }
                }
                pidx++;
            }
        }

        // Hotplug
        if (uev_idx >= 0 && (pfds[uev_idx].revents & POLLIN)) {
            char ubuf[8192];
            ssize_t r = recv(uev_fd, ubuf, sizeof(ubuf), 0);
            if (r > 0 && uevent_maybe_relevant(ubuf, r)) {
                target_t new_all[32];
                size_t new_len = 0;

                target_t disc[16];
                size_t disc_len = 0;
                autodetect_targets(vids, num_vids, disc, &disc_len, 16);

                for (size_t i = 0; i < num_manual_targets && new_len < 32; i++) {
                    if (!target_in_list(new_all, new_len, &manual_targets[i]))
                        new_all[new_len++] = manual_targets[i];
                }
                for (size_t i = 0; i < disc_len && new_len < 32; i++) {
                    if (!target_in_list(new_all, new_len, &disc[i]))
                        new_all[new_len++] = disc[i];
                }

                // Simplified hotplug sync: just update targets in existing contexts
                for (int i = 0; i < 4; i++) {
                    size_t old_targets_len = ctxs[i].targets_len;
                    target_t old_targets[16];
                    memcpy(old_targets, ctxs[i].targets, sizeof(target_t) * old_targets_len);
                    ctxs[i].targets_len = 0;

                    for (size_t j = 0; j < new_len; j++) {
                        int type = (mode == FW_MODE_SEPARATE) ? get_type(new_all[j].pid) : 0;
                        if (type == i && ctxs[i].targets_len < 16) {
                            target_t t = new_all[j];
                            if (!target_in_list(old_targets, old_targets_len, &t)) {
                                dbg(1, "hotplug [%s]: new device %04x:%04x (%s)\n", ctxs[i].name, t.vid, t.pid, t.hidraw);
                                qmk_set(&t, level_to_qmk_pct(ctxs[i].last_level));
                            }
                            ctxs[i].targets[ctxs[i].targets_len++] = t;
                        }
                    }

                    for (size_t j = 0; j < old_targets_len; j++) {
                        if (!target_in_list(ctxs[i].targets, ctxs[i].targets_len, &old_targets[j])) {
                            dbg(1, "hotplug [%s]: device removed %04x:%04x\n", ctxs[i].name, old_targets[j].vid, old_targets[j].pid);
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < 4; i++) if (ctxs[i].fd >= 0) close(ctxs[i].fd);
    if (uev_fd >= 0) close(uev_fd);
    return 0;
}
