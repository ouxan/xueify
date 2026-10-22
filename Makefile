VERSION = 0.2.0

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
MANDIR ?= $(DATADIR)/man
ifeq ($(PREFIX),$(HOME)/.local)
USERUNITDIR ?= $(DATADIR)/systemd/user
else
USERUNITDIR ?= $(PREFIX)/lib/systemd/user
endif

PKG_CONFIG ?= pkg-config
PKGS = wayland-client libsystemd cairo pangocairo gdk-pixbuf-2.0

WLR_PROTOCOLS ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wlr-protocols 2>/dev/null || echo /usr/share/wlr-protocols)
WL_PROTOCOLS ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

# CFLAGS and LDFLAGS stay free for the user or the packager
CFLAGS ?= -O2 -pipe
XCFLAGS = -std=c11 -D_GNU_SOURCE -DVERSION='"$(VERSION)"' -Wall -Wextra -Ibuild -Isrc \
	$(shell $(PKG_CONFIG) --cflags $(PKGS)) $(CPPFLAGS) $(CFLAGS)
XLDFLAGS = -Wl,--as-needed $(LDFLAGS)
LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

PROTOCOLS = \
	wlr-layer-shell-unstable-v1:$(WLR_PROTOCOLS)/unstable/wlr-layer-shell-unstable-v1.xml \
	xdg-shell:$(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
	cursor-shape-v1:$(WL_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml \
	tablet-v2:$(WL_PROTOCOLS)/stable/tablet/tablet-v2.xml

proto_name = $(firstword $(subst :, ,$(1)))
proto_xml = $(lastword $(subst :, ,$(1)))
PROTO_HEADERS = $(foreach p,$(PROTOCOLS),build/$(call proto_name,$(p))-client-protocol.h)
PROTO_OBJS = $(foreach p,$(PROTOCOLS),build/$(call proto_name,$(p))-protocol.o)
OBJS = $(patsubst src/%.c,build/%.o,$(wildcard src/*.c)) $(PROTO_OBJS)

SUBST = sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@DATADIR@|$(DATADIR)|g' -e 's|@VERSION@|$(VERSION)|g'

all: xueify

xueify: $(OBJS)
	$(CC) $(XLDFLAGS) -o $@ $^ $(LIBS)

build/%.o: src/%.c src/xueify.h $(PROTO_HEADERS) | build
	$(CC) $(XCFLAGS) -c -o $@ $<

build/%-protocol.o: build/%-protocol.c
	$(CC) $(XCFLAGS) -c -o $@ $<

define proto_rules
build/$(call proto_name,$(1))-client-protocol.h: $(call proto_xml,$(1)) | build
	wayland-scanner client-header $$< $$@
build/$(call proto_name,$(1))-protocol.c: $(call proto_xml,$(1)) | build
	wayland-scanner private-code $$< $$@
endef
$(foreach p,$(PROTOCOLS),$(eval $(call proto_rules,$(p))))

build:
	mkdir -p build

# Sample cards drawn with the daemon's renderer, used for docs/preview.png
build/preview: tools/preview.c build/render.o
	$(CC) $(XCFLAGS) $(XLDFLAGS) -o $@ $^ $(LIBS)

preview: build/preview
	build/preview docs/preview.png

install: xueify
	install -Dm755 xueify $(DESTDIR)$(BINDIR)/xueify
	$(SUBST) data/org.freedesktop.Notifications.service.in | \
		install -Dm644 /dev/stdin $(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.Notifications.service
	$(SUBST) data/xueify.service.in | install -Dm644 /dev/stdin $(DESTDIR)$(USERUNITDIR)/xueify.service
	$(SUBST) docs/xueify.1 | install -Dm644 /dev/stdin $(DESTDIR)$(MANDIR)/man1/xueify.1
	install -Dm644 LICENSE $(DESTDIR)$(DATADIR)/licenses/xueify/LICENSE
	install -Dm644 THIRD_PARTY_NOTICES.md $(DESTDIR)$(DATADIR)/licenses/xueify/THIRD_PARTY_NOTICES.md

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xueify \
		$(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.Notifications.service \
		$(DESTDIR)$(USERUNITDIR)/xueify.service \
		$(DESTDIR)$(MANDIR)/man1/xueify.1
	rm -rf $(DESTDIR)$(DATADIR)/licenses/xueify

clean:
	rm -rf build xueify

.PHONY: all preview install uninstall clean
.SECONDARY:
