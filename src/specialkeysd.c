// specialkeysd: bedient die Sondertasten, die im Kiosk sonst niemand mehr
// abfaengt -- Helligkeit (Display und Tastatur), Exposé/F3 zum Umschalten
// des sway-Fensters -- und dimmt bei
// Untaetigkeit ab.
// Seit 2026-11-01 ohne tmux: F3/F4 gehen per swaymsg an sway, die Fenster
// liegen dort als Tabs. Ctrl+F3 (Ansichtswechsel) ist entfallen.
// Unter cage gibt es weder Idle- noch Tastenverwaltung; powerdevil und
// kglobalaccel haben das frueher gemacht.
//
// Hiess bis 2026-09-06 idle-dim, weil es mit dem Abdimmen angefangen hat --
// das ist laengst der kleinere Teil.
//
// build: make specialkeysd && make install-daemon   (siehe README)
// Laeuft als System-Dienst, weil nur root sowohl /dev/input/* lesen als auch
// /sys/class/backlight schreiben darf.
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

#ifndef BL                       // ueberschreibbar fuer Tests
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
#ifndef KIOSKUSER                // in dessen Namen swaymsg gerufen wird
#define KIOSKUSER "michi"
#endif
#ifndef ACPATH
#define ACPATH "/sys/class/power_supply/ADP1/online"
#endif
#define MAXFD 32
#define RESCAN 5                     // Sekunden: neue Eingabegeraete einsammeln
#define STEPMS 120                   // Entprellung: Video-Bus *und* Tastatur
                                     // melden denselben Tastendruck
#define WAKE "/run/user/%u/specialkeysd-wake"
                                     // Wer ohne Eingabegeraet schreibt -- das
                                     // Diktat von m5assistant tippt per
                                     // "tmux send-keys" direkt in den Puffer --
                                     // faesst diese Datei an und gilt damit als
                                     // Aktivitaet. Im Laufzeitverzeichnis des
                                     // Kioskusers, damit er ohne Rechte drankommt.

static int idle_bat = 1800;          // Timeout auf Akku
static int idle_ac  = 3600;          // Timeout am Netzteil

// Feste Stufen in Promille, unten fein und oben grob: der Wert steuert die
// PWM-Einschaltdauer, und die Wahrnehmung ist ungefaehr die vierte Wurzel
// davon -- ein Promille ist im Dunkeln deutlich sichtbar, bei 90 % dagegen
// nicht mehr. Promille statt Prozent, weil 1 % (13 von 1388 Schritten) nachts
// noch zu hell ist; das Panel nimmt Rohwerte bis hinunter zu 1.
static const int disp_steps[] = {0,2,5,10,20,50,100,200,300,400,
                                 500,600,700,800,900,1000};
static const int kbd_steps[]  = {0,10,100,500,1000};

struct dimmer {
    const char *path;
    const int  *steps;
    int         nsteps;
    long        max;             // <= 0: Geraet nicht vorhanden
    long        laststep;
};

// Wem sway gehoert; root darf dessen Socket nicht einfach benutzen.
static uid_t u_uid; static gid_t u_gid; static char *u_name, *u_home;

// F3 schaltet weiter (wie Alt-Tab), F4 macht ein Terminal auf.
// Der Socket-Name enthaelt die PID von sway, darum per glob suchen; ohne
// laufendes sway passiert nichts.
static void sway_do(const char *cmd) {
    if (!u_home) return;
    pid_t p = fork();
    if (p != 0) return;                      // nicht warten, SIGCHLD ist ignoriert
    if (initgroups(u_name, u_gid) || setgid(u_gid) || setuid(u_uid)) _exit(127);
    char pat[64]; glob_t g;
    snprintf(pat, sizeof pat, "/run/user/%u/sway-ipc.*.sock", (unsigned)u_uid);
    if (glob(pat, 0, NULL, &g) || g.gl_pathc == 0) _exit(1);
    setenv("SWAYSOCK", g.gl_pathv[g.gl_pathc - 1], 1);
    setenv("HOME", u_home, 1);
    execlp("swaymsg", "swaymsg", cmd, (char *)NULL);
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

// Promille-Stufe -> Rohwert des Geraets
static long raw_of(const struct dimmer *d, int permille) {
    return (permille * d->max + 500) / 1000;
}

// eine Stufe hoch oder runter
static void step(struct dimmer *d, int dir) {
    if (d->max <= 0) return;
    long t = nowms();
    if (t - d->laststep < STEPMS) return;
    d->laststep = t;
    long cur = rd(d->path); if (cur < 0) cur = 0;
    // Im Rohwert vergleichen, nicht in Promille: bei der Ruecktransformation
    // fallen mehrere Stufen auf denselben gerundeten Promillewert, und ein
    // Druck haette dann keine Wirkung mehr.
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

// alle Eingabegeraete offen halten; Bluetooth-Tastaturen wechseln beim
// Wiederverbinden die event-Nummer, deshalb regelmaessig neu einsammeln
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

    signal(SIGCHLD, SIG_IGN);                // keine Zombies einsammeln muessen
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
                // Ein abgemeldetes Geraet (Bluetooth weg, USB gezogen) meldet
                // von da an dauerhaft POLLERR/POLLHUP; ohne Aufraeumen kehrt
                // poll() sofort zurueck und die Schleife frisst einen Kern.
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    close(fds[i].fd);
                    fds[i] = fds[have - 1];
                    memcpy(names[i], names[have - 1], sizeof names[i]);
                    have--; i--;                 // Nachruecker im selben Durchlauf
                }
            }
            // nur echte Eingaben zaehlen als Aktivitaet, sonst haelt ein
            // sterbendes Geraet den Bildschirm dauerhaft wach
            if (!got) goto idle_check;

            last = now;

            // Aufwecken zaehlt nicht als Helligkeitswunsch, sonst verpufft
            // der erste Tastendruck nach einer Pause
            if (dimmed) {
                if (saved   > 0) wr(BL, saved);
                if (savedkb > 0) wr(KB, savedkb);
                dimmed = 0; dir = kdir = ktog = nextwin = 0;
            }

            if (nextwin) {
                long t = nowms();
                if (t - lastwin >= STEPMS) {
                    lastwin = t;
                    if (nextwin > 0) sway_do("workspace next_on_output");
                    else sway_do("exec footclient");
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
                // 0 % kann jetzt gewollt sein -- dann bleibt es aus
                if (saved > 0 || savedkb > 0) {
                    if (saved   > 0) wr(BL, 0);
                    if (savedkb > 0) wr(KB, 0);
                    dimmed = 1;
                }
            }
        }
    }
}
