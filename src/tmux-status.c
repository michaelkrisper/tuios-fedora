
// tmux-Statuszeile: Akku + CPU-Last + Netzdurchsatz.
// build: make && make install   (siehe README)
//
// Zwei Betriebsarten:
//   ohne Argument : einmal ausgeben, CPU-Delta ueber eine Cache-Datei (zum Testen)
//   --loop [sek]  : dauerhaft laufen und alle [sek] Sekunden eine Zeile schreiben
//
// --loop ist der Normalfall. tmux startet einen #()-Job genau einmal und liest
// laufend Zeilen mit, solange der Prozess lebt -- damit entfaellt fork/exec pro
// Tick, das mit ~1 ms der mit Abstand groesste Posten war. Der CPU-Zaehlerstand
// bleibt dann einfach im Speicher, die Cache-Datei braucht es nicht mehr.
// Beendet sich von selbst per SIGPIPE, sobald tmux die Leseseite schliesst.
//
// Bewusst ohne stdio: printf/fopen ziehen Puffer-Allokation und stdio-Init nach
// sich. Alles laeuft ueber open/read/write und einen eigenen Zahlenformatierer.
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifndef BATDIR                      // ueberschreibbar fuer Tests
#define BATDIR "/sys/class/power_supply/BAT0/"
#endif

static char out[512];
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

// Rate als Mbit/s mit einer Nachkommastelle: "0.0Mbit", "33.2Mbit".
// Alles unter 0,1 Mbit ist Hintergrundrauschen und wird zu 0.0 --
// feiner aufzuloesen bringt nichts.
// Mbit ist dezimal (1e6 bit), wie bei Leitungsangaben ueblich:
// Bytes/s * 8 / 1e6, in Zehnteln also Bytes/s / 12500.
// Breite des Ganzzahlteils, damit der Aufrufer vor dem Pfeil auffuellen kann
static int ratewidth(long bps) {
    return bps < 0 ? 2 : digits(bps / 125000);
}

static void putrate(long bps) {
    if (bps < 0) { put("--.-Mbit"); return; }
    long tenths = bps / 12500;
    putnum(tenths / 10, 0); put("."); putnum(tenths % 10, 0); put("Mbit");
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

static void render(long cpu, long rxr, long txr) {
    olen = 0;
    pad(cpu < 0 ? 1 : 3 - digits(cpu));
    put(cpu >= 70 ? "#[fg=#f38ba8]" : "#[fg=#89b4fa]");
    put("");
    if (cpu < 0) put("--"); else putnum(cpu, 0);
    put("%#[fg=#cdd6f4] ");
    pad(3 - ratewidth(rxr));
    put("#[fg=#94e2d5]↓"); putrate(rxr);
    put(" "); pad(3 - ratewidth(txr)); put("↑"); putrate(txr);
    put("#[fg=#cdd6f4] ");
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
        put(crit ? "#[fg=#1e1e2e,bg=#f38ba8]" : dis && cap <= 30 ? "#[fg=#fab387]" : "#[fg=#a6e3a1]");
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
        put("#[fg=#cdd6f4,bg=default] ");
    }
}

// Nur schreiben, wenn sich die Zeile geaendert hat. tmux behaelt die letzte
// Zeile stehen und schreibt ohnehin nur geaenderte Zellen ins Terminal --
// bleibt alles gleich, zeichnet foot nicht und cage komponiert nicht.
static char prev[sizeof out];
static int prevlen = -1;

static int emit(int nl) {
    if (nl) put("\n");
    if (olen == prevlen && memcmp(out, prev, (size_t)olen) == 0) return 0;
    memcpy(prev, out, (size_t)olen); prevlen = olen;
    return write(1, out, (size_t)olen) == olen ? 0 : 1;
}

int main(int argc, char **argv) {
    unsigned long busy, idle, pbusy, pidle, rx, tx, prx, ptx;

    if (argc > 1 && strcmp(argv[1], "--loop") == 0) {
        // Intervall in Sekunden, Default 5. Seltener heisst: foot zeichnet
        // seltener, cage komponiert seltener, das Panel darf laenger stehen.
        long secs = argc > 2 ? atol(argv[2]) : 5;
        if (secs < 1) secs = 1;
        struct timespec ts = { secs, 0 };
        cpu_totals(&pbusy, &pidle);
        net_totals(&prx, &ptx);
        for (long cpu = -1, rxr = -1, txr = -1;; ) {
            render(cpu, rxr, txr);
            if (emit(1)) return 1;                          // tmux weg -> Schluss
            nanosleep(&ts, NULL);
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
