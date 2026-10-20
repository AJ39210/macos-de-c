# Manual installation on Linux

This guide explains how to install macOS-DE without using the project
installers. macOS-DE currently runs as an **X11** desktop session; it is not a
Wayland session.

## 1. Install build dependencies

Install the development packages for GTK 3, X11, Pango, PAM, and the other
libraries listed in the root [README](../README.MD). Package names vary by
distribution. You will also need a C compiler, `make`, and `pkg-config`.
Transparency is drawn through GTK/GDK's RGBA visuals and Cairo, with X11
extensions used for window shapes. A compositor is needed to display
transparency; install **picom** for this session. macOS-DE starts picom with
fullscreen unredirect enabled when no compositor is already running, so games
can bypass compositing where supported. The Easy-Term application also needs
the VTE 2.91 runtime library.

For example, on Debian or Ubuntu:

```sh
sudo apt install libgtk-3-dev libcairo2-dev libx11-dev libxext-dev \
  libxft-dev libxcursor-dev libgdk-pixbuf-2.0-dev libfontconfig1-dev \
  libpango1.0-dev libpam0g-dev libxss-dev picom libvte-2.91-0
```

These package names are examples only; use the equivalent development and
runtime packages on your distribution. If your distribution does not provide
`libvte-2.91-0`, install its VTE 2.91 runtime package for Easy-Term.

## 2. Build macOS-DE

From the project root, build the desktop and its applications as a regular user:

```sh
make
```

The build stages the installable files under `build/`. Make sure these
directories exist before continuing:

- `build/bin` — desktop executable, window manager, and session scripts
- `build/assets` and `build/css` — artwork, fonts, and styles
- `build/apps` — bundled applications
- `build/widgets` — bundled widget resources
- `build/sessions` and `build/applications` — generated `.desktop` entries

## 3. Copy the files into the system

The project expects its shared files at `/usr/share/macos-de`. Keeping this
location avoids breaking hard-coded resource and application paths. Run the
following commands as root (or use your distribution's equivalent
administrator method), from the project root:

```sh
install -d /usr/share/macos-de \
  /usr/share/xsessions \
  /usr/share/applications

cp -a build/bin build/assets build/css build/apps build/widgets \
  /usr/share/macos-de/

install -m 644 build/sessions/macos-de.desktop \
  /usr/share/xsessions/macos-de.desktop
install -m 644 build/applications/*.desktop \
  /usr/share/applications/
```

The build now includes Easy-monitor in `build/apps/Easy-monitor` and generates
its launcher in `build/applications/macos-de-easy-monitor.desktop`. The wildcard
copy above installs the launcher along with the other bundled applications.

If your distribution stores X11 sessions somewhere other than
`/usr/share/xsessions`, copy `build/sessions/macos-de.desktop` to the session
directory used by its display manager. This is an X11 session entry; putting it
in a Wayland session directory will not make macOS-DE run on Wayland.

If you install the shared files somewhere other than `/usr/share/macos-de`,
update the `Exec=` and `Icon=` paths in the session and application desktop
entries, and make sure the resource paths used by the desktop and window
manager resolve to the new location. The canonical location is recommended.

## 4. Optional system integration

### PAM lock-screen authentication

The lock screen uses the PAM service name `macos-de`. On distributions whose
PAM configuration supports the `common-auth` and `common-account` includes,
install the supplied service file:

```sh
install -D -m 644 scripts/pam-macos-de /etc/pam.d/macos-de
```

The supplied file contains:

```text
@include common-auth
@include common-account
```

PAM include syntax and policy filenames vary by distribution. Check your
distribution's PAM documentation and adapt this service file if necessary;
do not replace or edit the system's global authentication policy. Without a
compatible PAM service, password unlock may not work.

The `livesession`, `default`, and `system` accounts intentionally start
unlocked and do not use automatic idle locking. Other accounts use the timeout
selected in DE Settings; they are locked on startup as well.

### Display brightness permissions

This step is optional. To allow ordinary users to write display-backlight
values, install the supplied udev rule if your system uses udev and has a
`video` group:

```sh
install -D -m 644 scripts/99-macos-de-backlight.rules \
  /etc/udev/rules.d/99-macos-de-backlight.rules
```

Add the login user to the `video` group using your distribution's user/group
tools, then reload udev rules using the appropriate command for your system
(commonly `udevadm control --reload-rules` followed by
`udevadm trigger --subsystem-match=backlight`). Log out and back in for group
membership changes to apply. Review the security implications of granting
backlight-device access on your system before enabling this.

## 5. Select and start the session

Log out of the current desktop and choose **macOS-DE** in the display manager's
session selector. The session entry launches
`/usr/share/macos-de/bin/macos-de-session`, which starts the desktop and window
manager from the installed `bin` directory.

If the display manager does not detect the session, confirm that the session
file is in its X11 session directory and that the file is readable. You can
also test the session from an existing X11 session by running:

```sh
/usr/share/macos-de/bin/macos-de-session
```

For a per-user install that does not require administrator access, see
[`install2.sh`](./install2.sh) and [`uninstall2.sh`](./uninstall2.sh).

## Manual uninstall

Remove only the files you installed. For the canonical system-wide paths above,
the project files and generated launch entries are:

```text
/usr/share/macos-de
/usr/share/xsessions/macos-de.desktop
/usr/share/applications/macos-de-*.desktop
/etc/pam.d/macos-de                         (if installed)
/etc/udev/rules.d/99-macos-de-backlight.rules (if installed)
```

Use your distribution's administrator method to remove these paths. Reload
udev rules after removing the optional rule if applicable. Removing macOS-DE
does not require changing your user's group membership or other PAM policies.
