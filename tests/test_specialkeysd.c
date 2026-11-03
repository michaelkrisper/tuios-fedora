// Unit tests for the brightness stepping in src/specialkeysd.c, against a
// plain file standing in for the sysfs brightness attribute.
#define main specialkeysd_main
#include "../src/specialkeysd.c"
#undef main
#include "check.h"

static const char *FILE_PATH = "test-brightness.tmp";

static long press(struct dimmer *d, int dir) {
    d->laststep = nowms() - 10 * STEPMS;        // outside the debounce window
    step(d, dir);
    return rd(d->path);
}

static void test_raw_of(void) {
    struct dimmer d = { FILE_PATH, disp_steps, 0, 1388, 0 };
    CHECK_EQ(raw_of(&d, 0), 0);
    CHECK_EQ(raw_of(&d, 2), 3);                 // 2.776 rounds up
    CHECK_EQ(raw_of(&d, 500), 694);
    CHECK_EQ(raw_of(&d, 1000), 1388);
}

static void test_step_display(void) {
    struct dimmer d = { FILE_PATH, disp_steps,
                        (int)(sizeof disp_steps / sizeof *disp_steps), 1388, 0 };
    wr(FILE_PATH, 0);
    CHECK_EQ(press(&d, 1), 3);
    CHECK_EQ(press(&d, 1), 7);
    CHECK_EQ(press(&d, -1), 3);
    CHECK_EQ(press(&d, -1), 0);
    CHECK_EQ(press(&d, -1), 0);                 // stays at the bottom

    wr(FILE_PATH, 1000);                        // between steps 700 and 800
    CHECK_EQ(press(&d, 1), 1110);
    CHECK_EQ(press(&d, 1), 1249);
    CHECK_EQ(press(&d, 1), 1388);
    CHECK_EQ(press(&d, 1), 1388);               // stays at the top

    wr(FILE_PATH, 1000);
    CHECK_EQ(press(&d, -1), 972);

    // debounce: a second event within STEPMS is ignored
    wr(FILE_PATH, 0);
    d.laststep = nowms() - 10 * STEPMS;
    step(&d, 1);
    step(&d, 1);
    CHECK_EQ(rd(FILE_PATH), 3);
}

static void test_step_small_range(void) {
    // keyboard backlight with few raw levels: distinct steps that round to
    // the same raw value must not swallow a key press
    struct dimmer d = { FILE_PATH, kbd_steps,
                        (int)(sizeof kbd_steps / sizeof *kbd_steps), 4, 0 };
    wr(FILE_PATH, 0);
    CHECK_EQ(press(&d, 1), 2);                  // 10 and 100 per mille both round to 0
    CHECK_EQ(press(&d, 1), 4);
    CHECK_EQ(press(&d, -1), 2);
    CHECK_EQ(press(&d, -1), 0);
}

static void test_missing_device(void) {
    struct dimmer d = { FILE_PATH, kbd_steps, 5, 0, 0 };
    wr(FILE_PATH, 7);
    CHECK_EQ(press(&d, 1), 7);                  // max <= 0: untouched
    CHECK_EQ(rd("test-does-not-exist.tmp"), -1);
}

int main(void) {
    int fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(FILE_PATH); return 1; }
    close(fd);
    test_raw_of();
    test_step_display();
    test_step_small_range();
    test_missing_device();
    unlink(FILE_PATH);
    return report("specialkeysd");
}
