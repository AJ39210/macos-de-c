CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS += -std=c11 -O2 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk+-3.0) -ldl

TARGET = Easy-Term
BUILD_DIR = ../../build/apps/Easy-Term

.PHONY: all clean

all: $(TARGET)
	mkdir -p $(BUILD_DIR)
	cp $(TARGET) Easy-Term.css logo.png version.cfg $(BUILD_DIR)/

$(TARGET): Easy-Term.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
