# esp-vision top-level Makefile
# Wraps idf.py with the right flags for the MicroPython esp32 port.

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BOARD     ?= ESP32_P4X_EYE
BUILD     := $(ROOT)/build/$(BOARD)
MP_BASE_REF ?= v1.28.0
MP_BASE_COMMIT := e0e9fbb17ed6fd06bb76e266ae554784c9c80804
MP_REPO   := $(ROOT)/lib/micropython
MP_OVERLAY := $(ROOT)/overlay/micropython
MP_COMPONENT_YML := $(ROOT)/overlay/component_yml
MP_PORT   := $(MP_REPO)/ports/esp32

IDF_ARGS := -C $(MP_PORT) \
            -B $(BUILD) \
            -D MICROPY_BOARD=$(BOARD) \
            -D USER_C_MODULES=$(ROOT)/micropython.cmake \
            -D MICROPY_FROZEN_MANIFEST=$(ROOT)/boards/$(BOARD)/manifest.py \
            -D EXTRA_COMPONENT_DIRS=$(ROOT)/components

ifdef ESPPORT
IDF_ARGS += -p $(ESPPORT)
endif

ifdef ESPBAUD
IDF_ARGS += -b $(ESPBAUD)
endif

.PHONY: all prepare-micropython ensure-build-source build flash monitor deploy erase menuconfig size clean distclean

all: build

prepare-micropython:
	@if [ "$$(git -C $(MP_REPO) rev-parse HEAD)" != "$(MP_BASE_COMMIT)" ]; then echo "lib/micropython must be checked out at $(MP_BASE_REF) ($(MP_BASE_COMMIT))"; exit 1; fi
	@if [ -d "$(MP_OVERLAY)" ]; then cp -a $(MP_OVERLAY)/. $(MP_REPO)/; fi
	@if [ -z "$$ESP_IDF_VERSION" ]; then echo "ESP_IDF_VERSION is not set; source the ESP-IDF export script before building"; exit 1; fi
	@if [ ! -f "$(MP_COMPONENT_YML)/release$$ESP_IDF_VERSION/idf_component.yml" ]; then echo "No component manifest for ESP-IDF $$ESP_IDF_VERSION; add overlay/component_yml/release$$ESP_IDF_VERSION/idf_component.yml"; exit 1; fi
	@cp $(MP_COMPONENT_YML)/release$$ESP_IDF_VERSION/idf_component.yml $(MP_PORT)/main/idf_component.yml
	@echo "[prepare-micropython] selected component manifest: overlay/component_yml/release$$ESP_IDF_VERSION/idf_component.yml (ESP-IDF $$ESP_IDF_VERSION)"

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
