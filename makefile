.SILENT:

CC = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0) -I. -Iwallpaper_engine -Icursor_engine -Istartup_engine -Idock -Iwindow_manager
LIBS = $(shell pkg-config --libs gtk+-3.0) $(shell pkg-config --libs x11 pangoft2) -lXss -lpam
X11_CFLAGS = $(shell pkg-config --cflags x11)
X11_LIBS = $(shell pkg-config --libs x11)
WM_CFLAGS = $(shell pkg-config --cflags x11 xext xft xcursor gdk-pixbuf-2.0 fontconfig glib-2.0) -Wall -Wextra
WM_LIBS = $(shell pkg-config --libs x11 xext xft xcursor gdk-pixbuf-2.0 fontconfig glib-2.0) -lm

BUILD_DIR = build
ASSETS_DIR = assets
CSS_DIR = css

CORE_SRCS = main.c \
            lib/system_status.c \
            lib/lock_screen.c \
            lib/dock_settings.c \
            $(wildcard wallpaper_engine/*.c) \
            $(wildcard desktop_engine/*.c) \
            $(wildcard widget_engine/*.c) \
            $(wildcard widgets/stats/*.c) \
            $(wildcard cursor_engine/*.c) \
            $(wildcard startup_engine/*.c) \
            $(wildcard misc/garbage_collector/*.c) \
            $(wildcard dock/*.c) \
            $(wildcard window_manager/*.c)

all: dirs core wm apps copy_resources
	@echo "Build successful. Run 'make test' to preview."

dirs:
	@mkdir -p $(BUILD_DIR)/bin
	@mkdir -p $(BUILD_DIR)/assets
	@mkdir -p $(BUILD_DIR)/css
	@mkdir -p $(BUILD_DIR)/apps/finder
	@mkdir -p $(BUILD_DIR)/applications
	@mkdir -p $(BUILD_DIR)/sessions
	@mkdir -p $(BUILD_DIR)/lib
	@mkdir -p $(BUILD_DIR)/widgets/stats

core:
	@$(CC) $(CORE_SRCS) -o $(BUILD_DIR)/bin/macos-de $(CFLAGS) $(LIBS)

wm:
	@$(CC) -c lib/macoswm.c -o $(BUILD_DIR)/lib/macoswm.o $(WM_CFLAGS)
	@$(CC) -c lib/client_decorations.c -o $(BUILD_DIR)/lib/client_decorations.o $(X11_CFLAGS) -Wall -Wextra
	@ar rcs $(BUILD_DIR)/lib/libmacoswm.a $(BUILD_DIR)/lib/macoswm.o $(BUILD_DIR)/lib/client_decorations.o
	@$(CC) lib/wm_main.c $(BUILD_DIR)/lib/libmacoswm.a -o $(BUILD_DIR)/bin/macos-wm $(WM_CFLAGS) $(WM_LIBS)

apps:
	@$(MAKE) -s -C apps/finder
	@$(MAKE) -s -C apps/DE-settings
	@$(MAKE) -s -C apps/Notification-manager
	@$(MAKE) -s -C apps/screenshoot-manager
	@$(MAKE) -s -C apps/Easy-File
	@$(MAKE) -s -C apps/Easy-Term

copy_resources: dirs
	@if [ -d "$(ASSETS_DIR)" ]; then cp -r $(ASSETS_DIR)/* $(BUILD_DIR)/assets/; fi
	@if [ -d "$(CSS_DIR)" ]; then cp -r $(CSS_DIR)/* $(BUILD_DIR)/css/; fi
	@cp widgets/stats/config.cfg widgets/stats/style.css \
		widgets/stats/stats.c widgets/stats/stats.h $(BUILD_DIR)/widgets/stats/
	@cp misc/crash_assist/macos-de-session $(BUILD_DIR)/bin/macos-de-session
	@cp scripts/macos-de-restart $(BUILD_DIR)/bin/macos-de-restart
	@cp scripts/extract-widget.py $(BUILD_DIR)/bin/extract-widget.py
	@chmod +x $(BUILD_DIR)/bin/macos-de-session \
		$(BUILD_DIR)/bin/macos-de-restart $(BUILD_DIR)/bin/extract-widget.py
	@printf "[Desktop Entry]\nName=macOS-DE\nComment=macOS-style X11 desktop environment and window manager\nExec=/usr/share/macos-de/bin/macos-de-session\nType=Application\nDesktopNames=macOS-DE\n" > $(BUILD_DIR)/sessions/macos-de.desktop
	@printf "[Desktop Entry]\nName=DE Settings\nComment=Configure macOS-DE appearance and system preferences\nExec=/usr/share/macos-de/apps/DE-settings/DE-settings\nIcon=/usr/share/macos-de/apps/DE-settings/logo.png\nTerminal=false\nType=Application\nCategories=Settings;DesktopSettings;\n" > $(BUILD_DIR)/applications/macos-de-settings.desktop
	@printf "[Desktop Entry]\nName=Screenshot Manager\nComment=Capture and manage desktop screenshots\nExec=/usr/share/macos-de/apps/screenshoot-manager/screenshoot-manager\nIcon=/usr/share/macos-de/apps/screenshoot-manager/logo.png\nTerminal=false\nType=Application\nCategories=Utility;\n" > $(BUILD_DIR)/applications/macos-de-screenshot-manager.desktop
	@printf "[Desktop Entry]\nName=Notification Manager\nComment=macOS-DE notification history and service\nExec=/usr/share/macos-de/apps/Notification-manager/Notification-manager\nIcon=/usr/share/macos-de/apps/Notification-manager/logo.png\nTerminal=false\nType=Application\nCategories=System;\n" > $(BUILD_DIR)/applications/macos-de-notification-manager.desktop
	@printf "[Desktop Entry]\nName=Easy-File\nComment=Browse files in macOS-DE\nExec=/usr/share/macos-de/apps/Easy-File/Easy-File\nIcon=/usr/share/macos-de/apps/Easy-File/logo.png\nTerminal=false\nType=Application\nCategories=Utility;FileTools;\n" > $(BUILD_DIR)/applications/macos-de-easy-file.desktop
	@printf "[Desktop Entry]\nName=Easy-Term\nComment=Terminal for macOS-DE\nExec=/usr/share/macos-de/apps/Easy-Term/Easy-Term\nIcon=/usr/share/macos-de/apps/Easy-Term/logo.png\nTerminal=false\nType=Application\nCategories=System;TerminalEmulator;\n" > $(BUILD_DIR)/applications/macos-de-easy-term.desktop

test: all
	@which Xephyr > /dev/null || sudo apt-get install -y xserver-xephyr
	@set -e; Xephyr :1 -screen 1280x800 -ac >/dev/null 2>&1 & xephyr_pid=$$!; \
		cleanup() { kill "$$xephyr_pid" 2>/dev/null || true; wait "$$xephyr_pid" 2>/dev/null || true; }; \
		trap cleanup EXIT; trap 'exit 130' INT; trap 'exit 143' HUP TERM; \
		sleep 1; DISPLAY=:1 $(BUILD_DIR)/bin/macos-de-session

install: all
	@sudo sh scripts/install.sh

uninstall:
	@sudo sh scripts/uninstall.sh

clean:
	@rm -rf $(BUILD_DIR)
	@$(MAKE) -s -C apps/finder clean
	@$(MAKE) -s -C apps/DE-settings clean
	@$(MAKE) -s -C apps/Notification-manager clean
	@$(MAKE) -s -C apps/screenshoot-manager clean
	@$(MAKE) -s -C apps/Easy-File clean
	@$(MAKE) -s -C apps/Easy-Term clean
	@echo "Cleaned."

.PHONY: all dirs core wm apps copy_resources test install uninstall clean