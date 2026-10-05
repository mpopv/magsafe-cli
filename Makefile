CFLAGS ?= -O2 -Wall -Wextra
LDLIBS = -framework IOKit -framework CoreFoundation -framework CoreAudio -framework Foundation
PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1
ZSHDIR = $(PREFIX)/share/zsh/site-functions

C_SOURCES = $(wildcard src/*.c)
OBJC_SOURCES = $(wildcard src/*.m)
HEADERS = $(wildcard src/*.h)
OBJECTS = $(C_SOURCES:src/%.c=build/%.o) $(OBJC_SOURCES:src/%.m=build/%.o)
# The parts that need no hardware, audio, or sudo, for the unit tests.
UNIT_SOURCES = tests/unit.c src/analysis.c src/stream.c src/timer.c src/morse.c

.PHONY: all test install uninstall format clean

all: build/magsafe

build/%.o: src/%.c $(HEADERS)
	@mkdir -p build
	$(CC) -std=c11 $(CFLAGS) -c $< -o $@

build/%.o: src/%.m $(HEADERS)
	@mkdir -p build
	$(CC) -fobjc-arc $(CFLAGS) -c $< -o $@

build/magsafe: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@

build/unit-test: $(UNIT_SOURCES) $(HEADERS)
	@mkdir -p build
	$(CC) -std=c11 $(CFLAGS) $(LDFLAGS) -Isrc $(UNIT_SOURCES) -o $@

# Development tool, not installed: see tools/analyze.m.
build/analyze: tools/analyze.m src/analysis.c src/analysis.h
	@mkdir -p build
	$(CC) -fobjc-arc $(CFLAGS) $(LDFLAGS) -Isrc tools/analyze.m src/analysis.c \
	  -framework AudioToolbox -framework Foundation -o $@

test: build/magsafe build/unit-test
	build/unit-test
	sh tests/cli.sh build/magsafe

install: build/magsafe
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)" "$(DESTDIR)$(ZSHDIR)"
	install -m 755 build/magsafe "$(DESTDIR)$(BINDIR)/magsafe"
	install -m 644 man/magsafe.1 "$(DESTDIR)$(MANDIR)/magsafe.1"
	install -m 644 completions/_magsafe "$(DESTDIR)$(ZSHDIR)/_magsafe"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/magsafe" "$(DESTDIR)$(MANDIR)/magsafe.1" \
	  "$(DESTDIR)$(ZSHDIR)/_magsafe"

format:
	xcrun clang-format -i $(C_SOURCES) $(OBJC_SOURCES) $(HEADERS) tests/*.c tools/*.m

clean:
	rm -rf build
