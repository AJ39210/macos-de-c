CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0)
WARNINGS := -Wall -Wextra -Wpedantic

TARGET := Easy-File
BUILD_DIR := ../../build/apps/Easy-File
ICON_ASSETS := $(wildcard icons/*.png)

.PHONY: all clean

all: $(TARGET) stage

$(TARGET): Easy-File.c
	$(CC) $(CFLAGS) $(WARNINGS) $(GTK_CFLAGS) $< -o $@ $(GTK_LIBS)

stage: $(TARGET) Easy-File.css logo.png version.cfg $(ICON_ASSETS)
	mkdir -p $(BUILD_DIR)/icons
	cp $(TARGET) Easy-File.css logo.png version.cfg $(BUILD_DIR)/
	cp icons/*.png $(BUILD_DIR)/icons/

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
