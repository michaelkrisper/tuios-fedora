# Reproducing this setup

Target hardware: MacBook Pro 11,1 (2013, Haswell, BCM4360 wifi, Cirrus audio,
FaceTime HD camera), Fedora 44. Everything below is what the machine actually
runs, not an idealised version of it — the file tree under `home/` and
`system/` mirrors the real paths.

Anything Fedora-specific is marked **[fedora]**; the rest ports to another
distribution unchanged, which is the point of the split.

---

## 0. Layout of this repo

```
src/                  the two C programs
bin/                  scripts that go to ~/.local/bin
home/                 user files; home/config/x  ->  ~/.config/x
home/bash_profile     ->  ~/.bash_profile      (loads the kiosk starter)
home/local/bin/       ->  ~/.local/bin
system/               root-owned files, mirroring / — copy with sudo
```

The live machine symlinks the four files that change often back into this
repo, so there are no drifting copies:

```sh
ln -sf ~/tuios-fedora/home/config/tmux/tmux.conf ~/.config/tmux/tmux.conf
ln -sf ~/tuios-fedora/home/config/sway/config    ~/.config/sway/config
ln -sf ~/tuios-fedora/src/tmux-status.c          ~/.local/src/tmux-status.c
ln -sf ~/tuios-fedora/src/specialkeysd.c         ~/.local/src/specialkeysd.c
```

---

## 1. Packages **[fedora]**

```sh
sudo dnf install -y \
  sway swaybg foot fuzzel tmux \
  cascadia-mono-nf-fonts terminus-fonts-console \
  brightnessctl wl-clipboard grim slurp kbd \
  tlp powertop thermald pciutils zram-generator grubby \
  NetworkManager-wifi \
  git gh ripgrep fd-find bat eza fzf jq htop git-delta
```

Wifi needs the out-of-tree Broadcom driver — the BCM4360 has no in-kernel
support: `akmod-wl` + `broadcom-wl` from RPM Fusion. This is the one piece that
makes resume slow (~8.4 s) and cannot be fixed from here.

`mbpfan` (fan control) is built from source, there is no RPM.

`tlp` replaced the hand-written power units on 2026-09-20; its config lives in
`/etc/tlp.d/01-mbp.conf` (see step 7). `tlp.service` is `Type=oneshot` and
udev-triggered, so nothing sits resident. **Mask `tlp-pd.service`** — the
package ships a PowerProfiles D-Bus daemon (35 MB of Python) that `disable`
alone does not stop, because D-Bus activates it again.

`pipewire`/`wireplumber` are installed but masked, see step 6. `cage` was
removed on 2026-09-20; sway had been the only thing actually running for
months.

---

## 2. Boot into a terminal, not a desktop

```sh
systemctl set-default multi-user.target          # no display manager
sudo install -Dm644 system/etc/systemd/system/getty@tty1.service.d/autologin.conf \
     /etc/systemd/system/getty@tty1.service.d/autologin.conf
```

The drop-in makes `agetty` log the user in on tty1 without a prompt. From
there the chain is:

```
tty1 login -> ~/.bash_profile -> ~/.config/tty-claude.sh
      stage 1   on /dev/tty1, no WAYLAND_DISPLAY:  AUTOCLAUDE_STAGE=done sway
      sway      config execs:                      env AUTOCLAUDE_STAGE=kiosk foot
      stage 2   foot's login shell sources the script again, sees "kiosk",
                and starts tmux + claude
```

`AUTOCLAUDE_STAGE` is what keeps the script from recursing into itself when it
is sourced a second and third time. `touch ~/.no-sway` drops you straight
into claude on the text console. Copy both files:

```sh
cp home/config/tty-claude.sh ~/.config/tty-claude.sh
cp home/bash_profile ~/.bash_profile      # sources the above at the end
```

Two things this depends on and that are easy to get wrong:

* `login-shell=yes` in `foot.ini` — otherwise foot starts a non-login shell,
  `.bash_profile` never runs, and stage 2 never happens.
* The script must not loop. When Claude exits you land in a plain shell; a
  restart loop makes the machine unusable over ssh-less serial.

Escape hatches, created with `touch`, deliberately not in this repo:
`~/.no-sway`, `~/.no-tmux`, `~/.no-autoclaude`.

`bin/bootmode` used to switch the default target between tty and KDE. Plasma
was uninstalled on 2026-09-20, so the `kde` branch now only prints a hint;
`graphical.target` has no display manager left.

---

## 3. The kiosk: sway and foot

```sh
mkdir -p ~/.config/sway ~/.config/foot
cp home/config/sway/config ~/.config/sway/config
cp home/config/foot/foot.ini ~/.config/foot/foot.ini
```

sway runs one fullscreen `foot`. The console font and keymap matter because
the plain text console is still there underneath **[fedora]**:

```sh
sudo cp system/etc/vconsole.conf /etc/vconsole.conf     # de-mac, ter-v32n
sudo cp system/etc/X11/xorg.conf.d/00-keyboard.conf /etc/X11/xorg.conf.d/
```

The sway config binds almost nothing on purpose. Special keys are handled by
`specialkeysd` (step 5) and binding them here too means two actions per press —
the comments in the config record both times that happened.

---

## 4. tmux and the status line

```sh
mkdir -p ~/.config/tmux
ln -sf $PWD/home/config/tmux/tmux.conf ~/.config/tmux/tmux.conf
make && make install                    # tmux-status + tmux-cycle-view -> ~/.local/bin
```

Keys, once running:

| | |
|---|---|
| `Alt-Tab`, Exposé key | next window; next pane when everything is gathered |
| `Ctrl` + Exposé key | cycle view: side by side → tiled → tabs |
| Dashboard key, `Ctrl-T` | new terminal, inside the current arrangement |
| `prefix + Space` | same cycle, from inside tmux |
| `prefix + A/T/B` | gather / tile / split back out, individually |
| `prefix + C` | new window running Claude |

---

## 5. specialkeysd

```sh
make install-daemon        # -> /usr/local/sbin, unit, enable --now
```

Without a desktop nothing handles brightness, keyboard backlight, idle dimming
or the Exposé/Dashboard keys. This daemon reads `/dev/input` directly and
writes `/sys/class/backlight`, which is why it runs as root and drops to the
desktop user to talk to tmux. `bin/bl` sets the backlight by percentage from a
shell.

---

## 6. Power and hardware

Kernel command line **[fedora]** — `/etc/default/grub` and `/etc/kernel/cmdline`
must be kept in sync, and BLS entries under `/boot/loader/entries/` are
regenerated from them:

```
rhgb quiet mem_sleep_default=deep pcie_aspm.policy=powersave
i915.enable_psr=2 i915.enable_fbc=1 thunderbolt.hosts_only=1
acpi_osi=!Darwin intel_pstate=active
resume=UUID=<root-uuid> resume_offset=<see below>
```

`acpi_osi=!Darwin` stops the firmware pretending to be macOS; without it the
generic ACPI battery is replaced by the SBS driver, which reports µAh instead
of µWh (`tmux-status` handles both).

Hardware that is switched off because this machine never uses it — camera,
both audio controllers including the internal microphone, and the Thunderbolt
controller:

```sh
sudo cp system/etc/modprobe.d/*.conf /etc/modprobe.d/
sudo cp system/etc/sysctl.d/99-mbp-tuning.conf /etc/sysctl.d/
sudo cp system/etc/udev/rules.d/*.rules /etc/udev/rules.d/
sudo cp system/etc/NetworkManager/conf.d/99-fast-wifi.conf /etc/NetworkManager/conf.d/
sudo cp system/etc/systemd/zram-generator.conf /etc/systemd/
sudo install -Dm644 system/etc/systemd/logind.conf.d/90-powerkey.conf \
     /etc/systemd/logind.conf.d/90-powerkey.conf
```

Services and helper scripts:

```sh
sudo cp system/usr/local/bin/*.sh /usr/local/bin/
sudo cp system/etc/systemd/system/*.service system/etc/systemd/system/*.timer \
     /etc/systemd/system/
sudo cp system/etc/mbpfan.conf /etc/mbpfan.conf
sudo systemctl daemon-reload
sudo systemctl enable --now tlp mbpfan auto-update.timer
sudo systemctl mask tlp-pd
```

| unit | what it does |
|---|---|
| `tlp` | governor, `energy_perf_bias=15`, turbo, SATA/PCIe/USB power policy, wifi `power_save` — everything the four hand-written units used to do |
| `mbpfan` | fan curve; the Apple SMC needs one, nothing else provides it |
| `auto-update.timer` | daily dnf/flatpak/npm update **[fedora]** |
| `specialkeysd` | the key daemon from step 5 |

Suspend hooks go to `/usr/lib/systemd/system-sleep/`, **not** `/etc/systemd/`:
SELinux labels files under `/etc` as `etc_t`, `systemd-sleep` refuses to run
them, and the denial is `dontaudit`ed — so it fails completely silently
**[fedora]**. They must also be executable; a non-executable hook is skipped
just as silently.

```sh
sudo cp system/usr/lib/systemd/system-sleep/* /usr/lib/systemd/system-sleep/
sudo chmod +x /usr/lib/systemd/system-sleep/*
```

The proprietary `wl` driver has three quirks that matter for the home wifi:

- It cannot join WPA2/WPA3 mixed networks: NetworkManager always offers
  `WPA-PSK-SHA256` as well, and `wl` rejects the association, which nmcli
  reports misleadingly as "network could not be found". The access point has
  to run plain WPA2-PSK.
- It does not deauthenticate cleanly on disconnect or suspend. The access
  point keeps the stale station and ignores new authentication until its
  inactivity timer fires, so reconnects after a short sleep hang for minutes.
  Fix on the router side (`ap_max_inactivity` 60 s instead of 300 s) and keep
  NetworkManager from giving up meanwhile:

  ```sh
  sudo nmcli con mod <ssid> 802-11-wireless-security.pmf disable \
       connection.autoconnect-retries 0 connection.auth-retries 0 \
       connection.autoconnect-priority 10
  ```
- Never `modprobe -r wl` to recover it: the reload crashes the driver
  (`Scan_results error (-22)`) and wifi stays dead until a reboot.

Closing the lid hibernates instead of suspending. zram cannot hold a
hibernation image, so an 8 GiB swapfile sits in its own btrfs subvolume at
priority 0 (zram keeps priority 100 for normal swapping). The swapfile needs
the SELinux type `swapfile_t` or logind reports that hibernation is unavailable
**[fedora]**. `resume_offset` depends on where the file lands on disk, so it
has to be read out again on every new install; the value in the repo's cmdline
applies only to this machine:

```sh
sudo btrfs subvolume create /var/swap
sudo btrfs filesystem mkswapfile --size 8G /var/swap/swapfile
sudo semanage fcontext -a -t swapfile_t '/var/swap(/.*)?'
sudo restorecon -Rv /var/swap
echo '/var/swap/swapfile none swap defaults,pri=0 0 0' | sudo tee -a /etc/fstab
sudo swapon -a
sudo cp system/etc/dracut.conf.d/resume.conf /etc/dracut.conf.d/
sudo dracut -f --regenerate-all
sudo grubby --update-kernel=ALL --args="resume=UUID=$(findmnt -no UUID /) \
     resume_offset=$(sudo btrfs inspect-internal map-swapfile -r /var/swap/swapfile)"
sudo install -Dm644 system/etc/systemd/logind.conf.d/91-lid-hibernate.conf \
     /etc/systemd/logind.conf.d/91-lid-hibernate.conf
sudo systemctl reload systemd-logind
```

## 7. Services that are switched off

Masked system-wide (each is a symlink to `/dev/null`):

```
abrtd alsa-restore alsa-state avahi-daemon cups geoclue iio-sensor-proxy
ModemManager packagekit passim pcscd rtkit-daemon switcheroo-control
```

Masked in the user session — no audio stack at all, which is consistent with
the audio hardware being removed from the bus:

```
pipewire pipewire.socket pipewire-pulse pipewire-pulse.socket wireplumber
obex at-spi-dbus-bus kunifiedpush-distributor plasma-startupsound
```

```sh
sudo systemctl mask abrtd alsa-restore alsa-state avahi-daemon cups geoclue \
     iio-sensor-proxy ModemManager packagekit passim pcscd rtkit-daemon \
     switcheroo-control
systemctl --user mask pipewire pipewire.socket pipewire-pulse \
     pipewire-pulse.socket wireplumber obex at-spi-dbus-bus \
     kunifiedpush-distributor plasma-startupsound
```

`plasmalogin.service` stays enabled but never starts, because it is wanted by
`graphical.target` and the default target is `multi-user.target`. That is how
`bin/bootmode` can switch back to a desktop without reinstalling anything.

---

## 8. Known open points

* **Resume takes ~8.4 s**, almost all of it in the `wl` wifi driver. Not
  fixable without replacing the card.
* **Suspend takes ~9.6 s** in the firmware's `_PTS` method, which polls the
  camera's power state up to 5000 times. `30-camera-d3` (write the camera to
  D3 first) and `pci-remove-unused` (drop it off the bus) cancelled each other
  out, so the idea was never actually tested. Both were deleted on 2026-09-20
  at the owner's request, together with `40-smt-defer` (which had been saving
  ~2.3 s per resume) and `50-resume-cpu-boost`. The `_PTS` brake is therefore
  accepted, not solved. The scripts are still in this repo's history if anyone
  wants to revisit it.
* A DSDT override was considered for the same problem and rejected as too
  risky for the gain.

---

## 9. Porting to another distribution

Unchanged: everything under `home/`, `src/`, `bin/`, the tmux/sway/foot
configuration, the daemon and its unit, the masking lists, the modprobe
blacklists, sysctl and udev rules.

Needs replacing: the package list and its names, the kernel command line
mechanism (`/etc/kernel/cmdline` + grubby vs. whatever the target uses), the
SELinux caveat about sleep-hook locations, and `auto-update.sh`, which is built
around dnf.
