CC = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0) -Wall -Wextra -Wpedantic
LIBS = $(shell pkg-config --libs gtk+-3.0) -lm

TARGET = DE-settings
BUILD_DIR = ../../build/apps/DE-settings

all: $(TARGET)

$(TARGET): DE-settings.c DE-settings.css logo.png version.cfg
	$(CC) DE-settings.c -o $(TARGET) $(CFLAGS) $(LIBS)
	mkdir -p $(BUILD_DIR)
	cp $(TARGET) DE-settings.css logo.png version.cfg $(BUILD_DIR)/

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)

.PHONY: all clean
