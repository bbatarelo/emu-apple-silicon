# E-MU Tracker Pre driver for Apple Silicon macOS.
#
#   make            build the driver and the tools
#   make install    install the driver  (asks for your password)
#   make uninstall  remove it           (asks for your password)
#   make check      show what the driver is doing
#   make record     record 5 seconds and report what arrived
#   make test       run the test suite (no hardware needed)
#   make test-recovery  fault-inject the driver and check it recovers
#   make test-recovery  fault-inject the running driver and check it recovers
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

.PHONY: test-recovery all driver tools install uninstall check record loopback test clean help

all: driver tools

help:
	@sed -n '2,12p' Makefile | sed 's/^# \?//'

# ---------------------------------------------------------------- the core

$(CORE_LIB): $(CORE)/src/*.rs $(CORE)/Cargo.toml
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

tools: $(BIN)/emu-probe $(BIN)/hal-check $(BIN)/hal-record $(BIN)/hal-trace \
       $(BIN)/hal-loopback

$(BIN)/emu-probe: tools/emu-probe/*.c tools/emu-probe/*.h shared/*.c shared/*.h $(CORE_LIB)
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

$(BIN)/hal-trace: tools/hal-trace/main.c
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation

$(BIN)/hal-loopback: tools/hal-loopback/*.c tools/hal-loopback/*.h
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ \
	    tools/hal-loopback/main.c tools/hal-loopback/analysis.c \
	    tools/hal-loopback/selftest.c \
	    -framework CoreAudio -framework CoreFoundation

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
	@echo "installed. Select the device in System Settings > Sound."

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

# Needs a cable from the outputs back to the inputs, both channels, at a level
# that does not clip. Everything else here can be checked without hardware;
# this is the only thing that closes the loop. Runs at whatever sample rate the
# device is set to; hal-loopback -r <hz> sets one first.
loopback: $(BIN)/hal-loopback
	@$(BIN)/hal-loopback -w $(BUILD)/loopback.wav

# Injects a transport fault into the installed driver and checks the engine
# rebuilds, gives up when it should, and comes back. Needs the driver installed
# and audio playing to it -- a fault can only go into a stream that exists.
test-recovery: $(BIN)/hal-check
	@./scripts/test-recovery.sh

test: $(BIN)/hal-loopback
	@cd $(CORE) && cargo test
	@echo
	@$(BIN)/hal-loopback selftest

clean:
	rm -rf $(BUILD)
	@cd $(CORE) && cargo clean
