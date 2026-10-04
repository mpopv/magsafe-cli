CC = xcrun clang
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror
LDLIBS = -framework IOKit -framework CoreFoundation
PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin

SOURCES = $(wildcard src/*.c)
HEADERS = $(wildcard src/*.h)

.PHONY: all test install uninstall format clean

all: build/magsafe

build/magsafe: $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SOURCES) $(LDLIBS) -o $@

test: build/magsafe
	sh tests/cli.sh build/magsafe

install: build/magsafe
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 build/magsafe "$(DESTDIR)$(BINDIR)/magsafe"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/magsafe"

format:
	xcrun clang-format -i $(SOURCES) $(HEADERS)

clean:
	rm -rf build
