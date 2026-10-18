CC ?= gcc
PKG_CONFIG ?= pkg-config
TARGET := screenshoot-manager
BUILD_DIR := ../../build/apps/screenshoot-manager
PREFIX ?= /usr/local
DESTDIR ?=
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 gio-2.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 gio-2.0)
CFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Wformat=2 -Wpedantic

# Runtime dependencies: maim and xclip; region capture uses slop or
# gnome-screenshot as a fallback when slop is unavailable.
.PHONY: all clean install install-assets

all: $(TARGET) install-assets

$(TARGET): screenshoot-manager.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(GTK_CFLAGS) $< -o $@ $(LDFLAGS) $(GTK_LIBS)
	mkdir -p $(BUILD_DIR)
	cp $(TARGET) $(BUILD_DIR)/

install-assets: screenshoot-manager.css logo.png version.cfg
	mkdir -p $(BUILD_DIR)
	cp screenshoot-manager.css logo.png version.cfg $(BUILD_DIR)/

install: all
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	install -m 0644 screenshoot-manager.css logo.png version.cfg \
		"$(DESTDIR)$(PREFIX)/bin/"

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
