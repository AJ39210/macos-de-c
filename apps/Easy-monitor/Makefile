CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0)
TARGET := Easy-monitor
BUILD_DIR := ../../build/apps/Easy-monitor

.PHONY: all resources clean

all: $(TARGET) resources

$(TARGET): Easy-monitor.c
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(GTK_CFLAGS) $< -o $@ $(GTK_LIBS)

resources: $(TARGET) Easy-monitor.css version.cfg
	mkdir -p $(BUILD_DIR)
	cp $(TARGET) Easy-monitor.css version.cfg $(BUILD_DIR)/

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
