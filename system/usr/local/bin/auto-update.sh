#!/usr/bin/bash
# Daily full update of all package managers. Runs as root via auto-update.timer.
# Logs: journalctl -u auto-update.service
set -uo pipefail

USER_NAME=michi
rc=0
step() { echo "==== $* ===="; }

step "dnf upgrade"
dnf upgrade -y --refresh || rc=1

step "flatpak update (system)"
flatpak update -y --noninteractive || rc=1

step "npm global update"
npm update -g || rc=1

step "rustup update (als $USER_NAME)"
runuser -l "$USER_NAME" -c 'rustup update' || rc=1

# Binaries built with `cargo install` — only if cargo-update is installed
if runuser -l "$USER_NAME" -c 'command -v cargo-install-update' >/dev/null 2>&1; then
  step "cargo install-update (als $USER_NAME)"
  runuser -l "$USER_NAME" -c 'cargo install-update -a' || rc=1
fi

# dnf5 has no /usr/bin/needs-restarting (that came from dnf-utils, dnf4);
# the subcommand returns rc=1 when a reboot is needed.
if ! dnf needs-restarting -r >/dev/null 2>&1; then
  step "HINWEIS: Reboot empfohlen (Kernel/Core-Update)"
  mkdir -p /run/motd.d
  printf '*** Reboot empfohlen: Kernel/Core-Update seit dem letzten Boot (%s) ***\n' \
    "$(date '+%Y-%m-%d %H:%M')" > /run/motd.d/95-reboot-needed.motd
else
  rm -f /run/motd.d/95-reboot-needed.motd
fi

step "fertig (rc=$rc)"
exit $rc
