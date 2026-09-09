# tools/linux/Makefile
#
# Top-level orchestrator for everything that has to be built on a Linux box
# for AppSandbox to support Linux guests: the 5 agent daemons (userspace
# ELFs) plus the two out-of-tree kernel modules (dxgkrnl and asb_drm).
#
# Workflow
# --------
#
# 1. Clone the appsandbox repo onto a Linux machine that has matching kernel
#    headers installed (e.g. Ubuntu 26.04 with linux-headers-generic).
# 2. cd tools/linux
# 3. make
# 4. Build artifacts land under tools/linux/dist/ — gitignored, this dir is
#    only valid on this machine for this kernel.
# 5. Copy dist/ over to your Windows host's appsandbox checkout, into
#    release/resources/linux/. Final instructions are printed at the end of
#    a successful build.
# 6. On the Windows host, rebuild AppSandbox.sln. The existing PostBuildEvent
#    on AppSandbox.vcxproj xcopies release/resources/ -> bin/<cfg>/resources/,
#    so the Linux tree comes along for the ride.
#
# Targets
# -------
#   make           agents + modules + install to dist/   (default)
#   make compile   build only, don't populate dist/
#   make agents    build only the userspace daemons
#   make modules   build only the kernel modules
#   make clean     wipe build outputs (object files, binaries, .ko)
#   make distclean clean + remove dist/

KVER  := $(shell uname -r)
ARCH  := $(shell uname -m)
DISTDIR := $(CURDIR)/dist

.PHONY: all compile agents modules dxgkrnl asb_drm install-agents install-modules \
        clean distclean copy-help

all: compile install-agents install-modules copy-help

compile: agents modules

# ---- userspace daemons ------------------------------------------------------

# Re-uses the existing tools/linux/agent/Makefile which knows how to compile
# each daemon with the right libs (libdrm for display, libasound for audio,
# libxcb for clipboard).
agents:
	$(MAKE) -C agent

install-agents: agents
	@install -d $(DISTDIR)
	@for bin in appsandbox-agent appsandbox-audio appsandbox-clipboard \
	            appsandbox-display appsandbox-input; do \
	    if [ -f agent/$$bin ]; then \
	        install -m 0755 agent/$$bin $(DISTDIR)/; \
	        echo "  installed $(DISTDIR)/$$bin"; \
	    fi; \
	done
	install -d $(DISTDIR)/gnome/appsandbox-pointer@appsandbox
	install -m 0644 agent/gnome/appsandbox-pointer@appsandbox/*.json agent/gnome/appsandbox-pointer@appsandbox/*.js $(DISTDIR)/gnome/appsandbox-pointer@appsandbox/

# ---- kernel modules ---------------------------------------------------------
#
# Both modules use the standard out-of-tree kbuild idiom (`make -C
# /lib/modules/<kver>/build M=<src>`). We don't go through DKMS here —
# DKMS is a runtime concern handled by setup.sh inside the guest VM. Here
# we just produce a .ko for the current kernel.

modules: dxgkrnl asb_drm

dxgkrnl:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/dxgkrnl/src modules

asb_drm:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/asb_drm modules

install-modules: modules
	@install -d $(DISTDIR)/modules/$(KVER)
	@if [ -f dxgkrnl/src/dxgkrnl.ko ]; then \
	    install -m 0644 dxgkrnl/src/dxgkrnl.ko $(DISTDIR)/modules/$(KVER)/; \
	    echo "  installed $(DISTDIR)/modules/$(KVER)/dxgkrnl.ko"; \
	fi
	@if [ -f asb_drm/asb_drm.ko ]; then \
	    install -m 0644 asb_drm/asb_drm.ko $(DISTDIR)/modules/$(KVER)/; \
	    echo "  installed $(DISTDIR)/modules/$(KVER)/asb_drm.ko"; \
	fi

# ---- cleanup ----------------------------------------------------------------

clean:
	-$(MAKE) -C agent clean
	-$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/dxgkrnl/src clean
	-$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/asb_drm clean

distclean: clean
	rm -rf $(DISTDIR)

# ---- copy-to-Windows reminder ----------------------------------------------

copy-help:
	@printf '\n'
	@printf '================================================================\n'
	@printf 'Build complete.  Artifacts staged in:\n'
	@printf '    %s\n' '$(DISTDIR)'
	@printf '\n'
	@printf 'Next step (manual): get this dist/ tree onto your Windows host\n'
	@printf 'and drop the contents at:\n'
	@printf '    <appsandbox>/release/resources/linux/\n'
	@printf '\n'
	@printf 'A simple tarball is fine; transfer it by whatever means you\n'
	@printf 'normally use to move files between this machine and the host\n'
	@printf '(USB stick, shared drive, file-share VM, scp, etc.):\n'
	@printf '\n'
	@printf '  tar -C %s -czf ~/asb-linux.tar.gz .\n' '$(DISTDIR)'
	@printf '\n'
	@printf 'Then on the Windows host, in your appsandbox checkout, extract\n'
	@printf 'over release/resources/linux/ (creating the dir if needed).\n'
	@printf '\n'
	@printf 'After copying, rebuild AppSandbox.sln (Release).  The PostBuildEvent\n'
	@printf 'on AppSandbox.vcxproj xcopies release/resources/ into\n'
	@printf 'bin/<cfg>/resources/, so the Linux artifacts travel to the runtime\n'
	@printf 'location automatically.\n'
	@printf '================================================================\n'
