# vita-turbo-vpn - build and deploy helpers.
#
#   make                      build the kernel module and both plugins (uses
#                             your VitaSDK if $VITASDK is set, otherwise the
#                             pinned VitaSDK Docker image)
#   make app                  build the LiveArea app (app/build-release/vpnturbo.vpk)
#   make test                 run the host tests (kernel parts, config parser, protocol core)
#   make install VITA_IP=...  upload the modules over FTP (kernel and plugins to
#                             ur0:tai/, quick-menu rows to ur0:QuickMenuReborn/)
#   make deploy  VITA_IP=...  install, then reboot the Vita so they load
#   make log     VITA_IP=...  fetch kernel.log, shell.log and app.log into ./logs/
#   make config               print the lines to add to ur0:tai/config.txt
#   make clean
#
# install/deploy/log talk to vitacompanion (FTP on :1337, commands on :1338)
# running on the Vita. Output: build/kernel/vpnturbo_kernel.skprx,
# build/vpnturbo.suprx (SceShell), build/vpnturbo_settings.suprx (the
# Settings page), build/vpnturbo_qm.suprx (quick-menu rows)

BUILD   ?= build
VITA_IP ?=
# Pinned so every build uses the same toolchain (VitaSDK 2026.08, GCC 15.2).
VITASDK_IMAGE ?= vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb

KERNEL  := $(BUILD)/kernel/vpnturbo_kernel.skprx
PLUGIN  := $(BUILD)/vpnturbo.suprx
SETTINGS:= $(BUILD)/vpnturbo_settings.suprx
QM      := $(BUILD)/vpnturbo_qm.suprx
FTP      = ftp://$(VITA_IP):1337
DATA    := ur0:/data/vpnturbo

.PHONY: all build app test install deploy log config clean check-ip

all: build

# The protocol core (core/vita.mk) first: the kernel module links it.
build:
ifdef VITASDK
	$(MAKE) -C core -f vita.mk
	cmake -S . -B $(BUILD) -DCMAKE_TOOLCHAIN_FILE=$(VITASDK)/share/vita.toolchain.cmake
	cmake --build $(BUILD) -j
else
	docker run --rm -v "$(CURDIR)":/src -w /src $(VITASDK_IMAGE) sh -c '\
	  make -C core -f vita.mk -j$$(nproc) && \
	  cmake -S . -B $(BUILD) -DCMAKE_TOOLCHAIN_FILE=$$VITASDK/share/vita.toolchain.cmake && \
	  cmake --build $(BUILD) -j$$(nproc); r=$$?; \
	  chown -R $(shell id -u):$(shell id -g) $(BUILD) core/build; exit $$r'
endif

app:
	app/build.sh release

test:
	sh kernel/tests/run.sh
	$(MAKE) -C conf test
	$(MAKE) -C core test

check-ip:
	@test -n "$(VITA_IP)" || { echo "Set VITA_IP, e.g. make $(MAKECMDGOALS) VITA_IP=192.168.1.50"; exit 1; }

install: build check-ip
	@for f in $(KERNEL) $(PLUGIN) $(SETTINGS); do \
	  echo "upload $$f"; curl -sS -T $$f "$(FTP)/ur0:/tai/$$(basename $$f)" || exit 1; \
	done
	@echo "upload $(QM)"; curl -sS --ftp-create-dirs -T $(QM) "$(FTP)/ur0:/QuickMenuReborn/$$(basename $(QM))"
	@echo "Installed. If this is the first install, add the plugin to config.txt (make config)."

deploy: install
	@printf 'reboot\n' | nc -w 3 $(VITA_IP) 1338 >/dev/null || true
	@echo "Rebooting the Vita - the plugin loads at boot."

log: check-ip
	@mkdir -p logs
	@for f in kernel.log shell.log app.log; do \
	  curl -sS "$(FTP)/$(DATA)/$$f" -o "logs/$$f" && echo "logs/$$f"; \
	done

config:
	@echo "Add to ur0:tai/config.txt (then reboot):"
	@echo ""
	@echo "*KERNEL"
	@echo "ur0:tai/vpnturbo_kernel.skprx"
	@echo "*main"
	@echo "ur0:tai/vpnturbo.suprx"
	@echo "*NPXS10015"
	@echo "ur0:tai/vpnturbo_settings.suprx"
	@echo ""
	@echo "Quick-menu rows (optional, needs QuickMenuReborn): vpnturbo_qm.suprx in ur0:QuickMenuReborn/"

clean:
	rm -rf $(BUILD) core/build app/build app/build-release
