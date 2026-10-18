#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this uninstaller as root (for example: sudo make uninstall)." >&2
    exit 1
fi

rm -rf -- /usr/share/macos-de
rm -f -- /usr/share/xsessions/macos-de.desktop
rm -f -- /usr/share/applications/macos-de-settings.desktop \
    /usr/share/applications/macos-de-screenshot-manager.desktop \
    /usr/share/applications/macos-de-notification-manager.desktop
rm -f -- /etc/udev/rules.d/99-macos-de-backlight.rules
rm -f -- /etc/pam.d/macos-de
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules
    udevadm trigger --subsystem-match=backlight
fi

echo "macOS-DE uninstalled. The user's video-group membership was left unchanged."