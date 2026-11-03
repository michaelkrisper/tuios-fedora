#!/usr/bin/env bash
# Remove unused PCI devices. A missing device is not an error.
#
# 02:00.0 is the FaceTime camera. It is not only here because of its idle
# draw: with acpi_osi=!Darwin, \_PTS waits up to 5000 x 1 ms for
# \_SB.PCI0.RP02.CMRA.DPST to report 3. With the camera removed, root port
# 00:1c.1 is empty and runtime-suspended, the ACPI config read returns 0xFF --
# lower 2 bits = 3, and the loop exits immediately.
# Writing it to D3 via setpci before suspend is NOT enough: the PCI core also
# puts the root port to sleep in dpm_suspend_noirq, and that happens before
# \_PTS (measured 2026-08-31: still 9.6 s).
set -u
for d in 0000:05:00.0 0000:00:03.0 0000:00:1b.0 0000:02:00.0; do
    f="/sys/bus/pci/devices/$d/remove"
    if [ -e "$f" ]; then
        echo 1 > "$f" && logger "pci-remove-unused: $d entfernt"
    fi
done
