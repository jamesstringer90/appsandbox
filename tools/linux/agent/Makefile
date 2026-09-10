# App Sandbox Linux guest daemons — builds the in-VM binaries that match
# the wire protocol the Windows host stack expects. See
# docs/linux-idd-implementation-plan.md for architecture.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?=

PREFIX        ?= /usr/local
DESTDIR       ?=
BINDIR        := $(PREFIX)/bin
SYSTEMDDIR    := /etc/systemd/system
USERUNITDIR   := /etc/systemd/user
POINTER_EXT   := appsandbox-pointer@appsandbox
EXTENSIONDIR  := /usr/share/gnome-shell/extensions/$(POINTER_EXT)

# Daemons get added here as each phase lands.
BINS := appsandbox-agent appsandbox-display appsandbox-input appsandbox-audio appsandbox-clipboard

SYSTEM_UNITS := systemd/appsandbox-agent.service \
                systemd/appsandbox-display.service \
                systemd/appsandbox-input.service \
                systemd/appsandbox-audio.service
# No clipboard systemd unit — the agent owns the clipboard helper's
# lifecycle (spawn_clipboard_helper in appsandbox-agent.c). Same
# pattern as the Windows agent which spawns clipboard.exe /
# clipboard-reader.exe.
USER_UNITS   :=

all: $(BINS)

appsandbox-agent: appsandbox-agent.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Display daemon needs libdrm for drmModeGetFB2 + drmPrimeHandleToFD.
appsandbox-display: appsandbox-display.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libdrm) -o $@ $< \
	       $(LDFLAGS) $(shell pkg-config --libs libdrm)

appsandbox-input: appsandbox-input.c $(wildcard protocol.h ../../../src/core/protocol.h)
	$(CC) $(CFLAGS) -I../../../src/core -o $@ $< $(LDFLAGS) -lsystemd

# Audio daemon uses libasound to talk to the kernel snd-aloop driver.
appsandbox-audio: appsandbox-audio.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lasound

# Clipboard daemon: XCB (with XFIXES) for X11 selection ownership,
# libc iconv for text format conversion. libpng would be added when
# we wire the DIB ↔ PNG path; today CF_DIB → image/bmp is header-munge.
appsandbox-clipboard: appsandbox-clipboard.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lxcb -lxcb-xfixes

install-pointer:
	install -d $(DESTDIR)$(EXTENSIONDIR)
	install -m 0644 gnome/$(POINTER_EXT)/metadata.json gnome/$(POINTER_EXT)/extension.js $(DESTDIR)$(EXTENSIONDIR)/

install: $(BINS) install-pointer
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BINS) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SYSTEMDDIR)
	for u in $(SYSTEM_UNITS); do \
	    install -m 0644 $$u $(DESTDIR)$(SYSTEMDDIR)/; \
	done
	@if [ -n "$(USER_UNITS)" ]; then \
	    install -d $(DESTDIR)$(USERUNITDIR); \
	    for u in $(USER_UNITS); do \
	        install -m 0644 $$u $(DESTDIR)$(USERUNITDIR)/; \
	    done; \
	fi

# Convenience: install + enable all units in one shot. Requires root.
enable: install
	systemctl daemon-reload
	systemctl enable --now appsandbox-agent.service
	systemctl enable --now appsandbox-display.service
	systemctl enable --now appsandbox-input.service

disable:
	-systemctl disable --now appsandbox-input.service
	-systemctl disable --now appsandbox-display.service
	-systemctl disable --now appsandbox-agent.service

clean:
	rm -f $(BINS)

.PHONY: all install install-pointer enable disable clean
