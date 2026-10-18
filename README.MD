# macOS-DE

<p align="center">
  <img src="https://img.shields.io/badge/C-A8B9CC?style=for-the-badge&logo=c&logoColor=black" alt="C" />
  <img src="https://img.shields.io/badge/GTK_3-75507B?style=for-the-badge&logo=gtk&logoColor=white" alt="GTK 3" />
  <img src="https://img.shields.io/badge/X11-000000?style=for-the-badge&logo=xorg&logoColor=white" alt="X11" />
  <img src="https://img.shields.io/badge/Make-064F8C?style=for-the-badge&logo=gnu&logoColor=white" alt="Makefile" />
  <img src="https://img.shields.io/badge/Shell_Script-4EAA25?style=for-the-badge&logo=gnu-bash&logoColor=white" alt="Shell Script" />
  <img src="https://img.shields.io/badge/Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black" alt="Linux" />
  <img src="https://img.shields.io/badge/build-stable-brightgreen?style=for-the-badge" alt="Build Passed" />
  <video src="assets/github/showcase.mp4" width="100%" controls autoplay muted loop></video>
</p>

macOS-DE is a Linux desktop environment inspired by the look and interaction
patterns of macOS. It is a native C application built with GTK 3 and X11; it is
not Apple's macOS and does not use macOS system components. The project
includes its own desktop, dock, top panel, window manager, settings, and desktop
utilities.

## Features

- X11 window manager with movable, resizable, minimizable, and maximizable
  windows, configurable window controls, and rounded-corner preferences.
- Desktop with wallpaper selection, desktop icons, context menus, and
  multi-selection.
- Transparent dock and top panel, with running-application indicators.
- DE Settings for appearance, wallpaper, widgets, and window preferences.
- Built-in System Stats widget and support for installable `.widg` packages.
- Volume and display-brightness controls in the panel, with media and
  brightness keyboard keys where supported by the keyboard.
- Automatic lock screen with PAM-backed Linux account-password authentication,
  an optional lock-screen wallpaper, and screen blanking after three minutes.
- The desktop locks after the startup animation; use the current Linux account
  password to enter the session.
- Finder, Easy-File, Easy-Term, Notification Manager, and Screenshot Manager.
- Session restart command: `macos-de-restart` restarts the desktop session
  without logging out or restarting the computer.
- Session logs are written to `/var/logs/MACOS-DE-LOGS.txt` when writable;
  otherwise they are saved under `~/.local/state/macos-de/`.

## Requirements

macOS-DE currently targets Linux systems running an **X11** session. It is not
intended to run as a Wayland desktop session.

On Ubuntu or Debian, install the build dependencies with:

```sh
sudo apt update
sudo apt install build-essential make pkg-config \
  libgtk-3-dev libx11-dev libxext-dev libxft-dev libxcursor-dev \
  libgdk-pixbuf-2.0-dev libfontconfig1-dev libpango1.0-dev \
  libpam0g-dev libxss-dev
```

The desktop and window manager are built from source. Some optional controls
use system utilities when available, such as `wpctl`, `pactl`, `amixer`, or
`xbacklight`.

The lock screen uses X11's ScreenSaver extension to detect inactivity and
PAM to verify the current Linux account password. It does not suspend the
computer: after three minutes on the lock screen it blanks the display, and
input wakes it to the password prompt. The selected wallpaper remains visible
behind a tall, rounded, blurred glass panel on the left, with the clock and
password prompt together. Locking dims the display; mouse or keyboard activity
restores its pre-lock brightness while the session remains locked. The top-right
session menu remains available on the lock screen.

## Build

From the project root:

```sh
make
```

Build outputs are staged under `build/`. `make` builds the desktop, window
manager, bundled applications, and resources.

To clean generated build outputs:

```sh
make clean
```

`make test` starts the desktop in a Xephyr nested X server so it can be previewed
without replacing the current desktop session. This target may install
Xephyr with `apt` using `sudo` if it is missing. It runs the desktop
interactively; exit the nested session to finish the test.

## Install and uninstall

Install the built desktop system-wide:

```sh
sudo make install
```

The installer places macOS-DE under `/usr/share/macos-de`, adds a session entry
for the display manager, and registers its applications. If the system has a
`video` group, it also installs a udev rule for backlight access and adds the
invoking login user to that group. **Log out and back in** after installation
for group membership and brightness permissions to take effect. The install
script also installs a PAM service definition for password-based lock-screen
unlocking. It does not switch the current desktop session automatically.

To remove the system-wide installation:

```sh
sudo make uninstall
```

The uninstaller removes the installed files, session entry, and backlight udev
rule. It intentionally does not remove the user from the `video` group.

After installing, log out and select **macOS-DE** from the display manager's
session menu.

## Included applications

- **Finder** — application launcher and dock integration.
- **DE Settings** — appearance, wallpaper and widget management, and window
  manager preferences.
- **Easy-File** — file browsing and common file actions.
- **Easy-Term** — terminal emulator with macOS-DE session commands.
- **Notification Manager** — notification service and history.
- **Screenshot Manager** — screen capture utility.

## Keyboard controls

| Key | Action |
| --- | --- |
| `Print Screen` | Capture a screenshot |
| `Shift` + `Print Screen` | Select a screen region to capture |
| Volume up/down/mute keys | Adjust or mute system audio |
| Brightness up/down keys | Raise or lower display brightness |
| `Alt` + `Tab` / `Alt` + `Shift` + `Tab` | Switch between windows |
| `Alt` + `F4` | Close the active window |
| `F11` | Toggle fullscreen for the active window |

The dedicated media and brightness keys depend on the key symbols exposed by
the keyboard and X11. Brightness control also requires a supported backlight
device and write permission; the installer configures the common `video`-group
setup when available.

The lock screen starts automatically after three minutes of inactivity.
Change this in **DE Settings → Preferences → Lock screen** to 15 or 30 minutes,
1 or 2 hours, or **Never**. The wallpaper used by the lock screen can be
selected separately under **DE Settings → Wallpaper And Widgets → Lock screen**;
clearing its selection makes it follow the desktop wallpaper. Use **DE Settings
→ Dev → Lock screen now** to open it immediately for testing.

## Widgets

The built-in System Stats widget can be toggled in DE Settings. Third-party
widgets are `.widg` ZIP archives containing a `config.cfg`, a prebuilt Linux
executable, and any required assets. The executable is launched as the current
user and **is not sandboxed**, so only install packages you trust.

See [widgets/README.md](widgets/README.md) for the package layout, manifest
format, and installation steps.

## Project layout

```text
apps/                 Desktop applications and their build files
assets/               Icons, wallpaper, cursor, and other resources
css/                  Desktop and dock stylesheets
desktop_engine/       Desktop icons and interaction
dock/                 Dock implementation
lib/                  System controls, window manager, and shared code
scripts/              Install, uninstall, restart, and packaging helpers
wallpaper_engine/     Wallpaper handling
widget_engine/        Widget lifecycle and desktop integration
widgets/              Built-in widgets and widget-author documentation
window_manager/       Desktop window-management integration
```

## Report bugs and get help !

Report bugs and issues via the github issues tab and help us fix them !
whit your help some day we will have no bugs and issues !

## Contributing

Keep changes focused and follow the existing C, GTK, and Makefile patterns.
Build the project with `make` after changes. For widget packages, follow the
separate [widget guide](widgets/README.md). Contributor credits and project
ownership information are in [contribute.md](contribute.md).