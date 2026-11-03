#!/usr/bin/env bash
# Short CPU performance burst right after resume so the wake-up work finishes
# quickly. Started in the background via systemd-run, so it does not block
# resume. Turbo stays on afterwards (see cpu-power.service).
set -u
BOOST_SECONDS=4
gov() { for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$1" > "$g" 2>/dev/null || true; done; }

gov performance
sleep "$BOOST_SECONDS"
gov powersave
