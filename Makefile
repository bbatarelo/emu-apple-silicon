# E-MU Tracker Pre driver for Apple Silicon macOS.
#
#   make            build the driver and the tools
#   make install    install the driver  (asks for your password)
#   make uninstall  remove it           (asks for your password)
#   make check      show what the driver is doing
#   make record     record 5 seconds and report what arrived
#   make version    the version this tree builds
#   make version-check  VERSION, README badge and tag agree
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
MIDI_BUNDLE := $(BUILD)/EMUMIDIDriver.plugin
MIDI_INSTALL_DIR := /Library/Audio/MIDI Drivers

CORE       := rust/emu-ca0189
CORE_LIB   := $(CORE)/target/release/libemu_ca0189.a

# One source of truth for the version. VERSION is the release; the git revision
# distinguishes a build from that tag from a working tree that has moved on,
# which "0.1.0" alone cannot. Both reach the code as defines and both bundles'
# Info.plist as substitutions, so nothing can report a version the build did
# not actually produce.
VERSION := $(shell cat VERSION)
GITREV  := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)$(shell \
             git diff --quiet 2>/dev/null || echo -dirty)

CFLAGS  := -std=c17 -Wall -Wextra -O2 \
           -DEMU_VERSION=\"$(VERSION)\" -DEMU_GITREV=\"$(GITREV)\"
FRAMEWORKS := -framework CoreFoundation -framework CoreAudio -framework IOKit
AUDIO_FRAMEWORKS := -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

.PHONY: version version-check test-recovery all driver midi-driver tools install uninstall uninstall-midi check record loopback test clean help

all: driver midi-driver tools

version:
	@echo "$(VERSION) (git $(GITREV))"

# Keeps the places a version is written from drifting apart. VERSION and the
# README badge are both in the tree and must agree -- there is no excuse for
# those differing. The tag is reported rather than enforced, because it is
# created at the merge, after the bump.
version-check:
	@badge=$$(sed -n 's/.*badge\/version-\([0-9][^-]*\)-.*/\1/p' README.md | head -1); \
	if [ "$$badge" != "$(VERSION)" ]; then \
	    echo "VERSION is $(VERSION) but the README badge says $${badge:-none}" >&2; \
	    echo "  update the badge at the top of README.md" >&2; \
	    exit 1; \
	fi; \
	echo "VERSION and README badge agree: $(VERSION)"; \
	if git rev-parse "v$(VERSION)" >/dev/null 2>&1; then \
	    echo "tag v$(VERSION) exists"; \
	else \
	    echo "tag v$(VERSION) not created yet -- tag it on the merge commit"; \
	fi

help:
	@sed -n '2,12p' Makefile | sed 's/^# \?//'

# ---------------------------------------------------------------- the core

$(CORE_LIB): $(CORE)/src/*.rs $(CORE)/Cargo.toml
	@echo "building the CA0189 core"
	@cd $(CORE) && cargo build --release

# -------------------------------------------------------------- the driver

driver: $(BUNDLE)

midi-driver: $(MIDI_BUNDLE)

$(BUNDLE): driver/*.c driver/*.h driver/Info.plist shared/*.c shared/*.h $(CORE_LIB)
	@echo "building the driver"
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	@sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@GITREV@/$(GITREV)/g' \
	     driver/Info.plist > $(BUNDLE)/Contents/Info.plist
	@clang -bundle $(CFLAGS) -mmacosx-version-min=14.0 \
	    -o $(BUNDLE)/Contents/MacOS/EMUTrackerPre \
	    driver/plugin.c driver/usb_engine.c shared/usb_util.c $(CORE_LIB) \
	    $(FRAMEWORKS)
	@./scripts/sign.sh $(BUNDLE)

# The MIDI half lives in a separate bundle because it loads into a different
# process: MIDIServer, not coreaudiod. The two share the USB device by each
# claiming only its own interface.
$(MIDI_BUNDLE): midi-driver/*.c midi-driver/Info.plist shared/*.c shared/*.h $(CORE_LIB)
	@echo "building the MIDI driver"
	@rm -rf $(MIDI_BUNDLE)
	@mkdir -p $(MIDI_BUNDLE)/Contents/MacOS
	@sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@GITREV@/$(GITREV)/g' \
	     midi-driver/Info.plist > $(MIDI_BUNDLE)/Contents/Info.plist
	@clang -bundle $(CFLAGS) -mmacosx-version-min=14.0 \
	    -o $(MIDI_BUNDLE)/Contents/MacOS/EMUMIDIDriver \
	    midi-driver/plugin.c shared/usb_util.c $(CORE_LIB) \
	    -framework CoreFoundation -framework CoreMIDI -framework IOKit
	@./scripts/sign.sh $(MIDI_BUNDLE)

# --------------------------------------------------------------- the tools

tools: $(BIN)/emu-probe $(BIN)/hal-check $(BIN)/hal-record $(BIN)/hal-trace \
       $(BIN)/hal-loopback $(BIN)/midi-check

$(BIN)/emu-probe: tools/emu-probe/*.c tools/emu-probe/*.h shared/*.c shared/*.h $(CORE_LIB)
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -Wno-deprecated-declarations -o $@ \
	    tools/emu-probe/main.c tools/emu-probe/capture.c tools/emu-probe/duplex.c \
	    tools/emu-probe/lltest.c tools/emu-probe/midi.c shared/usb_util.c \
	    $(CORE_LIB) $(FRAMEWORKS)

$(BIN)/midi-check: tools/midi-check/main.c
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -Wno-deprecated-declarations -o $@ $< \
	    -framework CoreMIDI -framework CoreFoundation

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

install: $(BUNDLE) $(MIDI_BUNDLE)
	@echo "installing to $(INSTALL_DIR) (this needs your password)"
	sudo rm -rf $(INSTALL_DIR)/EMUTrackerPre.driver
	sudo cp -R $(BUNDLE) $(INSTALL_DIR)/
	sudo mkdir -p "$(MIDI_INSTALL_DIR)"
	sudo rm -rf "$(MIDI_INSTALL_DIR)/EMUMIDIDriver.plugin"
	sudo cp -R $(MIDI_BUNDLE) "$(MIDI_INSTALL_DIR)/"
	sudo killall coreaudiod
	-sudo killall MIDIServer 2>/dev/null || true
	@echo
	@echo "installed. Select the device in System Settings > Sound."

uninstall:
	sudo rm -rf $(INSTALL_DIR)/EMUTrackerPre.driver
	sudo rm -rf "$(MIDI_INSTALL_DIR)/EMUMIDIDriver.plugin"
	sudo killall coreaudiod
	-sudo killall MIDIServer 2>/dev/null || true
	@echo "removed."

# MIDIServer holds the MIDI interface while the driver is installed, which
# blocks emu-probe's raw midi commands. This removes just the MIDI half.
uninstall-midi:
	sudo rm -rf "$(MIDI_INSTALL_DIR)/EMUMIDIDriver.plugin"
	-sudo killall MIDIServer 2>/dev/null || true
	@echo "MIDI driver removed."

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
