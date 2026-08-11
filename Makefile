CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror=format-security
LDFLAGS ?=
LDLIBS += -lz

PROGRAM := droidian-apex
SOURCES := \
	src/main.c \
	src/util.c \
	src/manifest.c \
	src/zip.c \
	src/discover.c \
	src/mount.c \
	src/container.c \
	src/diagnose.c
OBJECTS := $(SOURCES:.c=.o)

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
MANDIR ?= $(PREFIX)/share/man

.PHONY: all clean check install

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c src/apex-manager.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/test-manifest: tests/test_manifest.c src/manifest.c src/util.c src/apex-manager.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
		tests/test_manifest.c src/manifest.c src/util.c

check: $(PROGRAM) tests/test-manifest
	./tests/test-manifest
	./tests/test_cli.sh

install: $(PROGRAM)
	install -D -m 0755 $(PROGRAM) $(DESTDIR)$(SBINDIR)/$(PROGRAM)
	install -D -m 0644 droidian-apex.8 \
		$(DESTDIR)$(MANDIR)/man8/droidian-apex.8

clean:
	rm -f $(PROGRAM) $(OBJECTS) tests/test-manifest
