// Minimal test harness: no framework, just counted assertions.
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { checks++; const char *g_ = (got), *w_ = (want); \
    if (!g_ || strcmp(g_, w_)) { fails++; fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", \
    __FILE__, __LINE__, g_ ? g_ : "(null)", w_); } } while (0)
#define CHECK_EQ(got, want) do { checks++; long g_ = (long)(got), w_ = (long)(want); \
    if (g_ != w_) { fails++; fprintf(stderr, "%s:%d: %s = %ld, want %ld\n", \
    __FILE__, __LINE__, #got, g_, w_); } } while (0)

static int report(const char *name) {
    fprintf(stderr, "%s: %d checks, %d failed\n", name, checks, fails);
    return fails != 0;
}
