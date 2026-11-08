// specialkeysd: handles the special keys nobody else picks up in the kiosk --
// brightness (display and keyboard), Exposé/F3 to switch the sway window --
// and dims the screen when idle.
// Since 2026-11-01 there is no tmux: F3/F4 go to sway via swaymsg, where the
// windows are tabs. Ctrl+F3 (view switching) is gone.
// A bare sway session has no idle or special-key handling; powerdevil and
// kglobalaccel used to do that.
//
// Called idle-dim until 2026-09-06, because dimming is how it started --
// that has long been the smaller part.
//
// build: make specialkeysd && make install-daemon   (see README)
// Runs as a system service, because only root may both read /dev/input/* and
// write /sys/class/backlight.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <linux/input.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef BL                       // overridable for tests
#define BL    "/sys/class/backlight/intel_backlight/brightness"
#endif
#ifndef BLMAX
#define BLMAX "/sys/class/backlight/intel_backlight/max_brightness"
#endif
#ifndef KB
#define KB    "/sys/class/leds/smc::kbd_backlight/brightness"
#endif
#ifndef KBMAX
#define KBMAX "/sys/class/leds/smc::kbd_backlight/max_brightness"
#endif
#ifndef KIOSKUSER                // user swaymsg is run as
#define KIOSKUSER "michi"
#endif
#ifndef ACPATH
#define ACPATH "/sys/class/power_supply/ADP1/online"
#endif
#define MAXFD 32
#define RESCAN 5                     // seconds: pick up new input devices
#define STEPMS 120                   // debounce: the video bus *and* the keyboard
                                     // both report the same key press
#define WAKE "/run/user/%u/specialkeysd-wake"
                                     // Anything that types without an input
                                     // device -- the m5assistant dictation
                                     // injects text via a virtual keyboard --
                                     // touches this file and so counts as
                                     // activity. Lives in the kiosk user's
                                     // runtime dir so no privileges are needed.

static int idle_bat = 1800;          // timeout on battery
static int idle_ac  = 3600;          // timeout on AC

// Fixed steps in per mille, fine at the bottom and coarse at the top: the value
// sets the PWM duty cycle, and perceived brightness is roughly its fourth root
// -- one per mille is clearly visible in the dark, at 90 % it is not.
// Per mille rather than percent because 1 % (13 of 1388 steps) is still too
// bright at night; the panel accepts raw values down to 1.
static const int disp_steps[] = {0,2,5,10,20,50,100,200,300,400,
                                 500,600,700,800,900,1000};
static const int kbd_steps[]  = {0,10,100,500,1000};

struct dimmer {
    const char *path;
    const int  *steps;
    int         nsteps;
    long        max;             // <= 0: device not present
    long        laststep;
};

// Who owns sway; root cannot simply use that user's socket.
static uid_t u_uid; static gid_t u_gid; static char *u_name, *u_home;

// F3 switches to the next tab (like Alt-Tab, via swaybar-status's tick "next"),
// F4 opens a terminal.
// The socket name contains sway's PID, hence the glob; without a running sway
// nothing happens.
static void sway_do(const char *cmd, int tick) {
    if (!u_home) return;
    pid_t p = fork();
    if (p != 0) return;                      // don't wait, SIGCHLD is ignored
    if (initgroups(u_name, u_gid) || setgid(u_gid) || setuid(u_uid)) _exit(127);
    char pat[64]; glob_t g;
    snprintf(pat, sizeof pat, "/run/user/%u/sway-ipc.*.sock", (unsigned)u_uid);
    if (glob(pat, 0, NULL, &g) || g.gl_pathc == 0) _exit(1);
    setenv("SWAYSOCK", g.gl_pathv[g.gl_pathc - 1], 1);
    setenv("HOME", u_home, 1);
    if (tick) execlp("swaymsg", "swaymsg", "-t", "send_tick", cmd, (char *)NULL);
    else execlp("swaymsg", "swaymsg", cmd, (char *)NULL);
    _exit(127);
}

static long nowms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long rd(const char *p) {
    char b[64]; int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return -1;
    b[n] = 0; return strtol(b, NULL, 10);
}

static void wr(const char *p, long v) {
    char b[32]; int n = snprintf(b, sizeof b, "%ld\n", v);
    int fd = open(p, O_WRONLY);
    if (fd < 0) return;
    ssize_t ig = write(fd, b, (size_t)n); (void)ig;
    close(fd);
}

static long wake_stamp(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_mtime : 0;
}

// per-mille step -> raw device value
static long raw_of(const struct dimmer *d, int permille) {
    return (permille * d->max + 500) / 1000;
}

// one step up or down
static void step(struct dimmer *d, int dir) {
    if (d->max <= 0) return;
    long t = nowms();
    if (t - d->laststep < STEPMS) return;
    d->laststep = t;
    long cur = rd(d->path); if (cur < 0) cur = 0;
    // Compare raw values, not per mille: converting back maps several steps to
    // the same rounded per-mille value, and a key press would then do nothing.
    long want = cur;
    if (dir > 0) {
        for (int k = 0; k < d->nsteps; k++)
            if (raw_of(d, d->steps[k]) > cur) { want = raw_of(d, d->steps[k]); break; }
    } else {
        for (int k = d->nsteps - 1; k >= 0; k--)
            if (raw_of(d, d->steps[k]) < cur) { want = raw_of(d, d->steps[k]); break; }
    }
    wr(d->path, want);
}

// keep all input devices open; Bluetooth keyboards get a new event number
// when they reconnect, so rescan regularly
static int scan(struct pollfd *fds, char names[][64], int have) {
    DIR *d = opendir("/dev/input");
    if (!d) return have;
    struct dirent *e;
    while ((e = readdir(d)) && have < MAXFD) {
        if (strncmp(e->d_name, "event", 5)) continue;
        int known = 0;
        for (int i = 0; i < have; i++)
            if (!strcmp(names[i], e->d_name)) { known = 1; break; }
        if (known) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        fds[have].fd = fd; fds[have].events = POLLIN;
        snprintf(names[have], 64, "%.63s", e->d_name);
        have++;
    }
    closedir(d);
    return have;
}

int main(int argc, char **argv) {
    if (argc > 1) idle_bat = atoi(argv[1]);
    if (argc > 2) idle_ac  = atoi(argv[2]);

    struct dimmer disp = { BL, disp_steps,
                           (int)(sizeof disp_steps / sizeof *disp_steps),
                           rd(BLMAX), 0 };
    struct dimmer kbd  = { KB, kbd_steps,
                           (int)(sizeof kbd_steps / sizeof *kbd_steps),
                           rd(KBMAX), 0 };

    signal(SIGCHLD, SIG_IGN);                // no zombies to reap
    struct passwd *pw = getpwnam(KIOSKUSER);
    if (pw) {
        u_uid = pw->pw_uid; u_gid = pw->pw_gid;
        u_name = strdup(pw->pw_name); u_home = strdup(pw->pw_dir);
    }

    char wake[64] = "";
    if (u_home) snprintf(wake, sizeof wake, WAKE, (unsigned)u_uid);
    long wakeseen = wake_stamp(wake);

    struct pollfd fds[MAXFD]; char names[MAXFD][64];
    int have = scan(fds, names, 0);

    time_t last = time(NULL), lastscan = last;
    long saved = 0, savedkb = 0, kbd_prev = 0, lastwin = 0;
    int dimmed = 0;
    struct input_event ev[64];

    for (;;) {
        int r = poll(fds, (nfds_t)have, RESCAN * 1000);
        time_t now = time(NULL);

        if (r > 0) {
            int dir = 0, kdir = 0, ktog = 0, nextwin = 0, got = 0;
            for (int i = 0; i < have; i++) {
                if (!fds[i].revents) continue;
                ssize_t n;
                while ((n = read(fds[i].fd, ev, sizeof ev)) > 0) {
                    got = 1;
                    for (size_t k = 0; k < (size_t)n / sizeof ev[0]; k++) {
                        if (ev[k].type != EV_KEY) continue;
                        if (ev[k].value == 0) continue;
                        switch (ev[k].code) {
                        case KEY_BRIGHTNESSUP:    dir  =  1; break;
                        case KEY_BRIGHTNESSDOWN:  dir  = -1; break;
                        case KEY_KBDILLUMUP:      kdir =  1; break;
                        case KEY_KBDILLUMDOWN:    kdir = -1; break;
                        case KEY_KBDILLUMTOGGLE:  ktog =  1; break;
                        case KEY_SCALE:           nextwin =  1; break;
                        case KEY_DASHBOARD:       nextwin = -1; break;
                        }
                    }
                }
                // A device that went away (Bluetooth gone, USB unplugged)
                // reports POLLERR/POLLHUP forever; without cleanup poll()
                // returns immediately and the loop eats a whole core.
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    close(fds[i].fd);
                    fds[i] = fds[have - 1];
                    memcpy(names[i], names[have - 1], sizeof names[i]);
                    have--; i--;                 // handle the moved-in entry in this pass
                }
            }
            // only real input counts as activity, otherwise a dying device
            // keeps the screen awake forever
            if (!got) goto idle_check;

            last = now;

            // waking up does not count as a brightness request, otherwise the
            // first key press after a pause would change the brightness
            if (dimmed) {
                if (saved   > 0) wr(BL, saved);
                if (savedkb > 0) wr(KB, savedkb);
                dimmed = 0; dir = kdir = ktog = nextwin = 0;
            }

            if (nextwin) {
                long t = nowms();
                if (t - lastwin >= STEPMS) {
                    lastwin = t;
                    if (nextwin > 0) sway_do("next", 1);   // swaybar-status: tab or tiled window
                    else sway_do("exec footclient", 0);
                }
            }
            if (dir)  step(&disp, dir);
            if (kdir) step(&kbd,  kdir);
            if (ktog && kbd.max > 0) {
                long t = nowms();
                if (t - kbd.laststep >= STEPMS) {
                    kbd.laststep = t;
                    long cur = rd(KB);
                    if (cur > 0) { kbd_prev = cur; wr(KB, 0); }
                    else wr(KB, kbd_prev > 0 ? kbd_prev : kbd.max / 2);
                }
            }
        }

idle_check:
        if (now - lastscan >= RESCAN) { have = scan(fds, names, have); lastscan = now; }

        long wm = *wake ? wake_stamp(wake) : 0;
        if (wm && wm != wakeseen) {
            wakeseen = wm; last = now;
            if (dimmed) {
                if (saved   > 0) wr(BL, saved);
                if (savedkb > 0) wr(KB, savedkb);
                dimmed = 0;
            }
        }

        if (!dimmed) {
            int ac = rd(ACPATH) == 1;
            if (now - last >= (ac ? idle_ac : idle_bat)) {
                saved = rd(BL); savedkb = rd(KB);
                // 0 % may be intentional -- then it stays off
                if (saved > 0 || savedkb > 0) {
                    if (saved   > 0) wr(BL, 0);
                    if (savedkb > 0) wr(KB, 0);
                    dimmed = 1;
                }
            }
        }
    }
}
