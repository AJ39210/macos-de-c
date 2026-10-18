#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root (for example: sudo make install)." >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
install_dir=/usr/share/macos-de
session_dir=/usr/share/xsessions
applications_dir=/usr/share/applications
udev_rule=/etc/udev/rules.d/99-macos-de-backlight.rules
pam_config=/etc/pam.d/macos-de

for required in bin assets css apps applications widgets; do
    if [ ! -d "$build_dir/$required" ]; then
        echo "Missing build output: $build_dir/$required (run 'make' first)." >&2
        exit 1
    fi
done

desktop_entry="$build_dir/sessions/macos-de.desktop"
if [ ! -f "$desktop_entry" ]; then
    echo "Missing generated desktop entry: $desktop_entry (run 'make' first)." >&2
    exit 1
fi

install -d "$install_dir" "$session_dir" "$applications_dir"
cp -a "$build_dir/bin" "$build_dir/assets" "$build_dir/css" \
    "$build_dir/apps" "$build_dir/widgets" "$install_dir/"
install -m 644 "$desktop_entry" "$session_dir/macos-de.desktop"
install -m 644 "$build_dir"/applications/*.desktop "$applications_dir/"
install -m 644 "$project_dir/scripts/pam-macos-de" "$pam_config"

if getent group video >/dev/null 2>&1; then
    install -D -m 644 "$project_dir/scripts/99-macos-de-backlight.rules" \
        "$udev_rule"
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        usermod -a -G video "$SUDO_USER"
        echo "Added $SUDO_USER to the video group for display brightness control."
    else
        echo "No invoking desktop user was detected. Add your login user to the video group to control brightness." >&2
    fi
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules
        udevadm trigger --subsystem-match=backlight
    else
        echo "udevadm was not found; backlight permissions will update on the next boot." >&2
    fi
else
    echo "The video group does not exist; display brightness permissions were not configured." >&2
fi

echo "macOS-DE installed. Select it from the display manager's session list."
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    echo "Log out and back in before testing the brightness slider."
fi