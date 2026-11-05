CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -std=gnu11
BUILD = build

all: $(BUILD)/wsock32.dll

$(BUILD)/wsock32.def $(BUILD)/stubs.S $(BUILD)/exports.h: gen_exports.py | $(BUILD)
	python3 gen_exports.py $(BUILD)

$(BUILD)/wsock32.dll: wsock32.c $(BUILD)/wsock32.def $(BUILD)/stubs.S $(BUILD)/exports.h
	$(CC) $(CFLAGS) -I$(BUILD) -shared -o $@ wsock32.c $(BUILD)/stubs.S $(BUILD)/wsock32.def \
		-static-libgcc -Wl,--enable-stdcall-fixup -s

install: $(BUILD)/wsock32.dll
	cp $(BUILD)/wsock32.dll ../hlsw/wsock32.dll

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all install clean
