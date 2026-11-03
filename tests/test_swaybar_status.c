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

    int f = focused_node(root);
    CHECK(f >= 0);
    CHECK_STR(jsget(f, "name"), "~/c: vim");
}

static void test_cmd_add(void) {
    cmdlen = 0; cmdbuf[0] = 0;
    cmd_add("workspace 1");
    cmd_add("workspace 2");
    CHECK_STR(cmdbuf, "workspace 1; workspace 2");
}

int main(void) {
    test_numbers();
    test_rates();
    test_cpu();
    test_skip_if();
    test_smooth_power();
    test_label();
    test_json();
    test_tree();
    test_cmd_add();
    return report("swaybar-status");
}
