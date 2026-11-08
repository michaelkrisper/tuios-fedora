# CLAUDE.md

tuios-fedora versions a TTY kiosk setup for a MacBook Pro 11,1 on Fedora 44: tty1
autologin starts sway with `foot --server`, every window is a tab in swaybar, and two
dependency-free C programs replace what a desktop used to do: `swaybar-status` (the
bar: tabs plus status) and `specialkeysd` (brightness, keyboard backlight, idle dimming,
Exposé/Dashboard keys). Everything else is config files mirroring the real paths under
`home/` (= `~`) and `system/` (= `/`). `SETUP.md` is the reproduction guide; nothing
deploys automatically.

## Commands

```sh
make CFLAGS='-O2 -Wall -Wextra -Werror' all   # what CI builds, with gcc and clang
make test                                     # unit tests in tests/
make clean && make test CFLAGS='-O2 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all'
make install          # swaybar-status -> ~/.local/bin, then swaymsg reload
make install-daemon   # specialkeysd -> /usr/local/sbin + unit, needs sudo
```

- CI (`.github/workflows/ci.yml`): job `c` builds and tests with gcc and clang, then
  again under ASan/UBSan; job `shell` runs shellcheck on an explicit file list and
  `py_compile` on `home/local/bin/resume-check`. A new shell script must be added to that
  list, or CI will not check it. `.shellcheckrc` disables SC1090/SC1091 (sourced files
  exist only on the target machine).
- Battery path can be overridden at build time: `-DBATDIR=\"/sys/class/power_supply/BAT1/\"`.

## Architecture

- `src/swaybar-status.c`: swaybar's `status_command` (`--loop 5`). In loop mode it also
  holds sway IPC connections and manages tabs: one window per workspace, workspaces
  renamed `N:<title>` (bar strips the number). Key bindings in the sway config talk to it
  through `swaymsg -t send_tick <word>` (`next`, `prev`, `split`, `volume`). Output is
  Pango markup; it writes a line only when the output changed. The Claude weekly limit
  appears while `$XDG_RUNTIME_DIR/claude-bar/limits` exists (written by
  `statusline --bar` from the claude-statusline repo; inotify triggers a redraw).
- `src/specialkeysd.c`: root daemon reading `/dev/input`, writing `/sys/class/backlight`,
  calling `swaymsg` as the desktop user (socket found by globbing
  `/run/user/UID/sway-ipc.*.sock`). Anything that types without a keyboard can wake it by
  touching `$XDG_RUNTIME_DIR/specialkeysd-wake` (mtime only).
- `tests/*.c` `#include` the program source (with `main` renamed) to reach static
  helpers; `tests/check.h` is a tiny counted-assertion harness. No test framework.
- `home/config/tty-claude.sh`: tty1 starter (sway, then Claude Code in the first terminal).
- `system/usr/lib/systemd/system-sleep/*` and several units under `system/` are kept for
  reference only and are not enabled (see SETUP.md "Known open points").

## Conventions

- Code, comments and docs in English; user-facing strings (unit descriptions,
  `resume-check` output) in German.
- C: no dependencies beyond libc and kernel headers; the bar avoids stdio streams on
  purpose (open/read/write, own number formatter). Keep it fork-free per tick: the only
  fork is `wpctl get-volume`, run at start and on the `volume` tick.
- Fedora-specific steps in SETUP.md are marked **[fedora]** so the rest can be ported.
- Colours are Catppuccin Mocha; glyphs need a Nerd Font.

## Rules and pitfalls

- The live machine runs copies of `home/` and `system/` files, not symlinks into this
  repo; nothing installs them except `make install`/`install-daemon` for the two
  binaries. A config change here must also be applied at the live path (and vice versa).
  The live copies may carry German comments; never copy them over the repo files wholesale.
- Handle each key in exactly one place, either `specialkeysd` or the sway config. Binding
  it in both fires two actions per press (happened twice: brightness and Exposé).
- swaybar keeps the old `status_command` running after a binary update; only
  `swaymsg reload` restarts it (`make install` does that).
- Never kill the bar or daemon by pattern (`pkill -f`, `kill $(pgrep -f ...)`): the
  pattern also matches the calling shell's command line, which then dies too. Use
  `pkill -x swaybar-status`.
- The WLAN interface (`wlp3s0`) and battery (`BAT0`) are fixed in the source; the WLAN
  signal comes from the nl80211 scan cache because the `wl` driver refuses
  `GET_STATION` to non-root.
- Public repo: never add WLAN names, addresses, serials, router or account details to
  configs or docs.

## Workflow

- Every change starts with a GitHub issue. Branch `<issue>-<slug>` from `main`,
  open a PR with `Closes #<issue>`. Never push to `main` directly.
- PRs auto-merge (squash) once CI and the "Linked ticket" check pass; drafts are not merged.
- Versions exist only as git tags `vX.Y.Z`. Every merge to `main` tags the next patch;
  never edit version numbers in files. Minor/major releases: Actions -> Release.
- Nothing deploys automatically: install on the machine with `make install`,
  `make install-daemon`, and by copying changed files from `home/` and `system/`.
- Details: CONTRIBUTING.md.
