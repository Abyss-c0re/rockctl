# rockctl lives under Dev/clanker/rockctl — resolve Dev root for shared nanobot ARM toolchain
ROOT := $(abspath .)
# Clanker hub root (parent of rockctl)
CLANKER_ROOT := $(abspath $(ROOT)/..)
# Dev root (parent of Clanker) — override: make arm DEV_ROOT=...
DEV_ROOT := $(abspath $(CLANKER_ROOT)/..)
# Prefer in-hub nanobot toolchain
NANOBOT_TC := $(CLANKER_ROOT)/nanobot/toolchain/armv7l-linux-musleabihf-cross/bin
ifeq ($(wildcard $(NANOBOT_TC)/armv7l-linux-musleabihf-gcc),)
NANOBOT_TC := $(DEV_ROOT)/nanobot/toolchain/armv7l-linux-musleabihf-cross/bin
endif
ARM_CC := $(NANOBOT_TC)/armv7l-linux-musleabihf-gcc
ARM_STRIP := $(NANOBOT_TC)/armv7l-linux-musleabihf-strip

SRC := src/miio.c src/http.c src/places.c src/main.c third_party/aes.c third_party/md5.c
# Dash is embedded in clanker_dash_html.h (generated from www/index.html or clanker-dash)
HDR := src/*.h src/clanker_dash_html.h
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -Ithird_party -DCBC=1 -DECB=0 -DCTR=0 -pthread
LDFLAGS_HOST := -pthread
LDFLAGS_ARM := -static -pthread
HOST_OUT := build/host/rockctl
ARM_OUT := build/armv7/rockctl

.PHONY: all host arm clean install-robot embed-dash

# Sync ClankerDash SPA into rockctl embed header + www/
embed-dash:
	@./scripts/embed_dash.sh

all: host
	@test -x "$(ARM_CC)" && $(MAKE) arm || echo "skip arm (missing $(ARM_CC))"

host: $(HOST_OUT)
	@echo host $$(wc -c < $(HOST_OUT)) bytes

$(HOST_OUT): $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -o $@ $(SRC) $(LDFLAGS_HOST)

arm: $(ARM_OUT)
	@echo arm $$(wc -c < $(ARM_OUT)) bytes

$(ARM_OUT): $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	@test -x "$(ARM_CC)" || (echo "ARM toolchain not found: $(ARM_CC)" >&2; exit 1)
	$(ARM_CC) $(CFLAGS) $(LDFLAGS_ARM) -o $@ $(SRC)
	@test -x "$(ARM_STRIP)" && $(ARM_STRIP) $@ || true

install-robot: arm
	./scripts/install_robot.sh

clean:
	rm -rf build
