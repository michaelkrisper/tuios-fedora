# shellcheck shell=bash
# tty1: start the Wayland session (sway + foot) and Claude Code inside it.
# Bare text console only:  touch ~/.no-sway
# No autostart at all:     touch ~/.no-autoclaude
#
# 2026-09-20: the 3-second wait with a Ctrl-C escape is gone -- NAutoVTs=6,
# Alt+F2 gives a login console at any time. The "kde" hint had been wrong since
# Plasma was uninstalled. The cage fallback is gone too (package removed);
# without sway you land directly in claude on the text console.
# Brightness: `bl 30`.

if [ -e "$HOME/.no-autoclaude" ]; then
    :
elif [ "$(tty)" = "/dev/tty1" ] && [ -z "$WAYLAND_DISPLAY$DISPLAY" ] && [ -z "$AUTOCLAUDE_STAGE" ]; then
    # Stage 1 - text console
    # wlroots only knows XKB_DEFAULT_*; without these variables you get "us".
    # The source stays localectl, so there is a single source of truth.
    if [ -r /etc/X11/xorg.conf.d/00-keyboard.conf ]; then
        eval "$(awk -F'"' '
            /XkbLayout/  {printf "XKB_DEFAULT_LAYOUT=%s\n",  $4}
            /XkbModel/   {printf "XKB_DEFAULT_MODEL=%s\n",   $4}
            /XkbVariant/ {printf "XKB_DEFAULT_VARIANT=%s\n", $4}
            /XkbOptions/ {printf "XKB_DEFAULT_OPTIONS=%s\n", $4}
        ' /etc/X11/xorg.conf.d/00-keyboard.conf)"
        export XKB_DEFAULT_LAYOUT XKB_DEFAULT_MODEL XKB_DEFAULT_VARIANT XKB_DEFAULT_OPTIONS
    fi

    if command -v sway >/dev/null && [ ! -e "$HOME/.no-sway" ]; then
        AUTOCLAUDE_STAGE="done" sway   # stage 2 is set by the exec in the sway config
    else
        AUTOCLAUDE_STAGE="done"; export AUTOCLAUDE_STAGE
        cd "$HOME" && claude
    fi
    export AUTOCLAUDE_STAGE="done"
elif [ "$AUTOCLAUDE_STAGE" = "kiosk" ]; then
    # Stage 2 - in foot under sway
    export AUTOCLAUDE_STAGE="done"
    # shellcheck disable=SC2164  # on failure, start claude in the current dir
    cd "$HOME"
    # More Claude sessions as separate tabs: Super+C (sway).
    claude
fi
