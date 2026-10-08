CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11 -lm
PREFIX ?= $(HOME)/.local

all: xdf

xdf: xdf.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f xdf

# Install the binary, a desktop entry and an icon. GNOME shows the icon from
# the desktop entry, matched by WM_CLASS, and ignores the window's own icon.
install: xdf
	install -Dm755 xdf $(DESTDIR)$(PREFIX)/bin/xdf
	install -Dm644 xdf.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/xdf.svg
	sed 's|^Exec=.*|Exec=$(PREFIX)/bin/xdf|' xdf.desktop > xdf.desktop.tmp
	install -Dm644 xdf.desktop.tmp $(DESTDIR)$(PREFIX)/share/applications/xdf.desktop
	rm -f xdf.desktop.tmp
	-gtk-update-icon-cache -q -t $(DESTDIR)$(PREFIX)/share/icons/hicolor
	-update-desktop-database -q $(DESTDIR)$(PREFIX)/share/applications

.PHONY: all clean install
