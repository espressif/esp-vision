# esp-vision top-level Makefile
# Wraps idf.py with the right flags for the MicroPython esp32 port.

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BOARD     ?= ESP32_P4X_EYE
MICROPY_OVERLAY_TARGET ?= build
MP_BASE_REF ?= v1.28.0
MP_BASE_COMMIT := e0e9fbb17ed6fd06bb76e266ae554784c9c80804
MP_REPO   := $(ROOT)/lib/micropython
MP_OVERLAY := $(ROOT)/overlay/micropython
MP_COMPONENT_YML := $(ROOT)/overlay/component_yml
MP_BUILD  := $(ROOT)/build/micropython/idf$(ESP_IDF_VERSION)/micropython
ESP_VISION_IDF_OVERLAY := release$(ESP_IDF_VERSION)

ifeq ($(MICROPY_OVERLAY_TARGET),build)
MP_WORKTREE := $(MP_BUILD)
BUILD_SUFFIX :=
else ifeq ($(MICROPY_OVERLAY_TARGET),lib)
MP_WORKTREE := $(MP_REPO)
BUILD_SUFFIX := -lib
else
$(error MICROPY_OVERLAY_TARGET must be build or lib)
endif

BUILD     := $(ROOT)/build/$(BOARD)/idf$(ESP_IDF_VERSION)$(BUILD_SUFFIX)
MP_PORT   := $(MP_WORKTREE)/ports/esp32
BOARDS_DIR := $(ROOT)/boards

# Project each board's MicroPython-port files (boards/<BOARD>/port/) onto the
# prepared MicroPython esp32 port. boards/<BOARD>/ is the single authoring
# location; the matching ports/esp32/boards/<BOARD>/ directory is generated
# here rather than tracked under overlay/.
define project_boards
	@for port in $(BOARDS_DIR)/*/port; do \
		[ -d "$$port" ] || continue; \
		board=$$(basename $$(dirname "$$port")); \
		rm -rf "$(MP_PORT)/boards/$$board"; \
		mkdir -p "$(MP_PORT)/boards/$$board"; \
		cp -a "$$port/." "$(MP_PORT)/boards/$$board/"; \
	done
	@echo "[prepare-micropython] projected boards/*/port onto $(MP_PORT)/boards"
endef

IDF_ARGS := -C $(MP_PORT) \
            -B $(BUILD) \
            -D MICROPY_BOARD=$(BOARD) \
            -D ESP_VISION_IDF_OVERLAY=$(ESP_VISION_IDF_OVERLAY) \
            -D USER_C_MODULES=$(ROOT)/micropython.cmake \
            -D MICROPY_FROZEN_MANIFEST=$(ROOT)/boards/$(BOARD)/manifest.py \
            -D EXTRA_COMPONENT_DIRS=$(ROOT)/components

ifdef ESPPORT
IDF_ARGS += -p $(ESPPORT)
endif

ifdef ESPBAUD
IDF_ARGS += -b $(ESPBAUD)
endif

.PHONY: all prepare-micropython prepare-micropython-copy ensure-build-source build flash monitor deploy erase menuconfig size clean distclean

all: build

prepare-micropython:
	@if [ "$$(git -C $(MP_REPO) rev-parse HEAD)" != "$(MP_BASE_COMMIT)" ]; then echo "lib/micropython must be checked out at $(MP_BASE_REF) ($(MP_BASE_COMMIT))"; exit 1; fi
	@if [ -z "$$ESP_IDF_VERSION" ]; then echo "ESP_IDF_VERSION is not set; source the ESP-IDF export script before building"; exit 1; fi
	@if [ ! -f "$(MP_COMPONENT_YML)/$(ESP_VISION_IDF_OVERLAY)/idf_component.yml" ]; then echo "Unsupported ESP-IDF version $$ESP_IDF_VERSION; supported releases are v5.5, v6.0, and v6.1"; exit 1; fi
	@if [ "$(BOARD)" = "ESP32_S31_KORVO" ] && [ "$(ESP_VISION_IDF_OVERLAY)" != "release6.1" ]; then echo "ESP32_S31_KORVO is supported only with ESP-IDF release/v6.1 (current: $(ESP_VISION_IDF_OVERLAY))"; exit 1; fi
	@if [ ! -f "$(BOARDS_DIR)/$(BOARD)/port/mpconfigboard.cmake" ]; then echo "Board $(BOARD) is missing boards/$(BOARD)/port/mpconfigboard.cmake"; exit 1; fi
	@$(MAKE) --no-print-directory prepare-micropython-copy

ifeq ($(MICROPY_OVERLAY_TARGET),build)
# Export a clean tracked MicroPython snapshot instead of copying the working
# tree, so local submodule dirt and generated files never enter the build base.
prepare-micropython-copy:
	@rm -rf $(MP_BUILD)
	@mkdir -p $(MP_BUILD)
	@git -C $(MP_REPO) archive --format=tar -o "$(MP_BUILD)/.mp_src.tar" HEAD
	@tar -xf "$(MP_BUILD)/.mp_src.tar" -C $(MP_BUILD)
	@rm -f "$(MP_BUILD)/.mp_src.tar"
	@git -C $(MP_REPO) submodule foreach --recursive --quiet 'printf "%s\n" "$$displaypath"' > "$(MP_BUILD)/.mp_submodules"
	@while read submodule_path; do \
		if [ -d "$(MP_REPO)/$$submodule_path" ]; then \
			rm -rf "$(MP_BUILD)/$$submodule_path"; \
			mkdir -p "$(MP_BUILD)/$$submodule_path"; \
			git -C "$(MP_REPO)/$$submodule_path" archive --format=tar -o "$(MP_BUILD)/$$submodule_path/.mp_src.tar" HEAD || exit 1; \
			tar -xf "$(MP_BUILD)/$$submodule_path/.mp_src.tar" -C "$(MP_BUILD)/$$submodule_path" || exit 1; \
			rm -f "$(MP_BUILD)/$$submodule_path/.mp_src.tar"; \
		fi; \
	done < "$(MP_BUILD)/.mp_submodules"
	@rm -f "$(MP_BUILD)/.mp_submodules"
	@if [ -d "$(MP_OVERLAY)" ]; then cp -a $(MP_OVERLAY)/. $(MP_BUILD)/; fi
	$(project_boards)
	@yml="$(MP_COMPONENT_YML)/release$$ESP_IDF_VERSION/idf_component.yml"; \
	 [ -f "$$yml" ] || { echo "No component manifest for supported ESP-IDF release/v$$ESP_IDF_VERSION"; exit 1; }; \
	 cmp -s "$$yml" $(MP_PORT)/main/idf_component.yml || cp "$$yml" $(MP_PORT)/main/idf_component.yml; \
	 echo "[prepare-micropython] selected component manifest: $${yml#$(ROOT)/} (ESP-IDF $$ESP_IDF_VERSION)"
	@echo "[prepare-micropython] prepared build copy: build/micropython/idf$(ESP_IDF_VERSION)/micropython"
else ifeq ($(MICROPY_OVERLAY_TARGET),lib)
prepare-micropython-copy:
	@if [ -d "$(MP_OVERLAY)" ]; then cp -a $(MP_OVERLAY)/. $(MP_REPO)/; fi
	$(project_boards)
	@yml="$(MP_COMPONENT_YML)/release$$ESP_IDF_VERSION/idf_component.yml"; \
	 [ -f "$$yml" ] || { echo "No component manifest for supported ESP-IDF release/v$$ESP_IDF_VERSION"; exit 1; }; \
	 cmp -s "$$yml" $(MP_PORT)/main/idf_component.yml || cp "$$yml" $(MP_PORT)/main/idf_component.yml; \
	 echo "[prepare-micropython] selected component manifest: $${yml#$(ROOT)/} (ESP-IDF $$ESP_IDF_VERSION)"
	@echo "[prepare-micropython] applied overlay to lib/micropython"
endif

ensure-build-source: prepare-micropython
	@if [ -f "$(BUILD)/CMakeCache.txt" ] && [ ! -f "$(BUILD)/build.ninja" ]; then rm -rf $(BUILD); fi
	@if [ -f "$(BUILD)/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$(MP_PORT)" "$(BUILD)/CMakeCache.txt"; then rm -rf $(BUILD); fi

build: ensure-build-source
	@idf.py $(IDF_ARGS) build

flash: ensure-build-source
	@idf.py $(IDF_ARGS) flash

monitor: ensure-build-source
	@idf.py $(IDF_ARGS) monitor

deploy: build flash

erase: ensure-build-source
	@idf.py $(IDF_ARGS) erase-flash

menuconfig: ensure-build-source
	@idf.py $(IDF_ARGS) menuconfig

size: ensure-build-source
	@idf.py $(IDF_ARGS) size

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf $(ROOT)/build
