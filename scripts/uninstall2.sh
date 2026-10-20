#!/bin/sh
set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo "Run this user uninstaller as your regular user, without sudo." >&2
    exit 1
fi

: "${HOME:?HOME must be set}"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
case "$data_home" in
    /*) ;;
    *)
        echo "XDG_DATA_HOME must be an absolute path." >&2
        exit 1
        ;;
esac
if [ "$data_home" = / ]; then
    echo "Refusing to uninstall from the filesystem root." >&2
    exit 1
fi

install_dir="$data_home/macos-de"
rm -rf -- "$install_dir"
rm -f -- "$data_home/xsessions/macos-de.desktop"
rm -f -- "$data_home/applications/macos-de-settings.desktop" \
    "$data_home/applications/macos-de-screenshot-manager.desktop" \
    "$data_home/applications/macos-de-notification-manager.desktop" \
    "$data_home/applications/macos-de-easy-file.desktop" \
    "$data_home/applications/macos-de-easy-term.desktop" \
    "$data_home/applications/macos-de-easy-monitor.desktop"
session_link="$HOME/.local/bin/macos-de-session"
if [ -L "$session_link" ] &&
   [ "$(readlink "$session_link")" = "$install_dir/bin/macos-de-session-local" ]; then
    rm -f -- "$session_link"
fi

echo "User installation of macOS-DE removed."
echo "System PAM, backlight/udev permissions, and video-group membership were left unchanged."
