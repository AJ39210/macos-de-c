CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 gio-2.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 gio-2.0)
TARGET := Notification-manager
BUILD_DIR := ../../build/apps/Notification-manager

.PHONY: all resources clean

all: $(TARGET) resources

$(TARGET): Notification-manager.c
	$(CC) $(CFLAGS) -Wall -Wextra -Wpedantic $(GTK_CFLAGS) Notification-manager.c -o $@ $(GTK_LIBS)

resources: $(TARGET) Notification-manager.css logo.png version.cfg
	mkdir -p $(BUILD_DIR)
	cp $(TARGET) Notification-manager.css logo.png version.cfg $(BUILD_DIR)/

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
