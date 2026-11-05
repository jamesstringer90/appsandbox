KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)
INSTDIR ?= /lib/modules/$(shell uname -r)/updates/dxgkrnl
SOURCE_TAG := $(shell sed -n 's/^[[:space:]]*"tag": "\([^"]*\)".*/\1/p' ../../../vendor/dxgkrnl/source-version.json)
SOURCE_DIR := $(PWD)/.source

all: source
	$(MAKE) modules

modules:
	$(MAKE) -C $(KDIR) M=$(SOURCE_DIR)/src modules

source:
	@test -n "$(SOURCE_TAG)"
	@if [ ! -d "$(SOURCE_DIR)/.git" ]; then git clone --depth 1 --branch "$(SOURCE_TAG)" https://github.com/jamesstringer90/asb-dxgkrnl.git "$(SOURCE_DIR)"; fi
	@test -z "$$(git -C "$(SOURCE_DIR)" status --porcelain --untracked-files=no)"
	@git -C "$(SOURCE_DIR)" rev-parse --verify "refs/tags/$(SOURCE_TAG)" >/dev/null 2>&1 || git -C "$(SOURCE_DIR)" fetch --depth 1 origin "refs/tags/$(SOURCE_TAG):refs/tags/$(SOURCE_TAG)"
	@git -C "$(SOURCE_DIR)" switch --detach "refs/tags/$(SOURCE_TAG)"

clean:
	@if [ -d "$(SOURCE_DIR)/src" ]; then $(MAKE) -C $(KDIR) M=$(SOURCE_DIR)/src clean; fi

install: all
	install -d $(INSTDIR)
	install -m 0644 $(SOURCE_DIR)/src/dxgkrnl.ko $(INSTDIR)/
	depmod -a $(shell uname -r)

.PHONY: all source modules clean install
