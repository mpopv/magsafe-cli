CFLAGS ?= -O2 -Wall -Wextra
LDLIBS = -framework IOKit -framework CoreFoundation
PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1
ZSHDIR = $(PREFIX)/share/zsh/site-functions

SOURCES = $(wildcard src/*.c)
HEADERS = $(wildcard src/*.h)

.PHONY: all test install uninstall format clean

all: build/magsafe

build/magsafe: $(SOURCES) $(HEADERS)
	@mkdir -p build
	$(CC) -std=c11 $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -o $@

test: build/magsafe
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
	xcrun clang-format -i $(SOURCES) $(HEADERS)

clean:
	rm -rf build
