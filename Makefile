CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= $(HOME)/.local

all: swaybar-status specialkeysd

swaybar-status: src/swaybar-status.c
	$(CC) $(CFLAGS) -o $@ $<

specialkeysd: src/specialkeysd.c
	$(CC) $(CFLAGS) -o $@ $<

# swaybar startet status_command nicht von selbst neu -- nach dem Kopieren
# per "swaymsg reload" neu starten lassen.
install: swaybar-status
	install -Dm755 swaybar-status $(PREFIX)/bin/swaybar-status
	-swaymsg reload

# Braucht root: der Dienst liest /dev/input und schreibt /sys/class/backlight,
# beides darf nur root. Darum getrennt von "install".
install-daemon: specialkeysd
	sudo install -Dm755 specialkeysd /usr/local/sbin/specialkeysd
	sudo install -Dm644 system/etc/systemd/system/specialkeysd.service \
		/etc/systemd/system/specialkeysd.service
	sudo systemctl daemon-reload
	sudo systemctl enable --now specialkeysd

clean:
	rm -f swaybar-status specialkeysd

.PHONY: all install install-daemon clean
