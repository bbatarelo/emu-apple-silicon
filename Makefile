# E-MU Tracker Pre driver for Apple Silicon macOS.
#
#   make            build the driver and the tools
#   make install    install the driver  (asks for your password)
#   make uninstall  remove it           (asks for your password)
#   make check      show what the driver is doing
#   make record     record 5 seconds and report what arrived
#   make test       run the test suite (no hardware needed)
#
# Needs: Xcode command line tools, and Rust (stable). Nothing else.

SHELL := /usr/bin/env bash

BUILD      := build
BIN        := $(BUILD)/bin
BUNDLE     := $(BUILD)/EMUTrackerPre.driver
INSTALL_DIR := /Library/Audio/Plug-Ins/HAL

CORE       := rust/emu-ca0189
CORE_LIB   := $(CORE)/target/release/libemu_ca0189.a

CFLAGS  := -std=c17 -Wall -Wextra -O2
FRAMEWORKS := -framework CoreFoundation -framework CoreAudio -framework IOKit
AUDIO_FRAMEWORKS := -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

.PHONY: all driver tools install uninstall check record test clean help

all: driver tools

help:
	@sed -n '2,12p' Makefile | sed 's/^# \?//'

# ---------------------------------------------------------------- the core

$(CORE_LIB):
	@echo "building the CA0189 core"
	@cd $(CORE) && cargo build --release

# -------------------------------------------------------------- the driver

driver: $(BUNDLE)

$(BUNDLE): driver/*.c driver/*.h driver/Info.plist shared/*.c shared/*.h $(CORE_LIB)
	@echo "building the driver"
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	@cp driver/Info.plist $(BUNDLE)/Contents/Info.plist
	@clang -bundle $(CFLAGS) -mmacosx-version-min=14.0 \
	    -o $(BUNDLE)/Contents/MacOS/EMUTrackerPre \
	    driver/plugin.c driver/usb_engine.c shared/usb_util.c $(CORE_LIB) \
	    $(FRAMEWORKS)
	@./scripts/sign.sh $(BUNDLE)

# --------------------------------------------------------------- the tools

tools: $(BIN)/emu-probe $(BIN)/hal-check $(BIN)/hal-record

$(BIN)/emu-probe: tools/emu-probe/*.c tools/emu-probe/*.h shared/*.c $(CORE_LIB)
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -Wno-deprecated-declarations -o $@ \
	    tools/emu-probe/main.c tools/emu-probe/capture.c tools/emu-probe/duplex.c \
	    tools/emu-probe/lltest.c shared/usb_util.c $(CORE_LIB) $(FRAMEWORKS)

$(BIN)/hal-check: tools/hal-check/main.c
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation

$(BIN)/hal-record: tools/hal-record/main.c
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ $< $(AUDIO_FRAMEWORKS)

# ------------------------------------------------------------------ install
#
# Restarting coreaudiod interrupts all audio on the machine for a moment. Quit
# anything playing first.

install: $(BUNDLE)
	@echo "installing to $(INSTALL_DIR) (this needs your password)"
	sudo rm -rf $(INSTALL_DIR)/EMUTrackerPre.driver
	sudo cp -R $(BUNDLE) $(INSTALL_DIR)/
	sudo killall coreaudiod
	@echo
	@echo "installed. Select \"E-MU Tracker Pre\" in System Settings > Sound."

uninstall:
	sudo rm -rf $(INSTALL_DIR)/EMUTrackerPre.driver
	sudo killall coreaudiod
	@echo "removed."

# -------------------------------------------------------------- diagnostics

check: $(BIN)/hal-check
	@$(BIN)/hal-check

record: $(BIN)/hal-record
	@$(BIN)/hal-record 5 $(BUILD)/recording.wav
	@echo
	@echo "listen with: afplay $(BUILD)/recording.wav"

test:
	@cd $(CORE) && cargo test

clean:
	rm -rf $(BUILD)
	@cd $(CORE) && cargo clean
