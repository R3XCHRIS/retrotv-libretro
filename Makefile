# RetroTV libretro core
#
#   make deps      fetch FFmpeg 7.1 headers (once)
#   make           cross-build build/retrotv_libretro.so + build/harness for the Pi 5
#   make test      host unit tests (parser, overlay, aspect, loudness, last channel, EPG, guide, options, onboarding, guide sources, remote, frame timing)
#   make check     host compile of everything with warnings as errors
#   make pi-test   copy to the Pi and run the headless harness against real streams

ZIG     ?= python3 -m ziglang
TARGET  ?= aarch64-linux-gnu.2.36
HOSTCC  ?= cc
FFINC   := deps/ffmpeg

WARN    := -Wall -Wextra -Wno-unused-parameter
CFLAGS  := -std=gnu11 -O2 -g $(WARN) -fPIC -fvisibility=hidden -Iinclude -I$(FFINC)
SRCS    := src/retrotv.c src/player.c src/m3u.c src/osd.c src/aspect.c src/loud.c src/lastch.c src/epg.c src/guide.c src/options.c src/onboard.c src/testcard.c src/theme.c src/gunzip.c src/plconf.c src/remote.c src/webui.c src/vsmooth.c src/ffapi.c

all: build/retrotv_libretro.so build/harness

deps: $(FFINC)/.tag
$(FFINC)/.tag:
	scripts/fetch-deps.sh

build/retrotv_libretro.so: $(SRCS) src/*.h Makefile $(FFINC)/.tag | build
	$(ZIG) cc -target $(TARGET) $(CFLAGS) -shared -o $@ $(SRCS) -ldl -lpthread -lm

build/harness: tests/harness.c | build
	$(ZIG) cc -target $(TARGET) -std=gnu11 -O2 $(WARN) -Iinclude -o $@ $< -ldl -lm

build:
	mkdir -p build

# the remote control page, embedded in the core (the generated file is committed)
src/webui.c: src/webui.html scripts/embed.py
	python3 scripts/embed.py $< $@ webui

test: | build
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_m3u tests/test_m3u.c src/m3u.c
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_osd tests/test_osd.c src/osd.c -lm
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_aspect tests/test_aspect.c src/aspect.c
	build/test_m3u
	build/test_osd
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_loud tests/test_loud.c src/loud.c -lm
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_lastch tests/test_lastch.c src/lastch.c src/m3u.c
	build/test_aspect
	build/test_loud
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_epg tests/test_epg.c src/epg.c
	build/test_lastch
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_guide tests/test_guide.c src/guide.c src/epg.c src/osd.c src/m3u.c src/theme.c -lm
	build/test_epg
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -Iinclude -fsanitize=address,undefined -o build/test_options tests/test_options.c src/options.c
	build/test_guide
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_onboard tests/test_onboard.c src/onboard.c src/testcard.c src/osd.c -lm
	build/test_options
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_epgsrc tests/test_epgsrc.c src/gunzip.c src/plconf.c src/m3u.c src/epg.c -ldl -lpthread
	build/test_onboard
	build/test_epgsrc
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_remote tests/test_remote.c src/remote.c src/m3u.c -lpthread
	build/test_remote
	$(HOSTCC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -o build/test_vsmooth tests/test_vsmooth.c src/vsmooth.c -lm
	build/test_vsmooth

check: $(FFINC)/.tag | build
	$(HOSTCC) $(CFLAGS) -Werror -fsyntax-only $(SRCS)
	$(HOSTCC) -std=gnu11 $(WARN) -Werror -Iinclude -fsyntax-only tests/harness.c
	python3 scripts/embed.py src/webui.html build/webui.check.c webui
	cmp -s src/webui.c build/webui.check.c || { echo 'src/webui.c is stale: run make src/webui.c'; exit 1; }

pi-test: all
	scripts/pi-test.sh

clean:
	rm -rf build

.PHONY: all deps test check pi-test clean
