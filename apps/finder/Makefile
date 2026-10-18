CC = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0) -I. -I../../window_manager
LIBS = $(shell pkg-config --libs gtk+-3.0)

TARGET = finder
BUILD_DIR = ../../build/apps/finder

all: $(TARGET)

$(TARGET): finder.c ../../lib/dock_settings.c ../../lib/dock_settings.h ../../cursor_engine/cursor_animator.c ../../cursor_engine/cursor_animator.h ../../window_manager/window_manager.c ../../window_manager/window_manager.h version.cfg
	$(CC) finder.c ../../lib/dock_settings.c ../../cursor_engine/cursor_animator.c ../../window_manager/window_manager.c -o $(TARGET) $(CFLAGS) $(LIBS)
	mkdir -p $(BUILD_DIR)
	cp $(TARGET) $(BUILD_DIR)/
	cp version.cfg $(BUILD_DIR)/
	cp finder.css $(BUILD_DIR)/ 2>/dev/null || true
	mkdir -p $(BUILD_DIR)/assets/apps
	cp ../../assets/apps/unknown.png $(BUILD_DIR)/assets/apps/ 2>/dev/null || true

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)

.PHONY: all clean