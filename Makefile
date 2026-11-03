CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= $(HOME)/.local

all: swaybar-status specialkeysd

swaybar-status: src/swaybar-status.c
	$(CC) $(CFLAGS) -o $@ $<

specialkeysd: src/specialkeysd.c
	$(CC) $(CFLAGS) -o $@ $<

TESTS = test_swaybar_status test_specialkeysd

test_swaybar_status: tests/test_swaybar_status.c src/swaybar-status.c tests/check.h
	$(CC) $(CFLAGS) -o $@ $<

test_specialkeysd: tests/test_specialkeysd.c src/specialkeysd.c tests/check.h
	$(CC) $(CFLAGS) -o $@ $<

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

# swaybar does not restart status_command by itself -- after copying, have it
# restarted via "swaymsg reload".
install: swaybar-status
	install -Dm755 swaybar-status $(PREFIX)/bin/swaybar-status
	-swaymsg reload

# Needs root: the service reads /dev/input and writes /sys/class/backlight,
# both root-only. Hence separate from "install".
install-daemon: specialkeysd
	sudo install -Dm755 specialkeysd /usr/local/sbin/specialkeysd
	sudo install -Dm644 system/etc/systemd/system/specialkeysd.service \
		/etc/systemd/system/specialkeysd.service
	sudo systemctl daemon-reload
	sudo systemctl enable --now specialkeysd

clean:
	rm -f swaybar-status specialkeysd $(TESTS)

.PHONY: all test install install-daemon clean
