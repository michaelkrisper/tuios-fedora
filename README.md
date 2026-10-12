# tuios-fedora

A MacBook Pro 11,1 running Fedora that boots straight into a terminal — no
desktop, no compositor chrome, no session manager. sway holds a single
fullscreen `foot` window, tmux arranges the terminals inside it, and two small
C programs fill the gaps the desktop used to cover: a status line and a daemon
for the laptop's special keys.

The point is what it does *not* run. Plasma cost roughly 5.7 % of a core
continuously for kwin and plasmashell alone; dropping it took idle draw down
noticeably on a battery this old. What is left boots in about 15 seconds into
974 MiB of used RAM.

```
 53%  ↓0.0 ↑12.4 󰁹78% 1:35h 21.9W  12:41
```

## What is in here

| | |
|---|---|
| `tmux-status.c` | status line: CPU load, network throughput, battery — dependency-free C |
| `specialkeysd.c` | brightness, keyboard backlight, idle dimming, and the Exposé/Dashboard keys |
| `tmux-cycle-view` | cycles the terminals between side-by-side, tiled and one-per-tab |
| `tmux.conf` | the tmux setup those three plug into |
| `sway.config` | one fullscreen terminal, touchpad, and deliberately *no* key bindings |
| `specialkeysd.service` | runs the daemon as root, because `/dev/input` and backlight need it |

`SETUP.md` documents how to reproduce the whole thing on a fresh Fedora.

## Build

```sh
make && make install          # -> ~/.local/bin
make install-daemon           # -> /usr/local/sbin, needs sudo
```

`make install` also kills any running `tmux-status`: tmux keeps the old inode
of a `#()` job alive and will not respawn it just because the file on disk
changed.

## The status line

* **CPU** — busy percentage over the last interval, from `/proc/stat`. Turns
  red at 70 %.
* **Network** — average throughput over the last interval in decimal Mbit/s,
  from `/proc/net/dev`. Loopback and virtual interfaces (`veth*`, `docker*`,
  `podman*`, `br-*`, …) are skipped so bridged traffic is not counted twice.
* **Battery** — charge, remaining time and current draw from
  `/sys/class/power_supply/BAT0`. Remaining time uses a weighted average of the
  last twelve power samples, so it follows load changes without jumping every
  tick. Batteries exposed through the SBS driver report `charge_*`/`current_*`
  in µAh/µA instead of `energy_*`/`power_*` in µWh/µW; both are handled.

Colours are [Catppuccin Mocha](https://github.com/catppuccin/catppuccin).

`--loop [seconds]` is the intended mode: tmux starts a `#()` job once and reads
lines from it for as long as the process lives, which avoids a fork/exec per
tick — at roughly 1 ms that was by far the largest cost. The program exits by
itself via SIGPIPE once tmux closes the read end, and only writes a line when
the rendered output actually changed, so an idle bar causes no redraws at all.

Without `--loop` it prints one line and keeps its counters in
`$XDG_RUNTIME_DIR/.tmux-cpu`, which is useful for testing. The battery path can
be overridden at build time:

```sh
make CFLAGS='-O2 -DBATDIR=\"/sys/class/power_supply/BAT1/\"'
```

## The view cycle

`tmux-cycle-view` cycles a session between three arrangements: every terminal
side by side, the same panes tiled, and one window per terminal. `--next` steps
to the next window, or to the next pane once everything is gathered. `--new`
adds a terminal *inside* the current arrangement instead of always creating a
window. Bound to `prefix + Space`, and to the Exposé key through the daemon.

Called without a window id it has no tmux context to work from, so it resolves
the session over `#{client_session}` from `list-clients`. Neither a bare
`display -p` nor `display -p -c <client>` reports the session a client is
actually looking at — both answer with the most recently used one, which is a
different session more often than it sounds.

## specialkeysd

Booting into a bare sway session means nothing handles the laptop's special
keys any more — powerdevil and kglobalaccel used to. `specialkeysd` takes over:
display and keyboard brightness, dimming after an idle timeout, and the
Exposé/Dashboard keys, which drive the view cycle above. It reads `/dev/input`
directly and writes `/sys/class/backlight`, so it runs as root and drops to the
desktop user to talk to tmux.

The idle timeout is 30 minutes on battery and 60 on AC (`ExecStart` in the
unit). Anything that writes text without touching a keyboard — the m5assistant
dictation types straight into the tmux buffer with `send-keys` — counts as
activity by touching `$XDG_RUNTIME_DIR/specialkeysd-wake`; the daemon only
looks at the mtime and wakes the screen within five seconds.

Keys must be handled in exactly one place. Binding one of them in the
compositor as well means two actions per press — the comments in `sway.config`
record both times that happened, once for brightness and once for Exposé.

## Notes

The glyphs (, 󰁹, 󰂄) come from a Nerd Font — the terminal needs one to render
them. Source comments are in German.
