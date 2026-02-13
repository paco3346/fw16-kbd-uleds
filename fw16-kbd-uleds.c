/*
 * fw16-kbd-uleds
 * Copyright (c) 2026 paco3346
 * Licensed under the MIT License. See LICENSE file.
 */

// fw16-kbd-uleds.c
//
// Framework Laptop 16 backlight bridge for KDE/UPower.
// Default mode: auto (detect present modules, unified slider).
//
// Hotplug:
//   - Listens for kernel uevents (NETLINK_KOBJECT_UEVENT)
//   - On add/remove, re-scans /sys/bus/hid/devices and updates target list
//   - Newly-added targets are set to the current brightness level
//
// Debug levels (runtime):
//   FW16_KBD_ULEDS_DEBUG=0   (default) quiet
//   FW16_KBD_ULEDS_DEBUG=1   info: device discovery, hotplug changes, target list changes
//   FW16_KBD_ULEDS_DEBUG=2   verbose: also logs brightness events, debounce/apply details
//
// Build:
//   cc -O2 -Wall -Wextra -Wpedantic -std=c11 -o fw16-kbd-uleds fw16-kbd-uleds.c
//
// Install:
//   sudo install -Dm0755 fw16-kbd-uleds /usr/local/bin/fw16-kbd-uleds
//
// Requires:
//   - kernel module: uleds
//   - /usr/bin/qmk_hid (FrameworkComputer/qmk_hid)

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/netlink.h>
#include <linux/uleds.h>
#include <poll.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* -------------------- Debug -------------------- */

static int debug_level(void) {
    const char *e = getenv("FW16_KBD_ULEDS_DEBUG");
    if (!e || !*e) return 0;
    char *end = NULL;
    long v = strtol(e, &end, 10);
    if (end == e) return 0;
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    return (int)v;
}

static void dbg(int lvl, const char *fmt, ...) {
    if (debug_level() < lvl) return;
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
    if (pct == 0) return 0;
    if (pct <= 33) return 1;
    if (pct <= 66) return 2;
    return 3;
}

static unsigned level_to_qmk_pct(unsigned level) {
    switch (level) {
        case 0: return 0;
        case 1: return 33;
        case 2: return 66;
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
} target_t;

static int target_eq(const target_t *a, const target_t *b) {
    return a->vid == b->vid && a->pid == b->pid;
}

static int target_in_list(const target_t *list, size_t len, const target_t *t) {
    for (size_t i = 0; i < len; i++) {
        if (target_eq(&list[i], t)) return 1;
    }
    return 0;
}

static void print_targets(int lvl, const char *prefix, const target_t *list, size_t len) {
    if (debug_level() < lvl) return;
    fprintf(stderr, "%s", prefix);
    for (size_t i = 0; i < len; i++) {
        fprintf(stderr, "%s%04x:%04x", (i ? ", " : ""), list[i].vid, list[i].pid);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* -------------------- qmk_hid -------------------- */

static int qmk_set(uint16_t vid, uint16_t pid, unsigned pct) {
    const char *qmk = "/usr/bin/qmk_hid";

    char vid_s[8], pid_s[8], pct_s[8];
    snprintf(vid_s, sizeof(vid_s), "%04x", vid);
    snprintf(pid_s, sizeof(pid_s), "%04x", pid);
    snprintf(pct_s, sizeof(pct_s), "%u", pct);

    char *argv[] = {
        (char *)qmk,
        (char *)"--vid", vid_s,
        (char *)"--pid", pid_s,
        (char *)"via",
        (char *)"--backlight", pct_s,
        NULL
    };

    pid_t p;
    int rc = posix_spawn(&p, qmk, NULL, NULL, argv, environ);
    if (rc != 0) {
        dbg(2, "qmk_hid spawn failed rc=%d\n", rc);
        return -1;
    }

    int status = 0;
    if (waitpid(p, &status, 0) < 0) {
        dbg(2, "qmk_hid waitpid failed: %s\n", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;

    dbg(2, "qmk_hid failed vid=%04x pid=%04x\n", vid, pid);
    return -1;
}

static void qmk_apply_all(const target_t *targets, size_t len, unsigned level) {
    unsigned pct = level_to_qmk_pct(level);
    dbg(2, "apply level=%u pct=%u to %zu targets\n", level, pct, len);
    for (size_t i = 0; i < len; i++) {
        (void)qmk_set(targets[i].vid, targets[i].pid, pct);
    }
}

/* -------------------- HID auto-detect via sysfs -------------------- */

// Return 1 if a HID device with vid/pid appears present under /sys/bus/hid/devices, else 0.
// Looks for uevent line like: HID_ID=0003:000032AC:00000012
static int hid_present(uint16_t vid, uint16_t pid) {
    DIR *d = opendir("/sys/bus/hid/devices");
    if (!d) return 0;

    char needle[64];
    snprintf(needle, sizeof(needle), ":0000%04X:0000%04X", vid, pid);

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "/sys/bus/hid/devices/%s/uevent", ent->d_name);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        char buf[4096];
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r <= 0) continue;
        buf[r] = '\0';

        if (strstr(buf, needle)) {
            closedir(d);
            return 1;
        }
    }

    closedir(d);
    return 0;
}

static void autodetect_targets(uint16_t vid, target_t *out, size_t *len, size_t cap) {
    *len = 0;

    // Known FW16 input-module PIDs (VID 32ac):
    // keyboards: ANSI 0012, ISO 0018, JIS 0019
    // aux:       numpad 0014, RGB macropad 0013
    const uint16_t pids[] = { 0x0012, 0x0018, 0x0019, 0x0014, 0x0013 };

    for (size_t i = 0; i < sizeof(pids)/sizeof(pids[0]); i++) {
        if (hid_present(vid, pids[i])) {
            if (*len < cap) {
                out[*len] = (target_t){ .vid = vid, .pid = pids[i] };
                (*len)++;
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

typedef enum {
    FW_MODE_AUTO = 0,
    FW_MODE_UNIFIED
} fw_mode_t;

static fw_mode_t parse_mode(const char *s) {
    if (!s) return FW_MODE_AUTO;
    if (!strcmp(s, "auto")) return FW_MODE_AUTO;
    if (!strcmp(s, "unified")) return FW_MODE_UNIFIED;
    return FW_MODE_AUTO;
}

/* -------------------- Main -------------------- */

int main(int argc, char **argv) {
    fw_mode_t mode = FW_MODE_AUTO;
    uint16_t vid = 0x32ac;
    unsigned debounce_ms = 180;
    unsigned max_brightness = 100;

    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"vid", required_argument, 0, 'v'},
        {"debounce-ms", required_argument, 0, 'd'},
        {"max-brightness", required_argument, 0, 'b'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "m:v:d:b:", opts, NULL)) != -1) {
        switch (c) {
            case 'm': mode = parse_mode(optarg); break;
            case 'v': vid = (uint16_t)strtoul(optarg, NULL, 16); break;
            case 'd': debounce_ms = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'b': max_brightness = (unsigned)strtoul(optarg, NULL, 10); break;
        }
    }
    if (max_brightness == 0) max_brightness = 100;

    // Initial target discovery (auto/unified both discover at startup)
    target_t targets[16];
    size_t targets_len = 0;
    autodetect_targets(vid, targets, &targets_len, sizeof(targets)/sizeof(targets[0]));

    if (targets_len == 0) {
        fprintf(stderr, "No Framework HID targets detected (VID %04x)\n", vid);
        return 1;
    }

    // Info logs: what we found
    dbg(1, "autodetect: VID=%04x\n", vid);
    for (size_t i = 0; i < targets_len; i++) {
        uint16_t pid = targets[i].pid;
        if (pid == 0x0012) dbg(1, "found keyboard: ANSI (32ac:0012)\n");
        else if (pid == 0x0018) dbg(1, "found keyboard: ISO (32ac:0018)\n");
        else if (pid == 0x0019) dbg(1, "found keyboard: JIS (32ac:0019)\n");
        else if (pid == 0x0014) dbg(1, "found aux: numpad (32ac:0014)\n");
        else if (pid == 0x0013) dbg(1, "found aux: RGB macropad (32ac:0013)\n");
        else dbg(1, "found device: %04x:%04x\n", targets[i].vid, targets[i].pid);
    }
    print_targets(1, "targets: ", targets, targets_len);

    // Create unified LED (must include "kbd_backlight" in the name for UPower)
    int uleds_fd = create_uleds_led("framework::kbd_backlight", max_brightness);
    if (uleds_fd < 0) return 1;

    // Open uevent socket for hotplug
    int uev_fd = open_uevent_sock();
    if (uev_fd < 0) {
        // Not fatal; hotplug just won't work.
        dbg(1, "warning: failed to open uevent socket; hotplug disabled (%s)\n", strerror(errno));
    } else {
        dbg(1, "hotplug: listening for uevents\n");
    }

    // Apply consistent start state (off)
    qmk_apply_all(targets, targets_len, 0);

    unsigned last_level = 0;
    unsigned pending_level = 0;
    uint64_t deadline = 0;

    struct pollfd pfds[2];
    size_t pcount = (uev_fd >= 0) ? 2 : 1;

    for (;;) {
        uint64_t now = now_ms();
        int timeout = -1;

        if (deadline > 0) {
            timeout = (deadline <= now) ? 0 : (int)(deadline - now);
        }

        pfds[0].fd = uleds_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        if (uev_fd >= 0) {
            pfds[1].fd = uev_fd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
        }

        int pr = poll(pfds, pcount, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        now = now_ms();

        // Debounce expiry
        if (pr == 0 && deadline > 0 && deadline <= now) {
            if (pending_level != last_level) {
                qmk_apply_all(targets, targets_len, pending_level);
                last_level = pending_level;
            }
            deadline = 0;
            continue;
        }

        // uleds brightness change
        if (pfds[0].revents & POLLIN) {
            unsigned char buf[8];
            ssize_t r = read(uleds_fd, buf, sizeof(buf));
            if (r > 0) {
                unsigned raw = decode_uleds(buf, r);
                unsigned level = pct_to_level(raw);
                pending_level = level;
                deadline = now + debounce_ms;
                dbg(2, "event: raw=%u level=%u (read %zd bytes)\n", raw, level, r);
            }
        }

        // hotplug / uevent
        if (uev_fd >= 0 && (pfds[1].revents & POLLIN)) {
            char ubuf[8192];
            ssize_t r = recv(uev_fd, ubuf, sizeof(ubuf), 0);
            if (r > 0 && uevent_maybe_relevant(ubuf, r)) {
                // Rescan (simple + robust)
                target_t new_targets[16];
                size_t new_len = 0;
                autodetect_targets(vid, new_targets, &new_len, sizeof(new_targets)/sizeof(new_targets[0]));

                // Diff
                int changed = 0;

                // Additions
                for (size_t i = 0; i < new_len; i++) {
                    if (!target_in_list(targets, targets_len, &new_targets[i])) {
                        changed = 1;
                        dbg(1, "hotplug: new device connected %04x:%04x\n", new_targets[i].vid, new_targets[i].pid);
                        // Set new device to current brightness immediately
                        (void)qmk_set(new_targets[i].vid, new_targets[i].pid, level_to_qmk_pct(last_level));
                    }
                }

                // Removals
                for (size_t i = 0; i < targets_len; i++) {
                    if (!target_in_list(new_targets, new_len, &targets[i])) {
                        changed = 1;
                        dbg(1, "hotplug: device removed %04x:%04x\n", targets[i].vid, targets[i].pid);
                    }
                }

                if (changed) {
                    memcpy(targets, new_targets, sizeof(target_t) * new_len);
                    targets_len = new_len;
                    print_targets(1, "targets: ", targets, targets_len);
                }
            }
        }
    }

    close(uleds_fd);
    if (uev_fd >= 0) close(uev_fd);

    (void)mode; // mode currently only affects default behavior; kept for future expansion.
    return 0;
}
