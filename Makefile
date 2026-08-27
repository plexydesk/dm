# Copyright (C) 2024-2026 Siraj Razick
# SPDX-License-Identifier: AGPL-3.0-only

CC ?= gcc

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
LIBDIR      ?= $(PREFIX)/lib
INCDIR      ?= $(PREFIX)/include/plexy
SYSTEMD_DIR ?= $(PREFIX)/lib/systemd/system
SYSCONFDIR  ?= /etc

BASE_CFLAGS = -std=c11 -O2 -D_GNU_SOURCE -I. -Isrc -Iinclude -I$(INCDIR)
# Include GLEW before GL so the source's explicit <GL/gl.h> does not trip
# the "gl.h included before glew.h" guard in system GLEW headers.
BASE_CFLAGS += -include GL/glew.h
CFLAGS += $(BASE_CFLAGS)

PKG_MODULES = libdrm gbm egl gl libinput xkbcommon dbus-1 libsystemd

PKG_CFLAGS = $(shell pkg-config --cflags $(PKG_MODULES))
PKG_LIBS   = $(shell pkg-config --libs $(PKG_MODULES))

SRC = $(wildcard src/dm/*.c)

all: plexy-dm

plexy-dm: $(SRC) $(INCDIR)/plexy_canvas.h
	$(CC) $(SRC) $(CFLAGS) $(PKG_CFLAGS) \
	    $(PKG_LIBS) \
	    -lGLEW -lpam -lm -ludev -lXcursor -lX11 -lpthread \
	    -L$(LIBDIR) -lplexycanvas \
	    -Wl,-rpath,'$$ORIGIN/../lib' \
	    -o $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 plexy-dm $(DESTDIR)$(BINDIR)/plexy-dm
	install -d $(DESTDIR)$(SYSCONFDIR)/plexy-dm
	install -m 644 scripts/plexy-dm/plexy-dm.conf $(DESTDIR)$(SYSCONFDIR)/plexy-dm/plexy-dm.conf
	install -d $(DESTDIR)$(SYSTEMD_DIR)
	sed -e 's|@PLEXY_DM_EXEC@|$(BINDIR)/plexy-dm|g' \
	    -e 's|@PLEXY_DM_CONFIG@|$(SYSCONFDIR)/plexy-dm/plexy-dm.conf|g' \
	    scripts/systemd/plexy-dm.service.in > $(DESTDIR)$(SYSTEMD_DIR)/plexy-dm.service

clean:
	rm -f plexy-dm

.PHONY: all install clean
