# macOS-DE

<p align="center">
  <img src="https://img.shields.io/badge/C-A8B9CC?style=for-the-badge&logo=c&logoColor=black" alt="C" />
  <img src="https://img.shields.io/badge/GTK_3-75507B?style=for-the-badge&logo=gtk&logoColor=white" alt="GTK 3" />
  <img src="https://img.shields.io/badge/X11-000000?style=for-the-badge&logo=xorg&logoColor=white" alt="X11" />
  <img src="https://img.shields.io/badge/Make-064F8C?style=for-the-badge&logo=gnu&logoColor=white" alt="Makefile" />
  <img src="https://img.shields.io/badge/Shell_Script-4EAA25?style=for-the-badge&logo=gnu-bash&logoColor=white" alt="Shell Script" />
  <img src="https://img.shields.io/badge/Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black" alt="Linux" />
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
  libgdk-pixbuf-2.0-dev libfontconfig1-dev libpango1.0-dev
```

The desktop and window manager are built from source. Some optional controls
use system utilities when available, such as `wpctl`, `pactl`, `amixer`, or
`xbacklight`.

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
script does not switch the current desktop session automatically.

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

## Tree of the Project

'''text
.
├── apps
│   ├── DE-settings
│   │   ├── DE-settings.c
│   │   ├── DE-settings.css
│   │   ├── logo.png
│   │   ├── Makefile
│   │   └── version.cfg
│   ├── Easy-File
│   │   ├── Easy-File.c
│   │   ├── Easy-File.css
│   │   ├── icons
│   │   │   ├── assembly.png
│   │   │   ├── bin.png
│   │   │   ├── cfg.png
│   │   │   ├── c#.png
│   │   │   ├── c++.png
│   │   │   ├── c.png
│   │   │   ├── file.png
│   │   │   ├── folder.png
│   │   │   ├── png.png
│   │   │   ├── software.png
│   │   │   ├── unknown.png
│   │   │   ├── widg.png
│   │   │   └── zip.png
│   │   ├── logo.png
│   │   ├── Makefile
│   │   └── version.cfg
│   ├── Easy-Term
│   │   ├── Easy-Term.c
│   │   ├── Easy-Term.css
│   │   ├── logo.png
│   │   ├── Makefile
│   │   └── version.cfg
│   ├── finder
│   │   ├── finder.c
│   │   ├── finder.css
│   │   ├── Makefile
│   │   └── version.cfg
│   ├── Notification-manager
│   │   ├── logo.png
│   │   ├── Makefile
│   │   ├── Notification-manager.c
│   │   ├── Notification-manager.css
│   │   └── version.cfg
│   └── screenshoot-manager
│       ├── logo.png
│       ├── Makefile
│       ├── screenshoot-manager.c
│       ├── screenshoot-manager.css
│       └── version.cfg
├── assets
│   ├── apps
│   │   ├── finder.png
│   │   └── unknown.png
│   ├── github
│   ├── system
│   │   ├── cursor
│   │   │   └── default1
│   │   │       ├── Alternate.png
│   │   │       ├── Bonus-Move.png
│   │   │       ├── Bonus-Text.png
│   │   │       ├── Bonus-Unavailable.png
│   │   │       ├── Busy
│   │   │       │   ├── Busy_page_01.png
│   │   │       │   ├── Busy_page_02.png
│   │   │       │   ├── Busy_page_03.png
│   │   │       │   ├── Busy_page_04.png
│   │   │       │   ├── Busy_page_05.png
│   │   │       │   ├── Busy_page_06.png
│   │   │       │   ├── Busy_page_07.png
│   │   │       │   ├── Busy_page_08.png
│   │   │       │   ├── Busy_page_09.png
│   │   │       │   ├── Busy_page_10.png
│   │   │       │   ├── Busy_page_11.png
│   │   │       │   ├── Busy_page_12.png
│   │   │       │   ├── Busy_page_13.png
│   │   │       │   ├── Busy_page_14.png
│   │   │       │   └── Busy_page_15.png
│   │   │       ├── Diagonal-Resize-1.png
│   │   │       ├── Diagonal-Resize-2.png
│   │   │       ├── Handwriting.png
│   │   │       ├── Help.png
│   │   │       ├── Horizontal-Resize.png
│   │   │       ├── Link.png
│   │   │       ├── Move.png
│   │   │       ├── Normal.png
│   │   │       ├── Precision Select.png
│   │   │       ├── Text Select.png
│   │   │       ├── Unavailable.png
│   │   │       ├── Vertical Resize.png
│   │   │       └── Working
│   │   │           ├── Working_page_01.png
│   │   │           ├── Working_page_02.png
│   │   │           ├── Working_page_03.png
│   │   │           ├── Working_page_04.png
│   │   │           ├── Working_page_05.png
│   │   │           ├── Working_page_06.png
│   │   │           ├── Working_page_07.png
│   │   │           ├── Working_page_08.png
│   │   │           ├── Working_page_09.png
│   │   │           ├── Working_page_10.png
│   │   │           ├── Working_page_11.png
│   │   │           ├── Working_page_12.png
│   │   │           ├── Working_page_13.png
│   │   │           ├── Working_page_14.png
│   │   │           └── Working_page_15.png
│   │   ├── fonts
│   │   │   └── SF
│   │   │       ├── SFPRODISPLAYBLACKITALIC.OTF
│   │   │       ├── SFPRODISPLAYBOLD.OTF
│   │   │       ├── SFPRODISPLAYHEAVYITALIC.OTF
│   │   │       ├── SFPRODISPLAYLIGHTITALIC.OTF
│   │   │       ├── SFPRODISPLAYMEDIUM.OTF
│   │   │       ├── SFPRODISPLAYREGULAR.OTF
│   │   │       ├── SFPRODISPLAYSEMIBOLDITALIC.OTF
│   │   │       ├── SFPRODISPLAYTHINITALIC.OTF
│   │   │       └── SFPRODISPLAYULTRALIGHTITALIC.OTF
│   │   ├── icon.png
│   │   ├── icons
│   │   │   ├── battery
│   │   │   │   ├── battery-full-charged-symbolic.png
│   │   │   │   ├── battery-level-0-charging-symbolic.png
│   │   │   │   ├── battery-level-0-symbolic.png
│   │   │   │   ├── battery-level-100-charged-symbolic.png
│   │   │   │   ├── battery-level-100-symbolic.png
│   │   │   │   ├── battery-level-10-charging-symbolic.png
│   │   │   │   ├── battery-level-10-symbolic.png
│   │   │   │   ├── battery-level-20-charging-symbolic.png
│   │   │   │   ├── battery-level-20-symbolic.png
│   │   │   │   ├── battery-level-30-charging-symbolic.png
│   │   │   │   ├── battery-level-30-symbolic.png
│   │   │   │   ├── battery-level-40-charging-symbolic.png
│   │   │   │   ├── battery-level-40-symbolic.png
│   │   │   │   ├── battery-level-50-charging-symbolic.png
│   │   │   │   ├── battery-level-50-symbolic.png
│   │   │   │   ├── battery-level-60-charging-symbolic.png
│   │   │   │   ├── battery-level-60-symbolic.png
│   │   │   │   ├── battery-level-70-charging-symbolic.png
│   │   │   │   ├── battery-level-70-symbolic.png
│   │   │   │   ├── battery-level-80-charging-symbolic.png
│   │   │   │   ├── battery-level-80-symbolic.png
│   │   │   │   ├── battery-level-90-charging-symbolic.png
│   │   │   │   └── battery-level-90-symbolic.png
│   │   │   ├── ethernet
│   │   │   │   ├── network-wireless-offline-symbolic.png
│   │   │   │   ├── network-wireless-signal-excellent-symbolic.png
│   │   │   │   ├── network-wireless-signal-good-symbolic.png
│   │   │   │   ├── network-wireless-signal-none-symbolic.png
│   │   │   │   ├── network-wireless-signal-ok-symbolic.png
│   │   │   │   ├── network-wireless-signal-weak-symbolic.png
│   │   │   │   └── wire-pluged-in.png
│   │   │   └── other
│   │   │       ├── logaut.png
│   │   │       ├── reboot.png
│   │   │       └── shutdown.png
│   │   ├── start-up
│   │   │   ├── 00.png
│   │   │   ├── 01.png
│   │   │   ├── 02.png
│   │   │   ├── 03.png
│   │   │   ├── 04.png
│   │   │   ├── 05.png
│   │   │   ├── 06.png
│   │   │   ├── 07.png
│   │   │   ├── 08.png
│   │   │   ├── 09.png
│   │   │   ├── 10.png
│   │   │   ├── 11.png
│   │   │   ├── 12.png
│   │   │   ├── 13.png
│   │   │   ├── 14.png
│   │   │   ├── 15.png
│   │   │   ├── 16.png
│   │   │   ├── 17.png
│   │   │   ├── 18.png
│   │   │   ├── 19.png
│   │   │   ├── 20.png
│   │   │   ├── 21.png
│   │   │   ├── 22.png
│   │   │   ├── 23.png
│   │   │   ├── 24.png
│   │   │   ├── 25.png
│   │   │   ├── 26.png
│   │   │   ├── 27.png
│   │   │   ├── 28.png
│   │   │   ├── 29.png
│   │   │   ├── 30.png
│   │   │   ├── 31.png
│   │   │   ├── 32.png
│   │   │   ├── 33.png
│   │   │   ├── 34.png
│   │   │   ├── 35.png
│   │   │   ├── 36.png
│   │   │   ├── 37.png
│   │   │   ├── 38.png
│   │   │   ├── 39.png
│   │   │   ├── 40.png
│   │   │   ├── 41.png
│   │   │   ├── 42.png
│   │   │   ├── 43.png
│   │   │   ├── 44.png
│   │   │   ├── 45.png
│   │   │   ├── 46.png
│   │   │   ├── 47.png
│   │   │   ├── 48.png
│   │   │   ├── 49.png
│   │   │   ├── 50.png
│   │   │   ├── 51.png
│   │   │   ├── 52.png
│   │   │   ├── 53.png
│   │   │   ├── 54.png
│   │   │   ├── 55.png
│   │   │   ├── 56.png
│   │   │   ├── 57.png
│   │   │   ├── 58.png
│   │   │   ├── 59.png
│   │   │   ├── error_screen.png
│   │   │   ├── please_wait.png
│   │   │   └── start-up.mp3
│   │   └── volume
│   │       ├── volume_000.png
│   │       ├── volume_005.png
│   │       ├── volume_010.png
│   │       ├── volume_015.png
│   │       ├── volume_020.png
│   │       ├── volume_025.png
│   │       ├── volume_030.png
│   │       ├── volume_035.png
│   │       ├── volume_040.png
│   │       ├── volume_045.png
│   │       ├── volume_050.png
│   │       ├── volume_055.png
│   │       ├── volume_060.png
│   │       ├── volume_065.png
│   │       ├── volume_070.png
│   │       ├── volume_075.png
│   │       ├── volume_080.png
│   │       ├── volume_085.png
│   │       ├── volume_090.png
│   │       ├── volume_095.png
│   │       ├── volume_100.png
│   │       └── volume_mute.png
│   └── wallpaper
│       ├── mac2.png
│       ├── mac3.png
│       ├── mac4.png
│       ├── mac5.png
│       ├── mac6.png
│       └── mac.png
├── contribute.md
├── css
│   ├── dock.css
│   ├── macos.css
│   └── style.css
├── cursor_engine
│   ├── cursor_animator.c
│   └── cursor_animator.h
├── desktop_engine
│   ├── desktop_icons.c
│   └── desktop_icons.h
├── dock
│   ├── dock.c
│   └── dock.h
├── lib
│   ├── client_decorations.c
│   ├── client_decorations.h
│   ├── dock_settings.c
│   ├── dock_settings.h
│   ├── macoswm.c
│   ├── macoswm.h
│   ├── system_status.c
│   ├── system_status.h
│   └── wm_main.c
├── main.c
├── makefile
├── misc
│   ├── crash_assist
│   │   └── macos-de-session
│   └── garbage_collector
│       ├── garbage_collector.c
│       └── garbage_collector.h
├── README.MD
├── scripts
│   ├── 99-macos-de-backlight.rules
│   ├── extract-widget.py
│   ├── install.sh
│   ├── macos-de-restart
│   └── uninstall.sh
├── startup_engine
│   ├── startup.c
│   └── startup.h
├── wallpaper_engine
│   ├── wallpaper.c
│   └── wallpaper.h
├── widget_engine
│   ├── widget_manager.c
│   └── widget_manager.h
├── widgets
│   ├── README.md
│   └── stats
│       ├── config.cfg
│       ├── stats.c
│       ├── stats.h
│       └── style.css
└── window_manager
    ├── window_manager.c
    └── window_manager.c

41-Dirs
267-Files
'''

## Report bugs and get help !

Report bugs and issues via the github issues tab and help us fix them !
whit your help some day we will have no bugs and issues !

## Contributing

Keep changes focused and follow the existing C, GTK, and Makefile patterns.
Build the project with `make` after changes. For widget packages, follow the
separate [widget guide](widgets/README.md). Contributor credits and project
ownership information are in [contribute.md](contribute.md).