# RetroTV libretro core
#
#   make deps      fetch FFmpeg 7.1 headers (once)
#   make           cross-build build/retrotv_libretro.so + build/harness for the Pi 5
#   make test      host unit tests (parser, overlay, aspect, loudness, last channel, EPG, guide)
#   make check     host compile of everything with warnings as errors
#   make pi-test   copy to the Pi and run the headless harness against real streams

ZIG     ?= python3 -m ziglang
TARGET  ?= aarch64-linux-gnu.2.36
HOSTCC  ?= cc
FFINC   := deps/ffmpeg

WARN    := -Wall -Wextra -Wno-unused-parameter
CFLAGS  := -std=gnu11 -O2 -g $(WARN) -fPIC -fvisibility=hidden -Iinclude -I$(FFINC)
SRCS    := src/retrotv.c src/player.c src/m3u.c src/osd.c src/aspect.c src/loud.c src/lastch.c src/epg.c src/guide.c src/ffapi.c

all: build/retrotv_libretro.so build/harness

deps: $(FFINC)/.tag
$(FFINC)/.tag:
	scripts/fetch-deps.sh

build/retrotv_libretro.so: $(SRCS) src/*.h $(FFINC)/.tag | build
	$(ZIG) cc -target $(TARGET) $(CFLAGS) -shared -o $@ $(SRCS) -ldl -lpthread -lm

build/harness: tests/harness.c | build
	$(ZIG) cc -target $(TARGET) -std=gnu11 -O2 $(WARN) -Iinclude -o $@ $< -ldl -lm

build:
	mkdir -p build

test: | build
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_m3u tests/test_m3u.c src/m3u.c
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_osd tests/test_osd.c src/osd.c
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_aspect tests/test_aspect.c src/aspect.c
	build/test_m3u
	build/test_osd
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_loud tests/test_loud.c src/loud.c -lm
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_lastch tests/test_lastch.c src/lastch.c src/m3u.c
	build/test_aspect
	build/test_loud
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_epg tests/test_epg.c src/epg.c
	build/test_lastch
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_guide tests/test_guide.c src/guide.c src/epg.c src/osd.c src/m3u.c
	build/test_epg
	build/test_guide

check: $(FFINC)/.tag | build
	$(HOSTCC) $(CFLAGS) -Werror -fsyntax-only $(SRCS)
	$(HOSTCC) -std=gnu11 $(WARN) -Werror -Iinclude -fsyntax-only tests/harness.c

pi-test: all
	scripts/pi-test.sh

clean:
	rm -rf build

.PHONY: all deps test check pi-test clean
