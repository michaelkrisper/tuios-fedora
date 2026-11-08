
// swaybar-status: the kiosk's single bar -- tabs on the left; on the right
// Claude (weekly limit), CPU, RAM, network, battery, WLAN, volume, clock.
// Output is Pango markup (bar { pango_markup enabled }). Called tmux-status
// until 2026-11-01.
// build: make && make install   (see README)
//
// Tabs: every window gets its own workspace "N:title", so the bar's workspace
// buttons (strip_workspace_numbers) are the tabs. In --loop mode the program
// therefore also talks sway IPC: new window on an occupied workspace -> its own
// workspace behind the last one; window title -> workspace name; last window
// closed -> previous tab. sway's own tab bar is no use: it is a second bar with
// no room for the status.
//
// Claude: `statusline --bar` (repo claude-statusline) writes the weekly limit to
// $XDG_RUNTIME_DIR/claude-bar/limits. inotify on that directory shows changes
// immediately instead of on the next tick.
//
// Volume: `wpctl get-volume` is the one fork in here (13 ms), so it does not run
// per tick: only at start and on sway's tick "volume", sent by the volume key
// bindings. Changes without a key (headphone jack) show up on the next press.
//
// WLAN: nl80211 scan cache over netlink, see the WLAN section.
//
// Two modes:
//   no argument  : print once, CPU delta via a cache file (for testing)
//   --loop [sec] : keep running and write a line every [sec] seconds
//
// --loop is the normal case: swaybar starts status_command once and keeps
// reading lines -- this saves a fork/exec per tick, which at ~1 ms was by far
// the largest cost. The CPU counters simply stay in memory, no cache file
// needed. Exits by itself via SIGPIPE as soon as swaybar closes the read end.
//
// Deliberately no stdio streams: printf/fopen pull in buffer allocation and
// stdio init (snprintf for the sway commands is harmless). Everything goes
// through open/read/write and a hand-written number formatter.
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>                  // snprintf only, no FILE buffer
#include <string.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BATDIR                      // overridable for tests
#define BATDIR "/sys/class/power_supply/BAT0/"
#endif
#define WLANIF "wlp3s0"

static char out[2048];
static int olen;

static void put(const char *s) {
    int n = (int)strlen(s);
    if (olen + n < (int)sizeof out) { memcpy(out + olen, s, n); olen += n; }
}

static void putnum(long v, int pad) {
    char tmp[24]; int i = 0;
    if (v < 0) { put("-"); v = -v; }
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i < pad) tmp[i++] = '0';
    while (i-- > 0 && olen < (int)sizeof out - 1) out[olen++] = tmp[i];
}

// Fixed field width so the bar does not jump. Padding goes BEFORE the symbol:
// the number sticks to its symbol, while the right edge of every field, and
// thus the total length, stays put.
static int digits(long v) {
    int d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
}

static void pad(int n) { while (n-- > 0) put(" "); }

static void padnum(long v, int width) { pad(width - digits(v)); }

static int slurp(const char *path, char *b, int cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int n = (int)read(fd, b, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    b[n] = 0;
    return n;
}

static long num(const char *path) {
    char b[64];
    return slurp(path, b, sizeof b) ? strtol(b, NULL, 10) : -1;
}

// next unsigned number at or after *p; advances the pointer
static unsigned long nextul(const char **p) {
    while (**p && (**p < '0' || **p > '9')) (*p)++;
    unsigned long v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (unsigned long)(*(*p)++ - '0');
    return v;
}

static void cpu_totals(unsigned long *busy, unsigned long *idle) {
    char b[256];
    *busy = *idle = 0;
    if (!slurp("/proc/stat", b, sizeof b)) return;
    const char *p = b + 3;                                  // skip "cpu"
    unsigned long u = nextul(&p), ni = nextul(&p), sy = nextul(&p);
    unsigned long id = nextul(&p), io = nextul(&p);
    *idle = id + io;                                        // iowait counts as not busy
    *busy = u + ni + sy;
}

// Virtual interfaces count the real NIC's traffic a second time -- so bridges,
// veth pairs and loopback are skipped.
static int skip_if(const char *n, int len) {
    static const char *const pre[] = { "lo", "veth", "docker", "podman", "br-",
                                       "virbr", "cni", "tun", "tap", "vnet", 0 };
    for (int i = 0; pre[i]; i++) {
        int l = (int)strlen(pre[i]);
        if (len >= l && memcmp(n, pre[i], (size_t)l) == 0) return 1;
    }
    return 0;
}

static void net_totals(unsigned long *rx, unsigned long *tx) {
    char b[4096];
    *rx = *tx = 0;
    if (!slurp("/proc/net/dev", b, sizeof b)) return;
    const char *p = b;
    for (int i = 0; i < 2 && p; i++) { p = strchr(p, '\n'); if (p) p++; }
    while (p && *p) {
        while (*p == ' ') p++;
        const char *name = p, *colon = strchr(p, ':'), *eol = strchr(p, '\n');
        if (!colon || (eol && colon > eol)) break;
        const char *q = colon + 1;
        unsigned long r = nextul(&q);
        for (int i = 0; i < 7; i++) nextul(&q);         // packets .. multicast
        unsigned long t = nextul(&q);
        if (!skip_if(name, (int)(colon - name))) { *rx += r; *tx += t; }
        p = eol ? eol + 1 : NULL;
    }
}

// bytes/s from two counter readings, -1 if there is no delta yet
static long rate(unsigned long now, unsigned long prev, long secs) {
    if (secs <= 0 || now < prev) return -1;             // Reset/Overflow
    return (long)((now - prev) / (unsigned long)secs);
}

// Rate in Mbit/s with one decimal, without unit: "0.0", "33.2".
// The unit is not shown anywhere -- in your own bar you know it.
// Anything below 0.1 Mbit is background noise and becomes 0.0 -- finer
// resolution is pointless. Capped at 99.9 so the integer part has at most two
// digits (the line does ~50 anyway).
// Mbit is decimal (1e6 bit), as usual for link speeds:
// bytes/s * 8 / 1e6, i.e. in tenths bytes/s / 12500.
static long tenths(long bps) {
    long t = bps / 12500;
    return t > 999 ? 999 : t;
}

// width of the integer part, so the caller can pad before the arrow
static int ratewidth(long bps) {
    return bps < 0 ? 2 : digits(tenths(bps) / 10);
}

static void putrate(long bps) {
    if (bps < 0) { put("--.-"); return; }
    long t = tenths(bps);
    putnum(t / 10, 0); put("."); putnum(t % 10, 0);
}

// percent from two counter readings, -1 if there is no usable delta
static long cpu_pct(unsigned long busy, unsigned long idle,
                    unsigned long pbusy, unsigned long pidle) {
    long dt = (long)((busy + idle) - (pbusy + pidle));
    long di = (long)(idle - pidle);
    if (dt <= 0) return -1;
    if (di < 0) di = 0;
    if (di > dt) di = dt;                                   // guard against counter resets
    return 100 - di * 100 / dt;
}

// RAM in use from a /proc/meminfo buffer, -1 if unparsable. MemAvailable, not
// MemFree: page cache is reclaimable and would otherwise read as ~95 % all day.
static long mem_pct(const char *b) {
    const char *t = strstr(b, "MemTotal:"), *a = strstr(b, "MemAvailable:");
    if (!t || !a) return -1;
    unsigned long tot = nextul(&t), av = nextul(&a);
    if (!tot || av > tot) return -1;
    return (long)((tot - av) * 100 / tot);
}

// Remaining time from a weighted average of the last 12 power samples
// (12 x 5 s = one minute). Newer samples weigh more (weight rank^1.2), so the
// display follows load changes without jumping every tick. Same algorithm as
// the powermeter plasmoid. Switching between charging and discharging drops
// the history. Weights are precomputed (x1000) so no pow() or libm is needed.
#define HIST 12
static const long weight[HIST] = { 1000, 2297, 3737, 5278, 6899, 8586,
                                   10330, 12126, 13967, 15849, 17769, 19725 };
static long hist[HIST];
static int histn, histdis = -1;

static long smooth_power(long pw, int dis) {
    if (pw <= 0) { histn = 0; return -1; }
    if (dis != histdis) { histn = 0; histdis = dis; }
    if (histn == HIST) { memmove(hist, hist + 1, sizeof hist - sizeof *hist); histn--; }
    hist[histn++] = pw;
    long num = 0, den = 0;
    for (int i = 0; i < histn; i++) {              // i=0 oldest ... newest weighs most
        num += hist[i] * weight[i]; den += weight[i];
    }
    return num / den;
}

// With _OSI(Darwin) Linux binds the SBS driver instead of the generic ACPI
// battery. It reports charge_*/current_* in uAh/uA instead of energy_*/power_*
// in uWh/uW. If the energy variant is missing it is converted via voltage_now,
// so the bar shows watts either way. (This makes the remaining time slightly
// inaccurate because voltage_now drops with the charge level -- irrelevant
// for a display rounded to 5 min.)
static long as_uW(const char *energy, const char *charge) {
    long v = num(energy);
    if (v >= 0) return v;
    long c = num(charge), u = num(BATDIR "voltage_now");
    if (c < 0 || u < 0) return -1;
    return c * (u / 1000) / 1000;
}

// Colour change as a Pango span; NULL = back to the bar's foreground colour.
static int inspan;
static void col(const char *span) {
    if (inspan) put("</span>");
    inspan = span != NULL;
    if (span) put(span);
}
#define FG(c) "<span foreground='" c "'>"

// Icons are hard to see at text size. Bold does not help: the Nerd Font glyphs
// are identical in every weight of the font.
static void sym(const char *glyph) {
    put("<span font_size='14pt'>"); put(glyph); put("</span>");
}

// ---------------------------------------------------------------- Claude

static char rtdir[256];             // $XDG_RUNTIME_DIR/claude-bar

static void put_limits(void) {
    char path[sizeof rtdir + 8], b[600];
    if (!rtdir[0]) return;
    snprintf(path, sizeof path, "%s/limits", rtdir);
    struct stat st;
    // older than one weekly window: stale
    if (stat(path, &st) || time(NULL) - st.st_mtime > 7 * 86400) return;
    int l = slurp(path, b, sizeof b);
    while (l && (b[l - 1] == '\n' || b[l - 1] == ' ')) b[--l] = 0;
    if (l) { put(b); put("  "); }
}

// ---------------------------------------------------------------- Volume

static long vol = -1;              // percent; -1 = unknown, field is left out
static int volmute;

// "Volume: 0.60 [MUTED]\n" -> 60 and *muted = 1; anything else -> -1
static long vol_parse(const char *s, int *muted) {
    if (strncmp(s, "Volume: ", 8)) return -1;
    const char *p = s + 8;
    long whole = (long)nextul(&p), hundredths = (long)nextul(&p);
    *muted = strstr(p, "[MUTED]") != NULL;
    return whole * 100 + hundredths;
}

static void vol_refresh(void) {
    static char *const av[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    int p[2];
    pid_t pid;
    vol = -1;
    if (pipe2(p, O_CLOEXEC)) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int err = posix_spawnp(&pid, "wpctl", &fa, NULL, av, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (!err) {
        // Read to EOF: wpctl writes "[MUTED]" separately. The timeout and the
        // kill are there because a hanging PipeWire must not freeze the tabs.
        struct pollfd pf = { p[0], POLLIN, 0 };
        char b[64];
        int n = 0, r;
        while (n < (int)sizeof b - 1 && poll(&pf, 1, 500) > 0 &&
               (r = (int)read(p[0], b + n, sizeof b - 1 - (size_t)n)) > 0)
            n += r;
        b[n] = 0;
        vol = vol_parse(b, &volmute);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    close(p[0]);
}

static void put_volume(void) {
    if (vol < 0) return;
    int silent = volmute || vol == 0;
    col(silent ? FG("#6c7086") : FG("#bac2de"));
    // Crossed out, then 0, 1, 2, 3 waves: nf-md-volume_off, _low, _medium, _high.
    // Three waves only exist as nf-fa-volume_up; at 14pt it is as wide and as tall
    // as the md glyphs.
    sym(silent ? "\U000F0581" : vol <= 10 ? "\U000F057F" : vol <= 40 ? "\U000F0580"
        : vol <= 70 ? "\U000F057E" : "\uF028");
    col(NULL);
}

// ---------------------------------------------------------------- WLAN
// Connection and signal come from nl80211's scan cache: no fork, no root, no
// firmware access. It is the only unprivileged source here: the kernel has no
// /proc/net/wireless (CFG80211_WEXT off) and wl answers GET_STATION with EPERM.
// So "connected" is current, but the signal is only as new as the last scan.

static int wifibars;                // 1-4, 0 = not connected
static int nlfd = -1, nlfam;        // generic netlink socket, nl80211's family id
static char nlbuf[32768] __attribute__((aligned(4)));

static struct nlattr *nla_next(char **p, const char *end) {
    struct nlattr *a = (struct nlattr *)*p;
    if (end - *p < NLA_HDRLEN || a->nla_len < NLA_HDRLEN || a->nla_len > end - *p) return NULL;
    *p += NLA_ALIGN(a->nla_len);
    return a;
}

static int wifi_bars(int32_t mbm) {
    return mbm >= -5500 ? 4 : mbm >= -6700 ? 3 : mbm >= -7500 ? 2 : 1;
}

// One recv() of a GET_SCAN dump; the associated BSS sets *bars.
// 1 = dump finished, 0 = more to come, -1 = error reply or garbage.
static int wifi_msgs(char *b, int n, int *bars) {
    for (int off = 0; off < n; ) {
        struct nlmsghdr *h = (struct nlmsghdr *)(b + off);
        if (n - off < NLMSG_HDRLEN || (int)h->nlmsg_len < NLMSG_HDRLEN ||
            (int)h->nlmsg_len > n - off) return -1;
        if (h->nlmsg_type == NLMSG_DONE) return 1;
        if (h->nlmsg_type == NLMSG_ERROR) return -1;
        char *p = (char *)h + NLMSG_HDRLEN + GENL_HDRLEN, *end = (char *)h + h->nlmsg_len;
        for (struct nlattr *a; (a = nla_next(&p, end)); ) {
            if ((a->nla_type & NLA_TYPE_MASK) != NL80211_ATTR_BSS) continue;
            char *q = (char *)a + NLA_HDRLEN, *qend = (char *)a + a->nla_len;
            uint32_t status = NL80211_BSS_STATUS_AUTHENTICATED;
            int32_t mbm = 0;
            for (struct nlattr *c; (c = nla_next(&q, qend)); ) {
                int ty = c->nla_type & NLA_TYPE_MASK;
                if (ty == NL80211_BSS_STATUS) memcpy(&status, (char *)c + NLA_HDRLEN, 4);
                if (ty == NL80211_BSS_SIGNAL_MBM) memcpy(&mbm, (char *)c + NLA_HDRLEN, 4);
            }
            if (status == NL80211_BSS_STATUS_ASSOCIATED) *bars = wifi_bars(mbm);
        }
        off += NLMSG_ALIGN(h->nlmsg_len);
    }
    return 0;
}

static int nl_send(int type, int cmd, int flags, int attr, const void *d, int len) {
    struct { struct nlmsghdr n; struct genlmsghdr g; struct nlattr a; char d[8]; } r;
    memset(&r, 0, sizeof r);
    r.n.nlmsg_len = (uint32_t)(NLMSG_HDRLEN + GENL_HDRLEN + NLA_HDRLEN + len);
    r.n.nlmsg_type = (uint16_t)type;
    r.n.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags);
    r.g.cmd = (uint8_t)cmd;
    r.a.nla_type = (uint16_t)attr;
    r.a.nla_len = (uint16_t)(NLA_HDRLEN + len);
    memcpy(r.d, d, (size_t)len);
    return send(nlfd, &r, r.n.nlmsg_len, 0) == (ssize_t)r.n.nlmsg_len ? 0 : -1;
}

static void nl_close(void) {
    if (nlfd >= 0) close(nlfd);
    nlfd = -1;
}

static int nl_open(void) {
    nlfd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (nlfd < 0) return -1;
    struct timeval tv = { 0, 500000 };          // a stuck driver must not freeze the tabs
    setsockopt(nlfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    nlfam = 0;
    if (!nl_send(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, CTRL_ATTR_FAMILY_NAME, "nl80211", 8)) {
        int n = (int)recv(nlfd, nlbuf, sizeof nlbuf, 0);
        if (n >= (int)(NLMSG_HDRLEN + GENL_HDRLEN) && ((struct nlmsghdr *)nlbuf)->nlmsg_type == GENL_ID_CTRL) {
            char *p = nlbuf + NLMSG_HDRLEN + GENL_HDRLEN;
            for (struct nlattr *a; (a = nla_next(&p, nlbuf + n)); )
                if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID) {
                    uint16_t id;
                    memcpy(&id, (char *)a + NLA_HDRLEN, 2);
                    nlfam = id;
                }
        }
    }
    if (!nlfam) { nl_close(); return -1; }
    return 0;
}

static int wifi_read(void) {
    uint32_t idx = if_nametoindex(WLANIF);
    if (!idx || (nlfd < 0 && nl_open())) return 0;
    int bars = 0, r = -1;
    if (!nl_send(nlfam, NL80211_CMD_GET_SCAN, NLM_F_DUMP, NL80211_ATTR_IFINDEX, &idx, 4)) {
        do {
            int n = (int)recv(nlfd, nlbuf, sizeof nlbuf, 0);
            r = n > 0 ? wifi_msgs(nlbuf, n, &bars) : -1;
        } while (r == 0);
    }
    if (r < 0) nl_close();                      // next read starts clean: stale replies, family id
    return bars;
}

static void put_wifi(void) {
    // nf-md-wifi_strength_off_outline, _1, _2, _3, _4
    static const char *const glyph[] = { "\U000F092E", "\U000F091F", "\U000F0922", "\U000F0925", "\U000F0928" };
    col(wifibars ? FG("#94e2d5") : FG("#f38ba8"));
    sym(glyph[wifibars]);
    col(NULL); put(" ");
}

static void render(long cpu, long rxr, long txr) {
    olen = 0; inspan = 0;
    put_limits();
    pad(cpu < 0 ? 1 : 3 - digits(cpu));
    col(cpu >= 70 ? FG("#f38ba8") : FG("#89b4fa"));
    // CPU and battery fill more of the em than the other icons: at 14pt they stand
    // 4 and 7 px above the digits. 12pt and text size match the digits' height.
    put("<span font_size='12pt'></span>");
    if (cpu < 0) put("--"); else putnum(cpu, 0);
    put("%"); col(NULL); put(" ");
    char mi[512];
    long mem = slurp("/proc/meminfo", mi, sizeof mi) ? mem_pct(mi) : -1;
    pad(mem < 0 ? 1 : 3 - digits(mem));
    col(mem >= 90 ? FG("#f38ba8") : FG("#cba6f7"));
    // nf-md-chip: a flat bar with pins reads as a RAM module; nf-md-memory is a
    // square chip that looks just like the CPU icon. 12pt for the same reason as CPU.
    put("<span font_size='12pt'>\U000F061A</span>");
    if (mem < 0) put("--"); else putnum(mem, 0);
    put("%"); col(NULL); put(" ");
    pad(2 - ratewidth(rxr));
    col(FG("#94e2d5")); put("↓"); putrate(rxr);
    put(" "); pad(2 - ratewidth(txr)); put("↑"); putrate(txr);
    col(NULL); put(" ");
    long cap = num(BATDIR "capacity");
    char st[32];
    if (cap >= 0 && slurp(BATDIR "status", st, sizeof st)) {
        int dis = strncmp(st, "Discharging", 11) == 0;
        long pw = as_uW(BATDIR "power_now",   BATDIR "current_now");
        long en = as_uW(BATDIR "energy_now",  BATDIR "charge_now");
        // energy_full practically never changes -> read it once
        static long ef = -1;
        if (ef < 0) ef = as_uW(BATDIR "energy_full", BATDIR "charge_full");
        long rest = dis ? en : ef - en;
        long avg = smooth_power(pw, dis);

        padnum(cap, 3);
        int crit = dis && cap < 15;             // red background up to the end of the line
        col(crit ? "<span foreground='#1e1e2e' background='#f38ba8'>"
                 : dis && cap <= 30 ? FG("#fab387") : FG("#a6e3a1"));
        put(dis ? "󰁹" : "󰂄");
        putnum(cap, 0); put("%");
        // Remaining time rounded to 5 min so the display does not twitch
        // every tick. If a value is missing the field is left blank.
        if (avg > 0 && rest > 0) {
            long m = (rest * 60 / avg + 2) / 5 * 5;
            put(" "); putnum(m / 60, 0); put(":"); putnum(m % 60, 2); put("h");
        } else {
            pad(6);                                 // keep the field's width reserved
        }
        // power " NNW" = 4 characters, rounded rather than truncated
        if (pw > 0) {
            long w = (pw + 500000) / 1000000;
            put(" "); padnum(w, 2); putnum(w, 0); put("W");
        } else {
            pad(4);
        }
        col(NULL); put(" ");
    }
    put_wifi();
    put_volume();
    time_t t = time(NULL); struct tm tm;
    localtime_r(&t, &tm);
    put(" "); putnum(tm.tm_hour, 2);
    put(":"); putnum(tm.tm_min, 2); put(" ");
}

// Only write when the line changed -- if nothing changed, swaybar does not
// redraw and sway does not composite.
static char prev[sizeof out];
static int prevlen = -1;

static int emit(int nl) {
    if (nl) put("\n");
    if (olen == prevlen && memcmp(out, prev, (size_t)olen) == 0) return 0;
    memcpy(prev, out, (size_t)olen); prevlen = olen;
    return write(1, out, (size_t)olen) == olen ? 0 : 1;
}

// ---------------------------------------------------------------- JSON
// Minimal parser for sway's replies: values go into a flat array, children
// are linked via kid/next, strings are unescaped in place in the buffer (the
// result is never longer than the input).

typedef struct { char t; const char *k, *s; double n; int kid, next; } J;
static J *jv; static int jn, jcap;
static char *jp;

static int jnew(char t) {
    if (jn == jcap) {
        jcap = jcap ? jcap * 2 : 1024;
        J *nv = realloc(jv, (size_t)jcap * sizeof *jv);
        if (!nv) _exit(1);
        jv = nv;
    }
    jv[jn] = (J){ t, 0, 0, 0, -1, -1 };
    return jn++;
}

static void jws(void) { while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t') jp++; }

static unsigned hex4(const char *h) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = h[i];
        v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                               : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        if (!c) break;
    }
    return v;
}

static char *jstr(void) {
    char *o = ++jp, *start = o;
    while (*jp && *jp != '"') {
        if (*jp != '\\') { *o++ = *jp++; continue; }
        jp++;
        switch (*jp) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': case 'b': case 'f': *o++ = ' '; break;
        case 'u': {
            unsigned c = hex4(jp + 1); jp += 4;
            if (c >= 0xD800 && c < 0xDC00 && jp[1] == '\\' && jp[2] == 'u') {
                c = 0x10000 + ((c - 0xD800) << 10) + (hex4(jp + 3) - 0xDC00); jp += 6;
            }
            if (c < 0x80) *o++ = (char)c;
            else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
            else if (c < 0x10000) { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 63));
                                    *o++ = (char)(0x80 | (c & 63)); }
            else { *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | (c >> 12 & 63));
                   *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63)); }
            break;
        }
        case 0: jp--; break;
        default: *o++ = *jp;
        }
        jp++;
    }
    if (*jp) jp++;
    *o = 0;
    return start;
}

static int jparse(void) {
    jws();
    int i, last = -1;
    char c = *jp;
    if (c == '{' || c == '[') {
        i = jnew(c == '{' ? 'o' : 'a'); jp++;
        for (;;) {
            jws();
            if (!*jp) return -1;
            if (*jp == '}' || *jp == ']') { jp++; return i; }
            if (*jp == ',') { jp++; continue; }
            char *k = NULL;
            if (c == '{') {
                if (*jp != '"') return -1;
                k = jstr(); jws();
                if (*jp == ':') jp++;
            }
            int v = jparse();
            if (v < 0) return -1;
            jv[v].k = k;
            if (last < 0) jv[i].kid = v; else jv[last].next = v;
            last = v;
        }
    }
    if (c == '"') { i = jnew('s'); jv[i].s = jstr(); return i; }
    if (c == 't') { jp += 4; i = jnew('b'); jv[i].n = 1; return i; }
    if (c == 'f') { jp += 5; return jnew('b'); }
    if (c == 'n') { jp += 4; return jnew('z'); }
    char *end;
    double d = strtod(jp, &end);
    if (end == jp) return -1;
    jp = end; i = jnew('n'); jv[i].n = d;
    return i;
}

static int jroot(char *buf) { jn = 0; jp = buf; return jparse(); }

static int jget(int o, const char *k) {
    if (o < 0) return -1;
    for (int c = jv[o].kid; c >= 0; c = jv[c].next)
        if (jv[c].k && strcmp(jv[c].k, k) == 0) return c;
    return -1;
}
static const char *jsget(int o, const char *k) { int c = jget(o, k); return c >= 0 && jv[c].t == 's' ? jv[c].s : NULL; }
static double jnget(int o, const char *k) { int c = jget(o, k); return c >= 0 ? jv[c].n : -1; }

// ---------------------------------------------------------------- sway-IPC

static int readall(int fd, char *b, size_t n) {
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}

static int ipc_open(void) {
    const char *p = getenv("SWAYSOCK");
    if (!p || strlen(p) >= sizeof ((struct sockaddr_un *)0)->sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, p);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

static int ipc_send(int fd, uint32_t type, const char *msg) {
    uint32_t len = (uint32_t)strlen(msg);
    char h[14];
    memcpy(h, "i3-ipc", 6); memcpy(h + 6, &len, 4); memcpy(h + 10, &type, 4);
    return write(fd, h, 14) == 14 && write(fd, msg, len) == (ssize_t)len ? 0 : -1;
}

static char *ipc_recv(int fd, uint32_t *type) {
    char h[14]; uint32_t len;
    if (readall(fd, h, 14)) return NULL;
    memcpy(&len, h + 6, 4); memcpy(type, h + 10, 4);
    char *b = malloc((size_t)len + 1);
    if (!b || readall(fd, b, len)) { free(b); return NULL; }
    b[len] = 0;
    return b;
}

static char *ipc_call(int fd, uint32_t type, const char *msg) {
    uint32_t t;
    return ipc_send(fd, type, msg) ? NULL : ipc_recv(fd, &t);
}

// ---------------------------------------------------------------- Tabs

#define MAXWS 64
#define MAXLEAF 64
#define MAXLEN 28                   // tab label, see label()

// tiling windows of a subtree, most recently focused first
static void leaves(int node, int *out, int *n) {
    int nodes = jget(node, "nodes"), focus = jget(node, "focus");
    if (nodes < 0 || jv[nodes].kid < 0) {
        if (jget(node, "pid") >= 0 && *n < MAXLEAF) out[(*n)++] = node;
        return;
    }
    for (int f = focus >= 0 ? jv[focus].kid : -1; f >= 0; f = jv[f].next)
        for (int c = jv[nodes].kid; c >= 0; c = jv[c].next)
            if (jnget(c, "id") == jv[f].n) leaves(c, out, n);
    for (int c = jv[nodes].kid; c >= 0; c = jv[c].next) {
        int seen = 0;
        for (int f = focus >= 0 ? jv[focus].kid : -1; f >= 0; f = jv[f].next)
            if (jnget(c, "id") == jv[f].n) seen = 1;
        if (!seen) leaves(c, out, n);
    }
}

static int workspaces(int root, int *ws) {
    int n = 0, outs = jget(root, "nodes");
    for (int o = outs >= 0 ? jv[outs].kid : -1; o >= 0; o = jv[o].next) {
        const char *on = jsget(o, "name");
        if (on && strcmp(on, "__i3") == 0) continue;        // Scratchpad
        int w = jget(o, "nodes");
        for (int k = w >= 0 ? jv[w].kid : -1; k >= 0 && n < MAXWS; k = jv[k].next) {
            const char *t = jsget(k, "type");
            if (t && strcmp(t, "workspace") == 0) ws[n++] = k;
        }
    }
    return n;
}

// " and \ must not end up in names: sway's parser keeps \" literally instead
// of unescaping it.
// "<path>: <command/title>" -> "<last path component>: <command/title>",
// truncated at the end; a bare path is truncated at the front, because the
// end of a path is what matters.
static void label(char *dst, size_t cap, const char *t) {
    if (!t) t = "?";
    char tmp[512];
    const char *sep = strstr(t, ": ");
    int tail = sep != NULL;
    if (sep) {
        const char *b = t;
        for (const char *q = t; q < sep; q++) if (*q == '/') b = q + 1;
        snprintf(tmp, sizeof tmp, "%.*s%s", (int)(sep - b), b, sep);
        t = tmp;
    }
    size_t cps = 0;
    for (const char *p = t; *p; p++) if ((*p & 0xC0) != 0x80) cps++;
    const char *p = t, *end = t + strlen(t);
    size_t o = 0;
    if (cps > MAXLEN && !tail) {
        size_t skip = cps - (MAXLEN - 1);
        while (skip) { p++; while ((*p & 0xC0) == 0x80) p++; skip--; }
        memcpy(dst, "\xE2\x80\xA6", 3); o = 3;           // …
    } else if (cps > MAXLEN) {
        end = t;
        for (size_t k = 0; k < MAXLEN - 1; k++) { end++; while ((*end & 0xC0) == 0x80) end++; }
    }
    for (; p < end && o + 4 < cap; p++)
        dst[o++] = *p == '"' ? '\'' : *p == '\\' ? '/' : *p == '\n' ? ' ' : *p;
    if (end < t + strlen(t) && o + 4 <= cap) { memcpy(dst + o, "\xE2\x80\xA6", 3); o += 3; }
    dst[o] = 0;
}

static char cmdbuf[16384];
static int cmdlen;
static void cmd_add(const char *s) {
    int n = (int)strlen(s);
    if (cmdlen + n + 2 < (int)sizeof cmdbuf) {
        if (cmdlen) { memcpy(cmdbuf + cmdlen, "; ", 2); cmdlen += 2; }
        memcpy(cmdbuf + cmdlen, s, (size_t)n); cmdlen += n;
        cmdbuf[cmdlen] = 0;
    }
}

// Tiled mode (Ctrl+Fn+F3): all windows on one workspace, laid out by
// autotiling -- whichever window has focus gets split along its longer side,
// so the next window opens beside it (wide) or below it (tall). sway itself
// only knows manual splith/splitv. The workspace button then lists every
// window, the active one highlighted (swaybar renders Pango markup there too).
// Lost on restart: swaybar-status starts in tab mode and spreads the windows.
static int tiled;

// tiling windows in screen order, each with its parent container
static void inorder(int node, int parent, int *out, int *par, int *n) {
    int nodes = jget(node, "nodes");
    if (nodes < 0 || jv[nodes].kid < 0) {
        if (jget(node, "pid") >= 0 && *n < MAXLEAF) { par[*n] = parent; out[(*n)++] = node; }
        return;
    }
    for (int c = jv[nodes].kid; c >= 0; c = jv[c].next) inorder(c, node, out, par, n);
}

static int isfocused(int node) { int f = jget(node, "focused"); return f >= 0 && jv[f].n; }

// workspace holding the focused window (or the focused empty workspace)
static int curws(int *ws, int nws) {
    for (int i = 0; i < nws; i++) {
        if (isfocused(ws[i])) return i;
        int lv[MAXLEAF], pr[MAXLEAF], n = 0;
        inorder(ws[i], -1, lv, pr, &n);
        for (int k = 0; k < n; k++) if (isfocused(lv[k])) return i;
    }
    return -1;
}

// split the focused window along its longer side, unless its parent already
// runs that way (otherwise every focus change would nest another container)
static void autotile(int ws) {
    int lv[MAXLEAF], pr[MAXLEAF], n = 0;
    inorder(ws, -1, lv, pr, &n);
    for (int k = 0; k < n; k++) {
        if (!isfocused(lv[k])) continue;
        int r = jget(lv[k], "rect");
        int wide = jnget(r, "width") > jnget(r, "height");
        const char *pl = jsget(pr[k], "layout");
        if (pl && strcmp(pl, wide ? "splith" : "splitv") == 0) return;
        char line[96];
        snprintf(line, sizeof line, "[con_id=%.0f] split %s", jnget(lv[k], "id"), wide ? "h" : "v");
        cmd_add(line);
        return;
    }
}

// change: "new"/"close"/... from the window event, NULL at startup
static void tidy(int fd, const char *change, double con, int start) {
    char *tree = ipc_call(fd, 4, "");
    if (!tree) return;
    int root = jroot(tree), ws[MAXWS], nws = workspaces(root, ws);
    int top = 0;
    for (int i = 0; i < nws; i++) if (jnget(ws[i], "num") > top) top = (int)jnget(ws[i], "num");
    char line[1200];
    cmdlen = 0; cmdbuf[0] = 0;
    for (int i = 0; i < nws; i++) {
        int lv[MAXLEAF], n = 0;
        leaves(ws[i], lv, &n);
        for (int k = 0; k < n; k++) {
            int moveit = start ? k > 0
                       : !tiled && change && strcmp(change, "new") == 0 && n > 1 && jnget(lv[k], "id") == con;
            if (!moveit) continue;
            top++;
            int l = snprintf(line, sizeof line, "[con_id=%.0f] move container to workspace number %d",
                             jnget(lv[k], "id"), top);
            if (!start) snprintf(line + l, sizeof line - (size_t)l, "; workspace number %d", top);
            cmd_add(line);
        }
        int fl = jget(ws[i], "floating_nodes");
        for (int k = 0; tiled && k < n; k++) if (isfocused(lv[k])) { autotile(ws[i]); break; }
        if (change && strcmp(change, "close") == 0 && n == 0 && nws > 1 &&
            jget(ws[i], "focused") >= 0 && jv[jget(ws[i], "focused")].n &&
            (fl < 0 || jv[fl].kid < 0))
            cmd_add("workspace prev_on_output");
    }
    if (cmdlen) {
        free(ipc_call(fd, 0, cmdbuf));
        free(tree);
        if (!(tree = ipc_call(fd, 4, ""))) return;
        root = jroot(tree); nws = workspaces(root, ws);
    }
    cmdlen = 0; cmdbuf[0] = 0;
    for (int i = 0; i < nws; i++) {
        int lv[MAXLEAF], n = 0;
        leaves(ws[i], lv, &n);
        if (!n) continue;
        char lab[200], want[4096];
        int wl = snprintf(want, sizeof want, "%d:", (int)jnget(ws[i], "num"));
        if (tiled && n > 1) {
            int ord[MAXLEAF], pr[MAXLEAF], m = 0;
            inorder(ws[i], -1, ord, pr, &m);
            for (int k = 0; k < m && wl < (int)sizeof want - 400; k++) {
                char esc[600]; int e = 0;
                label(lab, sizeof lab, jsget(ord[k], "name"));
                for (const char *q = lab; *q && e < (int)sizeof esc - 6; q++)
                    e += *q == '&' ? sprintf(esc + e, "&amp;") : *q == '<' ? sprintf(esc + e, "&lt;")
                       : *q == '>' ? sprintf(esc + e, "&gt;") : (esc[e] = *q, 1);
                esc[e] = 0;
                wl += snprintf(want + wl, sizeof want - (size_t)wl, isfocused(ord[k])
                    ? "<span background='#89b4fa' foreground='#1e1e2e'> %s </span>"
                    : "<span background='#11111b' foreground='#a6adc8'> %s </span>", esc);
            }
        } else {
            label(lab, sizeof lab, jsget(lv[0], "name"));
            snprintf(want + wl, sizeof want - (size_t)wl, "%s", lab);
        }
        const char *have = jsget(ws[i], "name");
        if (have && strcmp(have, want) != 0) {
            char rn[8400];
            snprintf(rn, sizeof rn, "rename workspace \"%s\" to \"%s\"", have, want);
            cmd_add(rn);
        }
    }
    if (cmdlen) free(ipc_call(fd, 0, cmdbuf));
    free(tree);
}

// Tick "split": tab mode -> tiled gathers every window on the focused
// workspace one by one, so autotiling places each; tiled -> tab mode spreads
// them out like at startup.
static void split(int fd) {
    if (tiled) { tiled = 0; tidy(fd, NULL, 0, 1); return; }
    tiled = 1;
    double back = -1;
    for (;;) {
        char *tree = ipc_call(fd, 4, "");
        if (!tree) return;
        int ws[MAXWS], nws = workspaces(jroot(tree), ws), cur = curws(ws, nws);
        if (cur < 0) { free(tree); return; }
        int lv[MAXLEAF], pr[MAXLEAF], n = 0;
        inorder(ws[cur], -1, lv, pr, &n);
        for (int k = 0; k < n && back < 0; k++) if (isfocused(lv[k])) back = jnget(lv[k], "id");
        double next = -1;
        for (int i = 0; i < nws && next < 0; i++) {
            if (i == cur) continue;
            int ov[MAXLEAF], op[MAXLEAF], m = 0;
            inorder(ws[i], -1, ov, op, &m);
            if (m) next = jnget(ov[0], "id");
        }
        char line[256];
        cmdlen = 0; cmdbuf[0] = 0;
        if (next >= 0) {
            autotile(ws[cur]);
            snprintf(line, sizeof line, "[con_id=%.0f] move container to workspace number %d; [con_id=%.0f] focus",
                     next, (int)jnget(ws[cur], "num"), next);
        } else if (back >= 0) {
            snprintf(line, sizeof line, "[con_id=%.0f] focus", back);
        } else line[0] = 0;
        if (line[0]) cmd_add(line);
        if (cmdlen) free(ipc_call(fd, 0, cmdbuf));
        free(tree);
        if (next < 0) break;
    }
    tidy(fd, "focus", 0, 0);
}

// Tick "next"/"prev" (Alt+Tab, F3): next tab, in tiled mode next window.
static void cycle(int fd, int dir) {
    if (!tiled) {
        free(ipc_call(fd, 0, dir > 0 ? "workspace next_on_output" : "workspace prev_on_output"));
        return;
    }
    char *tree = ipc_call(fd, 4, "");
    if (!tree) return;
    int ws[MAXWS], nws = workspaces(jroot(tree), ws), cur = curws(ws, nws);
    int lv[MAXLEAF], pr[MAXLEAF], n = 0;
    if (cur >= 0) inorder(ws[cur], -1, lv, pr, &n);
    for (int k = 0; k < n; k++) {
        if (!isfocused(lv[k])) continue;
        char line[64];
        snprintf(line, sizeof line, "[con_id=%.0f] focus", jnget(lv[(k + dir + n) % n], "id"));
        free(ipc_call(fd, 0, line));
        break;
    }
    free(tree);
}

int main(int argc, char **argv) {
    unsigned long busy, idle, pbusy, pidle, rx, tx, prx, ptx;

    if (argc > 1 && strcmp(argv[1], "--loop") == 0) {
        // Interval in seconds, default 5. Less often means: swaybar redraws
        // less often, sway composites less often, the panel may idle longer.
        long secs = argc > 2 ? atol(argv[2]) : 5;
        if (secs < 1) secs = 1;
        const char *rt = getenv("XDG_RUNTIME_DIR");
        int ino = -1;
        if (rt && strlen(rt) < sizeof rtdir - 16) {
            memcpy(rtdir, rt, strlen(rt)); memcpy(rtdir + strlen(rt), "/claude-bar", 12);
            mkdir(rtdir, 0700);
            ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (ino >= 0) inotify_add_watch(ino, rtdir, IN_MOVED_TO | IN_CLOSE_WRITE);
        }
        // two connections: one subscribes to events, the other sends commands --
        // otherwise replies and events get mixed up in the same stream
        int cmd = ipc_open(), ev = cmd >= 0 ? ipc_open() : -1;
        if (ev >= 0) {
            free(ipc_call(ev, 2, "[\"window\",\"workspace\",\"tick\"]"));
            tidy(cmd, NULL, 0, 1);
        }
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        next.tv_sec += secs;
        cpu_totals(&pbusy, &pidle);
        net_totals(&prx, &ptx);
        vol_refresh();
        wifibars = wifi_read();
        for (long cpu = -1, rxr = -1, txr = -1;; ) {
            render(cpu, rxr, txr);
            if (emit(1)) return 1;                          // swaybar gone -> done
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (next.tv_sec - now.tv_sec) * 1000 + (next.tv_nsec - now.tv_nsec) / 1000000;
            if (ms > 0) {
                struct pollfd pf[2] = { { ev, POLLIN, 0 }, { ino, POLLIN, 0 } };
                if (poll(pf, 2, (int)ms) > 0) {
                    if (pf[1].revents) {
                        char b[4096];
                        while (read(ino, b, sizeof b) > 0) {}
                    }
                    if (pf[0].revents) {
                        uint32_t t;
                        char *m = ipc_recv(ev, &t);
                        if (!m) { close(ev); close(cmd); ev = cmd = -1; continue; }
                        int r = jroot(m);
                        const char *ch = jsget(r, "change");
                        double con = jnget(jget(r, "container"), "id");
                        char change[16] = "";
                        if (ch) snprintf(change, sizeof change, "%s", ch);
                        const char *tick = jsget(r, "payload");
                        int volkey = t == 0x80000007u && tick && !strcmp(tick, "volume");
                        int istick = t == 0x80000007u && tick;
                        int splitkey = istick && !strcmp(tick, "split");
                        int cyc = istick && !strcmp(tick, "next") ? 1 : istick && !strcmp(tick, "prev") ? -1 : 0;
                        free(m);
                        if (volkey) vol_refresh();
                        if (splitkey) split(cmd);
                        if (cyc) cycle(cmd, cyc);
                        if (t == 0x80000003u || (t == 0x80000000u &&
                            (!strcmp(change, "init") || !strcmp(change, "empty"))))
                            tidy(cmd, change, con, 0);
                    }
                    continue;                               // redraw, keep the tick schedule
                }
            }
            next.tv_sec += secs;
            if (vol < 0) vol_refresh();                     // at login WirePlumber may not be up yet
            wifibars = wifi_read();
            cpu_totals(&busy, &idle);
            cpu = cpu_pct(busy, idle, pbusy, pidle);
            pbusy = busy; pidle = idle;
            net_totals(&rx, &tx);
            rxr = rate(rx, prx, secs); txr = rate(tx, ptx, secs);
            prx = rx; ptx = tx;
        }
    }

    // One-shot mode: counters are kept in a cache file. The network rate also
    // needs the elapsed time -- CPU works in ticks and does without, bytes/s
    // does not.
    cpu_totals(&busy, &idle);
    net_totals(&rx, &tx);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    char cache[256]; const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) rt = "/tmp";
    int cl = (int)strlen(rt);
    if (cl > (int)sizeof cache - 14) cl = (int)sizeof cache - 14;
    memcpy(cache, rt, (size_t)cl);
    memcpy(cache + cl, "/.swaybar-cpu", 14);

    char b[128]; long cpu = -1, rxr = -1, txr = -1;
    if (slurp(cache, b, sizeof b)) {
        const char *p = b;
        pbusy = nextul(&p); pidle = nextul(&p);
        cpu = cpu_pct(busy, idle, pbusy, pidle);
        prx = nextul(&p); ptx = nextul(&p);
        long secs = now.tv_sec - (long)nextul(&p);
        if (secs > 0 && secs <= 300) {                      // stale reading -> rather show "--"
            rxr = rate(rx, prx, secs); txr = rate(tx, ptx, secs);
        }
    }
    int fd = open(cache, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        olen = 0;
        putnum((long)busy, 0); put(" "); putnum((long)idle, 0); put(" ");
        putnum((long)rx, 0);   put(" "); putnum((long)tx, 0);   put(" ");
        putnum((long)now.tv_sec, 0); put("\n");
        ssize_t ig = write(fd, out, (size_t)olen); (void)ig;
        close(fd);
    }
    vol_refresh();
    wifibars = wifi_read();
    render(cpu, rxr, txr);
    return emit(0);
}
