// Unit tests for the pure helpers in src/swaybar-status.c. The source is
// included directly so its static functions are reachable.
#define main swaybar_status_main
#include "../src/swaybar-status.c"
#undef main
#include "check.h"

static const char *rendered(void) { out[olen] = 0; return out; }

static void test_numbers(void) {
    CHECK_EQ(digits(0), 1);
    CHECK_EQ(digits(9), 1);
    CHECK_EQ(digits(10), 2);
    CHECK_EQ(digits(12345), 5);

    olen = 0; putnum(7, 2); put(":"); putnum(-42, 0); putnum(0, 0);
    CHECK_STR(rendered(), "07:-420");

    olen = 0; padnum(5, 3); putnum(5, 0);
    CHECK_STR(rendered(), "  5");

    const char *p = "cpu  12 34\n";
    CHECK_EQ(nextul(&p), 12);
    CHECK_EQ(nextul(&p), 34);
    CHECK_EQ(nextul(&p), 0);                    // end of string
}

static void test_rates(void) {
    CHECK_EQ(rate(100, 50, 0), -1);             // no elapsed time
    CHECK_EQ(rate(10, 50, 5), -1);              // counter reset
    CHECK_EQ(rate(1050, 50, 5), 200);

    olen = 0; putrate(-1);
    CHECK_STR(rendered(), "--.-");
    olen = 0; putrate(12499);                   // below 0.1 Mbit/s is noise
    CHECK_STR(rendered(), "0.0");
    olen = 0; putrate(12500);                   // 12500 B/s = 0.1 Mbit/s
    CHECK_STR(rendered(), "0.1");
    olen = 0; putrate(4150000);                 // 33.2 Mbit/s
    CHECK_STR(rendered(), "33.2");
    olen = 0; putrate(1000000000);              // capped
    CHECK_STR(rendered(), "99.9");

    CHECK_EQ(ratewidth(-1), 2);
    CHECK_EQ(ratewidth(0), 1);
    CHECK_EQ(ratewidth(4150000), 2);
}

static void test_cpu(void) {
    CHECK_EQ(cpu_pct(100, 100, 100, 100), -1);  // no ticks elapsed
    CHECK_EQ(cpu_pct(150, 150, 100, 100), 50);
    CHECK_EQ(cpu_pct(200, 100, 100, 100), 100);
    CHECK_EQ(cpu_pct(100, 200, 100, 100), 0);
    CHECK_EQ(cpu_pct(200, 50, 100, 100), 100);  // idle went backwards: clamp
}

static void test_mem(void) {
    CHECK_EQ(mem_pct("MemTotal:        8000000 kB\nMemFree:          200000 kB\n"
                     "MemAvailable:     800000 kB\n"), 90);
    CHECK_EQ(mem_pct("MemTotal:        8000000 kB\nMemFree:          200000 kB\n"), -1);
    CHECK_EQ(mem_pct("MemTotal: 0 kB\nMemAvailable: 0 kB\n"), -1);
    CHECK_EQ(mem_pct("MemTotal: 100 kB\nMemAvailable: 200 kB\n"), -1);
}

static void test_skip_if(void) {
    CHECK(skip_if("lo", 2));
    CHECK(skip_if("veth1a2b", 8));
    CHECK(skip_if("br-1234", 7));
    CHECK(skip_if("docker0", 7));
    CHECK(!skip_if("wlp3s0", 6));
    CHECK(!skip_if("eth0", 4));
    CHECK(!skip_if("l", 1));                    // shorter than every prefix
}

static void test_smooth_power(void) {
    histn = 0; histdis = -1;
    CHECK_EQ(smooth_power(0, 1), -1);           // no reading
    CHECK_EQ(smooth_power(10000000, 1), 10000000);
    CHECK_EQ(smooth_power(10000000, 1), 10000000);
    long a = smooth_power(20000000, 1);         // newest weighs more than half
    CHECK(a > 15000000 && a < 20000000);
    CHECK_EQ(smooth_power(5000000, 0), 5000000);  // charge state flipped: reset
    for (int i = 0; i < 3 * HIST; i++) smooth_power(7000000, 0);
    CHECK_EQ(histn, HIST);                      // history is bounded
    CHECK_EQ(smooth_power(7000000, 0), 7000000);
}

static void test_label(void) {
    char b[200];
    label(b, sizeof b, NULL);
    CHECK_STR(b, "?");
    label(b, sizeof b, "~/src/tuios-fedora: make test");
    CHECK_STR(b, "tuios-fedora: make test");
    label(b, sizeof b, "~/src");
    CHECK_STR(b, "~/src");
    label(b, sizeof b, "say \"hi\" \\ now");
    CHECK_STR(b, "say 'hi' / now");
    // a bare path keeps its end: MAXLEN code points, leading ellipsis
    label(b, sizeof b, "~/aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddd");
    CHECK_STR(b, "\xE2\x80\xA6" "/bbbbbbbbbb/cccccccccc/dddd");
    // "<path>: <cmd>" keeps its start: trailing ellipsis
    label(b, sizeof b, "~/x: 0123456789012345678901234567890123");
    CHECK_STR(b, "x: 012345678901234567890123" "\xE2\x80\xA6");
    // truncation counts code points, not bytes
    label(b, sizeof b, "~/äöüäöüäöüäöüäöüäöüäöüäöüäöüäöü");
    CHECK_STR(b, "\xE2\x80\xA6" "äöüäöüäöüäöüäöüäöüäöüäöüäöü");
}

static void test_json(void) {
    char doc[] = "{\"name\": \"a\\\"b\\u00e4\\ud83d\\ude00\", \"num\": 3, "
                 "\"ok\": true, \"no\": false, \"nil\": null, "
                 "\"list\": [1, 2.5, {\"k\": \"v\"}]}";
    int r = jroot(doc);
    CHECK(r >= 0);
    CHECK_STR(jsget(r, "name"), "a\"b\xC3\xA4\xF0\x9F\x98\x80");
    CHECK_EQ(jnget(r, "num"), 3);
    CHECK_EQ(jnget(r, "ok"), 1);
    CHECK_EQ(jnget(r, "no"), 0);
    CHECK(jget(r, "nil") >= 0 && jv[jget(r, "nil")].t == 'z');
    CHECK_EQ(jnget(r, "missing"), -1);
    CHECK(jsget(r, "num") == NULL);             // wrong type
    int l = jget(r, "list");
    CHECK(l >= 0 && jv[l].t == 'a');
    int e = jv[l].kid;
    CHECK(jv[e].n == 1);
    e = jv[e].next;
    CHECK(jv[e].n == 2.5);
    e = jv[e].next;
    CHECK_STR(jsget(e, "k"), "v");
    CHECK_EQ(jv[e].next, -1);

    char bad[] = "{\"a\": ";
    CHECK_EQ(jroot(bad), -1);
}

// A trimmed get_tree reply: one output with two workspaces, the second has
// two windows, of which id 12 was focused last and is focused now.
static void test_tree(void) {
    char tree[] =
        "{\"id\":1,\"type\":\"root\",\"nodes\":["
        " {\"id\":2,\"name\":\"__i3\",\"type\":\"output\",\"nodes\":["
        "  {\"id\":3,\"type\":\"workspace\",\"name\":\"__i3_scratch\",\"nodes\":[]}]},"
        " {\"id\":4,\"name\":\"eDP-1\",\"type\":\"output\",\"nodes\":["
        "  {\"id\":5,\"type\":\"workspace\",\"num\":1,\"name\":\"1:a\",\"focus\":[10],"
        "   \"nodes\":[{\"id\":10,\"pid\":100,\"name\":\"~/a\",\"nodes\":[]}]},"
        "  {\"id\":6,\"type\":\"workspace\",\"num\":2,\"name\":\"2\",\"focus\":[12,11],"
        "   \"nodes\":[{\"id\":11,\"pid\":101,\"name\":\"~/b\",\"nodes\":[]},"
        "              {\"id\":12,\"pid\":102,\"name\":\"~/c: vim\",\"focused\":true,"
        "               \"nodes\":[]}]}]}]}";
    int root = jroot(tree);
    CHECK(root >= 0);

    int ws[MAXWS];
    int n = workspaces(root, ws);
    CHECK_EQ(n, 2);                             // scratchpad skipped
    CHECK_STR(jsget(ws[0], "name"), "1:a");

    int lv[MAXLEAF], nl = 0;
    leaves(ws[1], lv, &nl);
    CHECK_EQ(nl, 2);
    CHECK_EQ(jnget(lv[0], "id"), 12);           // most recently focused first
    CHECK_EQ(jnget(lv[1], "id"), 11);
}

static void test_cmd_add(void) {
    cmdlen = 0; cmdbuf[0] = 0;
    cmd_add("workspace 1");
    cmd_add("workspace 2");
    CHECK_STR(cmdbuf, "workspace 1; workspace 2");
}

static void test_volume(void) {
    int m = -1;
    CHECK_EQ(vol_parse("Volume: 0.60\n", &m), 60);
    CHECK_EQ(m, 0);
    CHECK_EQ(vol_parse("Volume: 1.00 [MUTED]\n", &m), 100);
    CHECK_EQ(m, 1);
    CHECK_EQ(vol_parse("Volume: 0.05\n", &m), 5);
    CHECK_EQ(vol_parse("Translate ID error: no matching node\n", &m), -1);
    CHECK_EQ(vol_parse("", &m), -1);

    olen = 0; inspan = 0;
    vol = -1; put_volume();
    CHECK_STR(rendered(), "");
    volmute = 0;
#define ICON(colour, size, glyph) \
    "<span foreground='" colour "'><span font_size='" size "'>" glyph "</span></span>"
    static const struct { long v; const char *want; } levels[] = {
        { 0,   ICON("#6c7086", "14pt", "\U000F0581") },     // silent, same as muted
        { 1,   ICON("#bac2de", "14pt", "\U000F057F") },
        { 10,  ICON("#bac2de", "14pt", "\U000F057F") },
        { 20,  ICON("#bac2de", "14pt", "\U000F0580") },
        { 40,  ICON("#bac2de", "14pt", "\U000F0580") },
        { 50,  ICON("#bac2de", "14pt", "\U000F057E") },
        { 70,  ICON("#bac2de", "14pt", "\U000F057E") },
        { 80,  ICON("#bac2de", "14pt", "\uF028") },
        { 100, ICON("#bac2de", "14pt", "\uF028") },
        { 150, ICON("#bac2de", "14pt", "\uF028") },
    };
    for (size_t i = 0; i < sizeof levels / sizeof *levels; i++) {
        olen = 0; vol = levels[i].v; put_volume();
        CHECK_STR(rendered(), levels[i].want);
    }
    olen = 0; vol = 60; volmute = 1; put_volume();
    CHECK_STR(rendered(), ICON("#6c7086", "14pt", "\U000F0581"));
    olen = 0; vol = 90; put_volume();                       // muted beats loud
    CHECK_STR(rendered(), ICON("#6c7086", "14pt", "\U000F0581"));
    vol = -1; volmute = 0;
}

static int attr(char *b, int off, int type, const void *d, int len) {
    struct nlattr a = { .nla_len = (uint16_t)(NLA_HDRLEN + len), .nla_type = (uint16_t)type };
    memcpy(b + off, &a, NLA_HDRLEN);
    memcpy(b + off + NLA_HDRLEN, d, (size_t)len);
    return off + NLA_ALIGN(NLA_HDRLEN + len);
}

// One GET_SCAN message holding a single BSS.
static int scan_msg(char *b, uint32_t status, int32_t mbm) {
    char nest[32];
    int nl = attr(nest, 0, NL80211_BSS_STATUS, &status, 4);
    nl = attr(nest, nl, NL80211_BSS_SIGNAL_MBM, &mbm, 4);
    int off = attr(b, NLMSG_HDRLEN + GENL_HDRLEN, NL80211_ATTR_BSS, nest, nl);
    struct nlmsghdr h = { .nlmsg_len = (uint32_t)off, .nlmsg_type = 30 };
    memcpy(b, &h, sizeof h);
    return off;
}

static void test_wifi(void) {
    CHECK_EQ(wifi_bars(-5000), 4);
    CHECK_EQ(wifi_bars(-5500), 4);
    CHECK_EQ(wifi_bars(-5501), 3);
    CHECK_EQ(wifi_bars(-6700), 3);
    CHECK_EQ(wifi_bars(-7500), 2);
    CHECK_EQ(wifi_bars(-9000), 1);

    uint32_t buf[32];
    char *b = (char *)buf;
    int bars = 0, n = scan_msg(b, NL80211_BSS_STATUS_ASSOCIATED, -5900);
    CHECK_EQ(wifi_msgs(b, n, &bars), 0);
    CHECK_EQ(bars, 3);

    bars = 0; n = scan_msg(b, NL80211_BSS_STATUS_AUTHENTICATED, -5900);
    CHECK_EQ(wifi_msgs(b, n, &bars), 0);        // a neighbour's BSS is not a connection
    CHECK_EQ(bars, 0);

    struct nlmsghdr h = { .nlmsg_len = NLMSG_HDRLEN, .nlmsg_type = NLMSG_DONE };
    memcpy(b, &h, sizeof h);
    CHECK_EQ(wifi_msgs(b, NLMSG_HDRLEN, &bars), 1);
    h.nlmsg_type = NLMSG_ERROR;
    memcpy(b, &h, sizeof h);
    CHECK_EQ(wifi_msgs(b, NLMSG_HDRLEN, &bars), -1);
    h.nlmsg_len = 4096;                         // longer than what arrived
    memcpy(b, &h, sizeof h);
    CHECK_EQ(wifi_msgs(b, NLMSG_HDRLEN, &bars), -1);

    olen = 0; inspan = 0; wifibars = 3; put_wifi();
    CHECK_STR(rendered(), ICON("#94e2d5", "14pt", "\U000F0925") " ");
    olen = 0; wifibars = 0; put_wifi();
    CHECK_STR(rendered(), ICON("#f38ba8", "14pt", "\U000F092E") " ");
}

int main(void) {
    test_numbers();
    test_rates();
    test_cpu();
    test_mem();
    test_skip_if();
    test_smooth_power();
    test_label();
    test_json();
    test_tree();
    test_cmd_add();
    test_volume();
    test_wifi();
    return report("swaybar-status");
}
