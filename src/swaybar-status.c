
// swaybar-status: die eine Leiste des Kiosks -- links die Tabs, rechts
// Claude (Kontext des aktiven Tabs, 5-h-Limit), CPU, Netz, Akku, Uhr.
// Ausgabe ist Pango-Markup (bar { pango_markup enabled }). Hiess bis
// 2026-11-01 tmux-status.
// build: make && make install   (siehe README)
//
// Tabs: jedes Fenster bekommt einen eigenen Workspace "N:Titel", die
// Workspace-Buttons der Bar (strip_workspace_numbers) sind damit die Tabs.
// Im --loop-Modus haengt das Programm darum zusaetzlich am sway-IPC: neues
// Fenster auf belegtem Workspace -> eigener Workspace dahinter; Titel ->
// Workspace-Name; letztes Fenster zu -> voriger Tab. sways eigene Tableiste
// taugt nicht, sie ist eine zweite Leiste ohne Platz fuer den Status.
//
// Claude: `statusline --bar` (Repo claude-statusline) schreibt pro Session
// $XDG_RUNTIME_DIR/claude-bar/<Pfad, / als %> und dort "limits"; der
// Fenstertitel ist derselbe Pfad. inotify auf das Verzeichnis zeigt
// Aenderungen sofort statt erst zum naechsten Tick.
//
// Zwei Betriebsarten:
//   ohne Argument : einmal ausgeben, CPU-Delta ueber eine Cache-Datei (zum Testen)
//   --loop [sek]  : dauerhaft laufen und alle [sek] Sekunden eine Zeile schreiben
//
// --loop ist der Normalfall: swaybar startet status_command einmal und liest
// laufend Zeilen mit -- damit entfaellt fork/exec pro Tick, das mit ~1 ms der
// mit Abstand groesste Posten war. Der CPU-Zaehlerstand
// bleibt dann einfach im Speicher, die Cache-Datei braucht es nicht mehr.
// Beendet sich von selbst per SIGPIPE, sobald swaybar die Leseseite schliesst.
//
// Bewusst ohne stdio-Streams: printf/fopen ziehen Puffer-Allokation und
// stdio-Init nach sich (snprintf fuer die sway-Befehle ist harmlos). Alles laeuft ueber open/read/write und einen eigenen Zahlenformatierer.
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>                  // nur snprintf, kein FILE-Puffer
#include <string.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef BATDIR                      // ueberschreibbar fuer Tests
#define BATDIR "/sys/class/power_supply/BAT0/"
#endif

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

// Feste Feldbreite, damit die Leiste nicht springt. Aufgefuellt wird VOR dem
// Symbol: die Zahl bleibt am Symbol kleben, die rechte Kante jedes Feldes und
// damit die Gesamtlaenge stehen still.
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

// naechste vorzeichenlose Zahl ab *p, Zeiger wird weitergeschoben
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
    const char *p = b + 3;                                  // "cpu" ueberspringen
    unsigned long u = nextul(&p), ni = nextul(&p), sy = nextul(&p);
    unsigned long id = nextul(&p), io = nextul(&p);
    *idle = id + io;                                        // iowait zaehlt als nicht-arbeitend
    *busy = u + ni + sy;
}

// Virtuelle Interfaces zaehlen den Verkehr der echten NIC ein zweites Mal
// mit -- Bruecken, veth-Paare und Loopback fliegen darum raus.
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

// Bytes/s aus zwei Zaehlerstaenden, -1 wenn noch kein Delta vorliegt
static long rate(unsigned long now, unsigned long prev, long secs) {
    if (secs <= 0 || now < prev) return -1;             // Reset/Overflow
    return (long)((now - prev) / (unsigned long)secs);
}

// Rate in Mbit/s mit einer Nachkommastelle, ohne Einheit: "0.0", "33.2".
// Die Einheit steht nirgends -- in der eigenen Leiste ist sie bekannt.
// Alles unter 0,1 Mbit ist Hintergrundrauschen und wird zu 0.0 --
// feiner aufzuloesen bringt nichts. Gedeckelt bei 99.9, damit das Feld
// hoechstens zweistellig wird (die Leitung schafft ohnehin ~50).
// Mbit ist dezimal (1e6 bit), wie bei Leitungsangaben ueblich:
// Bytes/s * 8 / 1e6, in Zehnteln also Bytes/s / 12500.
static long tenths(long bps) {
    long t = bps / 12500;
    return t > 999 ? 999 : t;
}

// Breite des Ganzzahlteils, damit der Aufrufer vor dem Pfeil auffuellen kann
static int ratewidth(long bps) {
    return bps < 0 ? 2 : digits(tenths(bps) / 10);
}

static void putrate(long bps) {
    if (bps < 0) { put("--.-"); return; }
    long t = tenths(bps);
    putnum(t / 10, 0); put("."); putnum(t % 10, 0);
}

// Prozent aus zwei Zaehlerstaenden, -1 wenn kein brauchbares Delta vorliegt
static long cpu_pct(unsigned long busy, unsigned long idle,
                    unsigned long pbusy, unsigned long pidle) {
    long dt = (long)((busy + idle) - (pbusy + pidle));
    long di = (long)(idle - pidle);
    if (dt <= 0) return -1;
    if (di < 0) di = 0;
    if (di > dt) di = dt;                                   // gegen Zaehler-Resets
    return 100 - di * 100 / dt;
}

// Restzeit aus einem gewichteten Mittel der letzten 12 Leistungswerte
// (12 x 5 s = eine Minute). Neuere Samples zaehlen mehr (Gewicht rank^1.2),
// so folgt die Anzeige Lastwechseln, ohne bei jedem Tick zu springen.
// Gleicher Algorithmus wie im powermeter-Plasmoid. Beim Wechsel Laden/
// Entladen wird die Historie verworfen. Gewichte vorberechnet (x1000),
// damit kein pow() und keine libm noetig ist.
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
    for (int i = 0; i < histn; i++) {              // i=0 aeltestes ... neuestes am schwersten
        num += hist[i] * weight[i]; den += weight[i];
    }
    return num / den;
}

// Mit _OSI(Darwin) bindet Linux den SBS-Treiber statt der generischen
// ACPI-Batterie. Der liefert charge_*/current_* in uAh/uA statt
// energy_*/power_* in uWh/uW. Fehlt die Energie-Variante, wird sie ueber
// voltage_now umgerechnet, damit die Leiste in beiden Faellen Watt zeigt.
// (Die Restzeit wird dadurch minimal ungenau, weil voltage_now mit dem
// Ladestand faellt -- fuer eine auf 5 min gerundete Anzeige belanglos.)
static long as_uW(const char *energy, const char *charge) {
    long v = num(energy);
    if (v >= 0) return v;
    long c = num(charge), u = num(BATDIR "voltage_now");
    if (c < 0 || u < 0) return -1;
    return c * (u / 1000) / 1000;
}

// Farbwechsel als Pango-Span; NULL = zurueck zur Vordergrundfarbe der Leiste.
static int inspan;
static void col(const char *span) {
    if (inspan) put("</span>");
    inspan = span != NULL;
    if (span) put(span);
}
#define FG(c) "<span foreground='" c "'>"

// ---------------------------------------------------------------- Claude

static char rtdir[256];             // $XDG_RUNTIME_DIR/claude-bar
static char focus_key[512];         // Titel des aktiven Fensters

static void put_file(const char *name, long maxage) {
    char path[1024], b[600];
    int n = (int)strlen(rtdir);
    if (!n || n + 2 + (int)strlen(name) >= (int)sizeof path) return;
    memcpy(path, rtdir, (size_t)n); path[n++] = '/';
    for (const char *k = name; *k; k++) path[n++] = *k == '/' ? '%' : *k;
    path[n] = 0;
    struct stat st;
    if (stat(path, &st) || (maxage && time(NULL) - st.st_mtime > maxage)) return;
    int l = slurp(path, b, sizeof b);
    while (l && (b[l - 1] == '\n' || b[l - 1] == ' ')) b[--l] = 0;
    if (l) { put(b); put("  "); }
}

static void render(long cpu, long rxr, long txr) {
    olen = 0; inspan = 0;
    if (focus_key[0]) put_file(focus_key, 0);
    put_file("limits", 5 * 3600);           // aelter als ein 5-h-Fenster: ungueltig
    pad(cpu < 0 ? 1 : 3 - digits(cpu));
    col(cpu >= 70 ? FG("#f38ba8") : FG("#89b4fa"));
    put("");
    if (cpu < 0) put("--"); else putnum(cpu, 0);
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
        // energy_full aendert sich praktisch nie -> nur einmal lesen
        static long ef = -1;
        if (ef < 0) ef = as_uW(BATDIR "energy_full", BATDIR "charge_full");
        long rest = dis ? en : ef - en;
        long avg = smooth_power(pw, dis);

        padnum(cap, 3);
        int crit = dis && cap < 15;             // roter Hintergrund bis Zeilenende
        col(crit ? "<span foreground='#1e1e2e' background='#f38ba8'>"
                 : dis && cap <= 30 ? FG("#fab387") : FG("#a6e3a1"));
        put(dis ? "󰁹" : "󰂄");
        putnum(cap, 0); put("%");
        // Restzeit auf 5 min gerundet, damit die Anzeige nicht bei jedem
        // Tick zuckt. Fehlt ein Wert, entfaellt das Feld ganz -- die Leiste
        // ist ohnehin nicht mehr auf feste Spalten ausgelegt.
        if (avg > 0 && rest > 0) {
            long m = (rest * 60 / avg + 2) / 5 * 5;
            put(" "); putnum(m / 60, 0); put(":"); putnum(m % 60, 2); put("h");
        } else {
            pad(6);                                 // Feld bleibt reserviert
        }
        // Leistung " NNW" = 4 Zeichen, gerundet statt abgeschnitten
        if (pw > 0) {
            long w = (pw + 500000) / 1000000;
            put(" "); padnum(w, 2); putnum(w, 0); put("W");
        } else {
            pad(4);
        }
        col(NULL); put(" ");
    }
    time_t t = time(NULL); struct tm tm;
    localtime_r(&t, &tm);
    put(" "); putnum(tm.tm_hour, 2);
    put(":"); putnum(tm.tm_min, 2); put(" ");
}

// Nur schreiben, wenn sich die Zeile geaendert hat -- bleibt alles gleich,
// zeichnet swaybar nicht und sway komponiert nicht.
static char prev[sizeof out];
static int prevlen = -1;

static int emit(int nl) {
    if (nl) put("\n");
    if (olen == prevlen && memcmp(out, prev, (size_t)olen) == 0) return 0;
    memcpy(prev, out, (size_t)olen); prevlen = olen;
    return write(1, out, (size_t)olen) == olen ? 0 : 1;
}

// ---------------------------------------------------------------- JSON
// Minimaler Parser fuer die sway-Antworten: Werte landen in einem flachen
// Array, Kinder sind ueber kid/next verkettet, Strings werden im Puffer selbst
// entschluesselt (kuerzer wird es dabei nie).

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
#define MAXLEN 28                   // Tab-Beschriftung, laengere vorne gekuerzt

// Tiling-Fenster eines Teilbaums, zuletzt fokussiertes zuerst
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

static int focused_node(int node) {
    int c = jget(node, "focused");
    if (c >= 0 && jv[c].n) return node;
    const char *lists[2] = { "nodes", "floating_nodes" };
    for (int l = 0; l < 2; l++) {
        int a = jget(node, lists[l]);
        for (int k = a >= 0 ? jv[a].kid : -1; k >= 0; k = jv[k].next) {
            int f = focused_node(k);
            if (f >= 0) return f;
        }
    }
    return -1;
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

// " und \ kommen nicht in Namen: sways Parser laesst \" stehen statt es
// aufzuloesen. Gekuerzt wird vorne -- bei Pfaden zaehlt das Ende.
static void label(char *dst, size_t cap, const char *t) {
    if (!t) t = "?";
    size_t cps = 0;
    for (const char *p = t; *p; p++) if ((*p & 0xC0) != 0x80) cps++;
    const char *p = t;
    size_t o = 0;
    if (cps > MAXLEN) {
        size_t skip = cps - (MAXLEN - 1);
        while (skip) { p++; while ((*p & 0xC0) == 0x80) p++; skip--; }
        memcpy(dst, "\xE2\x80\xA6", 3); o = 3;           // …
    }
    for (; *p && o + 1 < cap; p++)
        dst[o++] = *p == '"' ? '\'' : *p == '\\' ? '/' : *p == '\n' ? ' ' : *p;
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

// change: "new"/"close"/... aus dem Fenster-Event, NULL beim Start
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
                       : change && strcmp(change, "new") == 0 && n > 1 && jnget(lv[k], "id") == con;
            if (!moveit) continue;
            top++;
            int l = snprintf(line, sizeof line, "[con_id=%.0f] move container to workspace number %d",
                             jnget(lv[k], "id"), top);
            if (!start) snprintf(line + l, sizeof line - (size_t)l, "; workspace number %d", top);
            cmd_add(line);
        }
        int fl = jget(ws[i], "floating_nodes");
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
        char lab[200], want[256];
        label(lab, sizeof lab, jsget(lv[0], "name"));
        snprintf(want, sizeof want, "%d:%s", (int)jnget(ws[i], "num"), lab);
        const char *have = jsget(ws[i], "name");
        if (have && strcmp(have, want) != 0) {
            snprintf(line, sizeof line, "rename workspace \"%s\" to \"%s\"", have, want);
            cmd_add(line);
        }
    }
    int f = focused_node(root);
    const char *ft = f >= 0 && jget(f, "pid") >= 0 ? jsget(f, "name") : NULL;
    snprintf(focus_key, sizeof focus_key, "%s", ft ? ft : "");
    if (cmdlen) free(ipc_call(fd, 0, cmdbuf));
    free(tree);
}

int main(int argc, char **argv) {
    unsigned long busy, idle, pbusy, pidle, rx, tx, prx, ptx;

    if (argc > 1 && strcmp(argv[1], "--loop") == 0) {
        // Intervall in Sekunden, Default 5. Seltener heisst: swaybar zeichnet
        // seltener, sway komponiert seltener, das Panel darf laenger stehen.
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
        // zwei Verbindungen: eine abonniert Events, die andere schickt Befehle --
        // sonst mischen sich Antworten und Events im selben Strom
        int cmd = ipc_open(), ev = cmd >= 0 ? ipc_open() : -1;
        if (ev >= 0) {
            free(ipc_call(ev, 2, "[\"window\",\"workspace\"]"));
            tidy(cmd, NULL, 0, 1);
        }
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        next.tv_sec += secs;
        cpu_totals(&pbusy, &pidle);
        net_totals(&prx, &ptx);
        for (long cpu = -1, rxr = -1, txr = -1;; ) {
            render(cpu, rxr, txr);
            if (emit(1)) return 1;                          // swaybar weg -> Schluss
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
                        free(m);
                        if (t == 0x80000003u || (t == 0x80000000u &&
                            (!strcmp(change, "init") || !strcmp(change, "empty"))))
                            tidy(cmd, change, con, 0);
                    }
                    continue;                               // neu zeichnen, Tick bleibt
                }
            }
            next.tv_sec += secs;
            cpu_totals(&busy, &idle);
            cpu = cpu_pct(busy, idle, pbusy, pidle);
            pbusy = busy; pidle = idle;
            net_totals(&rx, &tx);
            rxr = rate(rx, prx, secs); txr = rate(tx, ptx, secs);
            prx = rx; ptx = tx;
        }
    }

    // Einmal-Modus: Zaehlerstaende liegen in einer Cache-Datei. Fuer die
    // Netzrate braucht es zusaetzlich die verstrichene Zeit -- CPU rechnet
    // mit Ticks und kommt ohne aus, Bytes/s nicht.
    cpu_totals(&busy, &idle);
    net_totals(&rx, &tx);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    char cache[256]; const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) rt = "/tmp";
    int cl = (int)strlen(rt);
    if (cl > (int)sizeof cache - 12) cl = (int)sizeof cache - 12;
    memcpy(cache, rt, (size_t)cl);
    memcpy(cache + cl, "/.tmux-cpu", 11);

    char b[128]; long cpu = -1, rxr = -1, txr = -1;
    if (slurp(cache, b, sizeof b)) {
        const char *p = b;
        pbusy = nextul(&p); pidle = nextul(&p);
        cpu = cpu_pct(busy, idle, pbusy, pidle);
        prx = nextul(&p); ptx = nextul(&p);
        long secs = now.tv_sec - (long)nextul(&p);
        if (secs > 0 && secs <= 300) {                      // alter Stand -> lieber "--"
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
    render(cpu, rxr, txr);
    return emit(0);
}
