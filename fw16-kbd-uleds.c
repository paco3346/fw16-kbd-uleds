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

typedef struct {
    int fd;
    char name[64];
    target_t targets[16];
    size_t targets_len;
    unsigned last_level;
    unsigned pending_level;
    uint64_t deadline;
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

static void autodetect_targets(const uint16_t *vids, size_t num_vids, target_t *out, size_t *len, size_t cap) {
    const uint16_t pids[] = { 0x0012, 0x0018, 0x0019, 0x0014, 0x0013 };

    for (size_t v = 0; v < num_vids; v++) {
        for (size_t i = 0; i < sizeof(pids)/sizeof(pids[0]); i++) {
            if (hid_present(vids[v], pids[i])) {
                if (*len < cap) {
                    target_t t = { .vid = vids[v], .pid = pids[i] };
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
    fprintf(stderr, "  -d, --debounce-ms <ms>         Debounce time in milliseconds (default: 180)\n");
    fprintf(stderr, "  -b, --max-brightness <val>     Maximum brightness value (default: 100)\n");
    fprintf(stderr, "  -l, --list                     List auto-discovered devices and exit\n");
    fprintf(stderr, "  -h, --help                     Show this help message\n");
    fprintf(stderr, "\nEnvironment Variables:\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_DEBUG           Debug level: 0 (default), 1 (info), 2 (verbose)\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_MODE            Same as --mode\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_VID             Same as --vid\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_DEBOUNCE_MS     Same as --debounce-ms\n");
    fprintf(stderr, "  FW16_KBD_ULEDS_MAX_BRIGHTNESS  Same as --max-brightness\n");
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
    fw_mode_t mode = FW_MODE_UNIFIED;
    uint16_t vids[8];
    size_t num_vids = 0;
    target_t manual_targets[16];
    size_t num_manual_targets = 0;
    unsigned debounce_ms = 180;
    unsigned max_brightness = 100;

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
                if (num_manual_targets < 16) manual_targets[num_manual_targets++] = (target_t){v, p};
            } else if (num_vids < 8) {
                vids[num_vids++] = (uint16_t)strtoul(tok, NULL, 16);
            }
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(dup);
    }

    const char *env_debounce = getenv("FW16_KBD_ULEDS_DEBOUNCE_MS");
    if (env_debounce) debounce_ms = (unsigned)strtoul(env_debounce, NULL, 10);

    const char *env_max_brightness = getenv("FW16_KBD_ULEDS_MAX_BRIGHTNESS");
    if (env_max_brightness) max_brightness = (unsigned)strtoul(env_max_brightness, NULL, 10);

    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"vid", required_argument, 0, 'v'},
        {"debounce-ms", required_argument, 0, 'd'},
        {"max-brightness", required_argument, 0, 'b'},
        {"list", no_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int c;
    int do_list = 0;
    while ((c = getopt_long(argc, argv, "m:v:d:b:lh", opts, NULL)) != -1) {
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
                        if (num_manual_targets < 16) manual_targets[num_manual_targets++] = (target_t){v, p};
                    } else if (num_vids < 8) {
                        vids[num_vids++] = (uint16_t)strtoul(tok, NULL, 16);
                    }
                    tok = strtok_r(NULL, ",", &saveptr);
                }
                free(dup);
                break;
            }
            case 'd': debounce_ms = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'b': max_brightness = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'l': do_list = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
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
                if (n > 0) cli_pos += n;
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
            // Apply start state
            qmk_apply_all(ctxs[i].targets, ctxs[i].targets_len, 0);
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

    struct pollfd pfds[5]; // up to 4 uleds + 1 uevent
    for (;;) {
        uint64_t now = now_ms();
        int timeout = -1;

        for (int i = 0; i < 4; i++) {
            if (ctxs[i].fd >= 0 && ctxs[i].deadline > 0) {
                int t = (ctxs[i].deadline <= now) ? 0 : (int)(ctxs[i].deadline - now);
                if (timeout == -1 || t < timeout) timeout = t;
            }
        }

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

        // Debounce expiries
        for (int i = 0; i < 4; i++) {
            if (ctxs[i].fd >= 0 && ctxs[i].deadline > 0 && ctxs[i].deadline <= now) {
                if (ctxs[i].pending_level != ctxs[i].last_level) {
                    qmk_apply_all(ctxs[i].targets, ctxs[i].targets_len, ctxs[i].pending_level);
                    ctxs[i].last_level = ctxs[i].pending_level;
                }
                ctxs[i].deadline = 0;
            }
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
                        unsigned level = pct_to_level(raw);
                        ctxs[i].pending_level = level;
                        ctxs[i].deadline = now + debounce_ms;
                        dbg(2, "event [%s]: raw=%u level=%u\n", ctxs[i].name, raw, level);
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
                                dbg(1, "hotplug [%s]: new device %04x:%04x\n", ctxs[i].name, t.vid, t.pid);
                                qmk_set(t.vid, t.pid, level_to_qmk_pct(ctxs[i].last_level));
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
