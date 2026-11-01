# tuios-fedora

A MacBook Pro 11,1 running Fedora that boots straight into a terminal — no
desktop, no compositor chrome, no session manager. every window gets its
own sway workspace, and the workspace buttons in swaybar act as tabs next to
the status line — terminals are `footclient` windows of a
single `foot --server`, Chrome sits next to them as just another tab — and two small
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
| `swaybar-status.c` | the one bar: tabs (one window per workspace, named after its title), Claude context + 5 h limit of the focused tab, CPU, network, battery, clock — dependency-free C |
| `specialkeysd.c` | brightness, keyboard backlight, idle dimming, and the Exposé/Dashboard keys |
| `sway.config` | bar with tabs + status, foot server, touchpad |
| `specialkeysd.service` | runs the daemon as root, because `/dev/input` and backlight need it |

`SETUP.md` documents how to reproduce the whole thing on a fresh Fedora.

## Build

```sh
make && make install          # -> ~/.local/bin
make install-daemon           # -> /usr/local/sbin, needs sudo
```

`make install` also runs `swaymsg reload`: swaybar keeps the old binary running
and only restarts `status_command` on reload.

## The status line

In `--loop` mode the program also holds two sway IPC connections: every new
window on an occupied workspace moves to a fresh one behind the last, every
workspace is renamed "N:<window title>" (the bar strips the number), and
closing the last window of the visible workspace jumps to the previous tab.
The window title is the working directory (bash `PROMPT_COMMAND`, and
`statusline --bar` from claude-statusline inside Claude Code). That same path
keys `$XDG_RUNTIME_DIR/claude-bar/<path, / as %>`, where `statusline --bar`
leaves the session's context gauge; `limits` holds the 5 h window. inotify on
that directory redraws immediately.

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

Output is Pango markup (`pango_markup enabled` in the bar block).

`--loop [seconds]` is the intended mode: swaybar starts `status_command` once and reads
lines from it for as long as the process lives, which avoids a fork/exec per
tick — at roughly 1 ms that was by far the largest cost. The program exits by
itself via SIGPIPE once swaybar closes the read end, and only writes a line when
the rendered output actually changed, so an idle bar causes no redraws at all.

Without `--loop` it prints one line and keeps its counters in
`$XDG_RUNTIME_DIR/.tmux-cpu`, which is useful for testing. The battery path can
be overridden at build time:

```sh
make CFLAGS='-O2 -DBATDIR=\"/sys/class/power_supply/BAT1/\"'
```

## specialkeysd

Booting into a bare sway session means nothing handles the laptop's special
keys any more — powerdevil and kglobalaccel used to. `specialkeysd` takes over:
display and keyboard brightness, dimming after an idle timeout, and the
Exposé/Dashboard keys (F3 = next tab, F4 = new terminal). It reads `/dev/input`
directly and writes `/sys/class/backlight`, so it runs as root and drops to the
desktop user to call `swaymsg` (socket found by globbing
`/run/user/UID/sway-ipc.*.sock`).

The idle timeout is 30 minutes on battery and 60 on AC (`ExecStart` in the
unit). Anything that writes text without touching a keyboard — the m5assistant
dictation, which types without a keyboard — counts as
activity by touching `$XDG_RUNTIME_DIR/specialkeysd-wake`; the daemon only
looks at the mtime and wakes the screen within five seconds.

Keys must be handled in exactly one place. Binding one of them in the
compositor as well means two actions per press — the comments in `sway.config`
record both times that happened, once for brightness and once for Exposé.

## Notes

The glyphs (, 󰁹, 󰂄) come from a Nerd Font — the terminal needs one to render
them. Source comments are in German.
