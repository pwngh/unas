# @pwngh/unas
#
# Copyright (c) Preston Neal
#
# This source code is licensed under the MIT license found in the
# LICENSE.md file in the root directory of this source tree.
#
# @license MIT

# Makefile — POSIX make. Run ./configure first (it writes config.mk).
#
# The `.POSIX:` first target engages strict POSIX semantics in BSD make
# and gmake alike. No GNU extensions, no autotools, no CMake. The
# Makefile never invokes ./configure — that is a deliberate manual step.
#
# Artifacts:
#   libunas.a   the engine (jsonw, http, net, fsapi, random) — the
#               C99/POSIX core, reusable and unit-testable.
#   unasd       the daemon: jails a share root, serves the HTTP file API.
#
#   ./configure && make && make test

.POSIX:

# Default install prefix. Assigned BEFORE the include so a PREFIX
# written into config.mk by `./configure --prefix=DIR` overrides it
# (later assignments win); `make PREFIX=DIR install` overrides both.
PREFIX = /usr/local

include config.mk

CFLAGS   = $(CONFIG_CFLAGS)
CPPFLAGS = $(CONFIG_CPPFLAGS)
LDFLAGS  = $(CONFIG_LDFLAGS)
LDLIBS   = $(CONFIG_LDLIBS)

AR       = ar
ARFLAGS  = -rcs

# ---------------------------------------------------------------------
# Engine library (libunas.a). Member order is irrelevant: the `s` in
# ARFLAGS writes the archive symbol table. compat/random.o has its own
# rule below because it compiles WITHOUT _POSIX_C_SOURCE (see configure).
# ---------------------------------------------------------------------
LIB_OBJS = src/compat/random.o \
           src/core/jsonw.o \
           src/core/http.o \
           src/core/net.o \
           src/core/fsapi.o

DAEMON_OBJS = src/core/unasd.o

all: unasd

# Rebuilt from scratch each time. An archive (.a) is just a bundle of
# compiled .o files; `ar -r` edits that bundle in place, adding or
# replacing members but never noticing one that is gone. So if a module
# is renamed or dropped, its old .o would linger inside. Deleting the
# archive first (the `rm -f $@` below) guarantees only today's members
# are in it.
libunas.a: $(LIB_OBJS)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $(LIB_OBJS)

unasd: $(DAEMON_OBJS) libunas.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(DAEMON_OBJS) libunas.a $(LDLIBS)

# ---------------------------------------------------------------------
# Engine objects. Explicit rules (not suffix rules): sources live under
# src/*/ and explicit deps drive correct incremental rebuilds. Every
# object also depends on config.mk: re-running ./configure must rebuild
# the world, never link objects compiled with stale flags.
#
# compat/ compiles WITHOUT _POSIX_C_SOURCE so the platform RNG is visible
# (COMPAT_CPPFLAGS is the only divergence — same $(CFLAGS) as the core);
# it links cleanly with the POSIX core.
# ---------------------------------------------------------------------
src/compat/random.o: src/compat/random.c src/compat/random.h config.mk
	$(CC) $(CFLAGS) $(COMPAT_CPPFLAGS) -c -o $@ src/compat/random.c

src/core/jsonw.o: src/core/jsonw.c src/core/jsonw.h config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/core/jsonw.c

src/core/http.o: src/core/http.c src/core/http.h config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/core/http.c

src/core/net.o: src/core/net.c src/core/net.h config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/core/net.c

src/core/fsapi.o: src/core/fsapi.c src/core/fsapi.h src/core/jsonw.h config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/core/fsapi.c

src/core/unasd.o: src/core/unasd.c src/core/http.h src/core/net.h src/core/fsapi.h src/core/jsonw.h src/compat/random.h config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/core/unasd.c

# ---------------------------------------------------------------------
# Tests: C unit binaries linked against libunas.a, a static doc-drift guard
# (check-docs.sh), and a POSIX sh driver that curls the running daemon.
# `make test` fails if any assert fails.
# ---------------------------------------------------------------------
API_TESTS = tests/api/test_pathjail \
            tests/api/test_http \
            tests/api/test_jsonw

tests/api/test_pathjail: tests/api/test_pathjail.c libunas.a config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ tests/api/test_pathjail.c libunas.a $(LDLIBS)

tests/api/test_http: tests/api/test_http.c libunas.a config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ tests/api/test_http.c libunas.a $(LDLIBS)

tests/api/test_jsonw: tests/api/test_jsonw.c libunas.a config.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ tests/api/test_jsonw.c libunas.a $(LDLIBS)

test: unasd $(API_TESTS)
	./tests/api/test_pathjail
	./tests/api/test_http
	./tests/api/test_jsonw
	sh ./tests/check-docs.sh
	./tests/run.sh

# POSIX utilities only: install(1) is a BSD/GNU extension, and the SysV
# variant on Solaris/AIX takes incompatible options.
install: unasd
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp unasd $(DESTDIR)$(PREFIX)/bin/unasd
	chmod 755 $(DESTDIR)$(PREFIX)/bin/unasd

clean:
	rm -f unasd libunas.a \
	      src/core/*.o src/compat/*.o \
	      $(API_TESTS)

distclean: clean
	rm -f config.mk config.mk.tmp config.log

.PHONY: all test install clean distclean
