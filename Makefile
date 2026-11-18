# E-MU Tracker Pre driver for Apple Silicon macOS.
#
#   make            build the driver and the tools
#   make install    install the driver  (asks for your password)
#   make uninstall  remove it           (asks for your password)
#   make check      show what the driver is doing
#   make identity   what the built and the installed driver each are
#   make record     record 5 seconds and report what arrived
#   make version    the version this tree builds
#   make version-check  VERSION, README badge and tag agree
#   make test       run the test suite (no hardware needed)
#   make MODE=two-part [install]  development build: resident HAL + engine process
#   make reload     two-part only: replace the engine, leave Core Audio running
#   make test-integration  silent start/retry tests (installed, idle device)
#   make test-build  build all C test executables without running them
#   make test-recovery  fault-inject the installed driver and check it recovers
#   make test-device    both of the above on the unit EMU_DEVICE names
#
# Needs: Xcode command line tools, and Rust (stable). Nothing else.

SHELL := /usr/bin/env bash

BUILD      := build
BIN        := $(BUILD)/bin
BUNDLE     := $(BUILD)/EMUTrackerPre.driver
INSTALL_DIR := /Library/Audio/Plug-Ins/HAL
MIDI_BUNDLE := $(BUILD)/EMUMIDIDriver.plugin
MIDI_INSTALL_DIR := /Library/Audio/MIDI Drivers

# The optional development build: the same HAL plug-in, with the engine in a
# process of its own that can be replaced without restarting coreaudiod. Its
# artifacts live in their own directory, so alternating modes can never install
# one mode's bundle as the other's. See docs/SPLIT-DRIVER-PLAN.md.
MODE       ?= one-part
TWO        := $(BUILD)/two-part
TWO_BUNDLE := $(TWO)/EMUTrackerPre.driver
TWO_HAL    := $(TWO_BUNDLE)/Contents/MacOS/EMUTrackerPre
ENGINE     := $(TWO)/emu-engine
ENGINE_SERVICE := net.batarelo.EMUTrackerPre.Engine
ENGINE_PLIST   := /Library/LaunchDaemons/$(ENGINE_SERVICE).plist

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
# For a sanitizer pass over the C tests, into a build directory of its own:
#   make test-split SANITIZE=-fsanitize=thread BUILD=build/tsan
SANITIZE ?=
CFLAGS  += $(SANITIZE)
FRAMEWORKS := -framework CoreFoundation -framework CoreAudio -framework IOKit
AUDIO_FRAMEWORKS := -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

.PHONY: version version-check test-recovery all driver midi-driver tools install uninstall uninstall-midi check record loopback test clean help

# What `make` builds and `make install` installs. The release driver unless
# MODE=two-part is given; the two never share a build directory.
ifeq ($(MODE),one-part)
HAL_BUNDLE := $(BUNDLE)
HAL_BUILT  := $(BUNDLE)
all: driver midi-driver tools
else ifeq ($(MODE),two-part)
HAL_BUNDLE := $(TWO_BUNDLE)
HAL_BUILT  := $(TWO_HAL)
all: two-part midi-driver tools
else
$(error MODE is "$(MODE)"; it must be one-part (the default) or two-part)
endif

# A recipe that fails after creating its target must not leave it looking
# built.
.DELETE_ON_ERROR:

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
	@sed -n '2,17p' Makefile | sed -E 's/^# ?//'

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

# ------------------------------------------------- the optional two-part build
#
# The plug-in source is the same file; only what it links against differs.
# One-part links the USB engine in. Two-part links the engine link instead and
# has no USB code at all: `make identity MODE=two-part` shows both facts.

.PHONY: two-part
two-part: $(TWO_HAL) $(ENGINE)

# The target is the bundle's executable, not its directory, and it is the last
# thing written: a directory exists from the first mkdir on, so a build that
# failed half way would look finished to the next `make`. With the file as the
# target -- and .DELETE_ON_ERROR, for a failure after it exists, such as
# signing -- a failed build is one make tries again. Every other file in the
# bundle is rewritten on the way, so nothing stale survives either.
$(TWO_HAL): driver/plugin.c driver/engine_remote.c driver/engine_shared.h driver/usb_engine.h \
            driver/ring.h driver/Info.plist shared/*.h
	@echo "building the two-part HAL"
	@mkdir -p $(TWO_BUNDLE)/Contents/MacOS
	@sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@GITREV@/$(GITREV)/g' \
	     driver/Info.plist > $(TWO_BUNDLE)/Contents/Info.plist
	@# The driver host is sandboxed; this is what lets it look the engine up.
	@/usr/libexec/PlistBuddy -c "Add :AudioServerPlugIn_MachServices array" \
	    -c "Add :AudioServerPlugIn_MachServices:0 string $(ENGINE_SERVICE)" \
	    $(TWO_BUNDLE)/Contents/Info.plist
	@clang -bundle $(CFLAGS) -mmacosx-version-min=14.0 -o $@.new \
	    driver/plugin.c driver/engine_remote.c \
	    -framework CoreFoundation -framework CoreAudio
	@mv -f $@.new $@
	@./scripts/sign.sh $(TWO_BUNDLE)
	@codesign --verify --strict $(TWO_BUNDLE)

# Built beside itself and moved into place: a running engine keeps the image
# it started from, and launchd never finds a half-written one.
$(ENGINE): driver/engine_service.c driver/engine_shared.h driver/usb_engine.c driver/usb_engine.h \
           driver/ring.h shared/*.c shared/*.h $(CORE_LIB)
	@echo "building the engine"
	@mkdir -p $(TWO)
	@clang $(CFLAGS) -Wno-deprecated-declarations -mmacosx-version-min=14.0 -o $(ENGINE).new \
	    driver/engine_service.c driver/usb_engine.c shared/usb_util.c $(CORE_LIB) \
	    -framework CoreFoundation -framework IOKit
	@./scripts/sign.sh $(ENGINE).new
	@codesign --verify --strict $(ENGINE).new
	@mv -f $(ENGINE).new $(ENGINE)

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

# Only the two-part install uses it; the release recipe is as it was.
$(BIN)/hal-defaults: tools/hal-defaults/main.c
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

#
# One recipe for both modes, so there is one place the installed driver is
# replaced. The bundle has the same name and identifier either way: whichever
# mode is installed replaces the other, and exactly one owns the hardware --
# the engine service is registered before a HAL that needs it is loaded, and
# removed before a HAL that owns USB itself is.

install: $(HAL_BUILT) $(MIDI_BUNDLE) $(if $(filter two-part,$(MODE)),$(ENGINE) $(BIN)/hal-defaults)
	@echo "installing to $(INSTALL_DIR) (this needs your password)"
	@codesign --verify --strict $(HAL_BUNDLE)
ifeq ($(MODE),two-part)
	@echo "the system's sound selections, to put back afterwards:"
	@$(BIN)/hal-defaults save $(BUILD)/audio-defaults.txt
	sudo ./scripts/engine-service.sh install "$(abspath $(ENGINE))" "$$(id -un)"
else
	@if [ -e $(ENGINE_PLIST) ]; then sudo ./scripts/engine-service.sh remove; fi
endif
	sudo rm -rf $(INSTALL_DIR)/EMUTrackerPre.driver
	sudo cp -R $(HAL_BUNDLE) $(INSTALL_DIR)/
	sudo mkdir -p "$(MIDI_INSTALL_DIR)"
	sudo rm -rf "$(MIDI_INSTALL_DIR)/EMUMIDIDriver.plugin"
	sudo cp -R $(MIDI_BUNDLE) "$(MIDI_INSTALL_DIR)/"
	sudo killall coreaudiod
	-sudo killall MIDIServer 2>/dev/null || true
	@echo
	@echo "installed. Select the device in System Settings > Sound."
ifeq ($(MODE),two-part)
	@echo "sound selections:"
	@$(BIN)/hal-defaults restore $(BUILD)/audio-defaults.txt || echo "  (some could not be put back; see above)"
	@echo "The engine runs from $(abspath $(ENGINE)): do not move this checkout."
	@echo "After changing the engine: make reload    To go back: make install"
endif

# Replaces the engine and nothing else: no password, no coreaudiod restart.
# Stop audio on the E-MU devices first; a stream running across a reload ends
# and its client has to reopen.
.PHONY: reload engine-status
reload: $(ENGINE)
	@./scripts/engine-service.sh reload "$(abspath $(ENGINE))"

engine-status: $(ENGINE)
	@$(ENGINE) ctl status

uninstall:
	@if [ -e $(ENGINE_PLIST) ]; then sudo ./scripts/engine-service.sh remove; fi
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

# The tree's build and the installed copy, reported separately: they are
# different revisions more often than not, and a measurement is of the
# installed one. Read-only.
.PHONY: identity
identity: $(HAL_BUILT) $(MIDI_BUNDLE)
	@echo "source: $(VERSION) (git $(GITREV)), MODE=$(MODE)"
	@./scripts/bundle-identity.sh $(HAL_BUNDLE)
	@./scripts/bundle-identity.sh $(INSTALL_DIR)/EMUTrackerPre.driver
	@./scripts/bundle-identity.sh $(MIDI_BUNDLE)
	@./scripts/bundle-identity.sh "$(MIDI_INSTALL_DIR)/EMUMIDIDriver.plugin"

record: $(BIN)/hal-record
	@$(BIN)/hal-record 5 $(BUILD)/recording.wav
	@echo
	@echo "listen with: afplay $(BUILD)/recording.wav"

# Needs a cable from the outputs back to the inputs, both channels, at a level
# that does not clip. Unlike the silent integration tests, this checks the
# analog signal path. Runs at whatever sample rate the
# device is set to; hal-loopback -r <hz> sets one first.
loopback: $(BIN)/hal-loopback
	@$(BIN)/hal-loopback -w $(BUILD)/loopback.wav

# Injects a transport fault into the installed driver and checks the engine
# rebuilds, gives up when it should, and comes back. Needs the driver
# installed. It opens its own silent stream on the unit EMU_DEVICE names -- a
# fault can only go into a stream that exists -- so nothing has to be playing
# and no default device changes.
test-recovery: $(BIN)/hal-check $(BIN)/hal-stream
	@./scripts/test-recovery.sh

# Everything that runs unattended against one installed unit, named by UID:
# the restart and startup-fault suites, then recovery. No cables.
.PHONY: test-device
test-device: test-integration
	@$(MAKE) --no-print-directory test-recovery

$(BIN)/hal-stream: tests/integration/hal-stream.c
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation

# Hardware-free suite: safe to run without installing or opening a device.
.PHONY: test-rust test-analysis test-regression test-build test-integration test-hal-startup

test: test-rust test-analysis test-regression test-split

test-rust:
	@cd $(CORE) && cargo test

test-analysis: $(BIN)/hal-loopback
	@$(BIN)/hal-loopback selftest

test-regression: test-usb-abort test-hal-surface

# Build-only entry point for contributors; never installs a driver.
test-build: $(BIN)/hal-loopback $(BIN)/usb-abort-test $(BIN)/hal-surface-test $(BIN)/hal-restart-test \
            $(BIN)/hal-stream \
            $(TWO)/test/fake-engine $(TWO)/test/split-engine-test two-part

# Run sequentially even under make -j: both tests use the same installed unit.
# Recovery (active playback) and analog loopback (cables) have separate setup;
# see tests/README.md rather than silently including them here.
#
# The unit is resolved to its full UID first and the tests are given that:
# with two units attached, "the first one found" is the wrong one half the
# time, and hal-stream refuses to choose.
test-integration: $(BIN)/hal-restart-test $(BIN)/hal-stream
	@uid="$$($(BIN)/hal-stream --uid)" || exit 2; echo "unit: $$uid"; \
	 EMU_DEVICE="$$uid" $(BIN)/hal-restart-test && \
	 EMU_DEVICE="$$uid" $(BIN)/hal-restart-test --startup-stale

test-hal-startup: $(BIN)/hal-restart-test $(BIN)/hal-stream
	@uid="$$($(BIN)/hal-stream --uid)" || exit 2; echo "unit: $$uid"; \
	 EMU_DEVICE="$$uid" $(BIN)/hal-restart-test --startup-stale

# An installed two-part engine runs from this build directory, and launchd
# keeps restarting it from there: removing it out from under the service leaves
# a daemon that cannot start. The guard refuses, and says what to do first.
clean:
	@./scripts/engine-service.sh guard-clean "$(abspath $(BUILD))"
	rm -rf $(BUILD)
	@cd $(CORE) && cargo clean

# Deterministic issue #7 reproducer. No USB hardware or installed-driver changes.
# Also included in `make test`; never opens a real USB device.
.PHONY: test-usb-abort
test-usb-abort: $(BIN)/usb-abort-test
	@$(BIN)/usb-abort-test

$(BIN)/usb-abort-test: tests/regression/usb-abort-test.c driver/usb_engine.c driver/usb_engine.h driver/ring.h shared/usb_util.c shared/usb_util.h shared/device.h $(CORE_LIB)
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -Wno-deprecated-declarations -o $@ $< shared/usb_util.c $(CORE_LIB) $(FRAMEWORKS)

# The release HAL surface -- objects, UIDs, rates, offsets, controls, custom
# properties, diagnostics keys, and what the plug-in asks of the engine --
# compared with the recorded one. The production plug-in runs against a fake
# host, bus and engine: no USB hardware, nothing installed. A deliberate change
# to the surface updates the expected file: make hal-surface-accept.
.PHONY: test-hal-surface hal-surface-accept
HAL_SURFACE_EXPECTED := tests/regression/hal-surface.expected

test-hal-surface: $(BIN)/hal-surface-test
	@$(BIN)/hal-surface-test > $(BUILD)/hal-surface.actual || \
	    { cat $(BUILD)/hal-surface.actual; exit 1; }
	@diff -u $(HAL_SURFACE_EXPECTED) $(BUILD)/hal-surface.actual || \
	    { echo "HAL surface differs from $(HAL_SURFACE_EXPECTED)" >&2; exit 1; }
	@echo "HAL surface matches $(HAL_SURFACE_EXPECTED)"

hal-surface-accept: $(BIN)/hal-surface-test
	@$(BIN)/hal-surface-test > $(HAL_SURFACE_EXPECTED)
	@echo "recorded $(HAL_SURFACE_EXPECTED); review the diff before committing"

$(BIN)/hal-surface-test: tests/regression/hal-surface-test.c driver/plugin.c driver/usb_engine.h shared/usb_util.h shared/device.h
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation

# The two-part driver across a real process boundary: the production plug-in
# and engine link in one process, the production engine service over a pretend
# device in another, started by launchd under a throwaway name in the user's
# own domain. No USB, nothing installed, no root. See tests/README.md.
.PHONY: test-split
test-split: $(TWO)/test/fake-engine $(TWO)/test/split-engine-test
	@./tests/regression/split-engine-test.sh $(TWO)/test

$(TWO)/test/fake-engine: driver/engine_service.c driver/engine_shared.h driver/usb_engine.h driver/ring.h \
                         tests/regression/fake-transport.c shared/usb_util.h shared/device.h
	@mkdir -p $(TWO)/test
	@clang $(CFLAGS) -DEMU_ENGINE_TEST_HOOKS -o $@ driver/engine_service.c \
	    tests/regression/fake-transport.c -framework CoreFoundation

$(TWO)/test/split-engine-test: tests/regression/split-engine-test.c driver/plugin.c driver/engine_remote.c \
                               driver/engine_shared.h driver/usb_engine.h driver/ring.h shared/usb_util.h
	@mkdir -p $(TWO)/test
	@clang $(CFLAGS) -DEMU_REMOTE_TEST_SERVICE -o $@ $< -framework CoreAudio -framework CoreFoundation

# Requires an idle, installed E-MU. Streams silence; no default/rate changes.
.PHONY: test-hal-restart
test-hal-restart: $(BIN)/hal-restart-test $(BIN)/hal-stream
	@uid="$$($(BIN)/hal-stream --uid)" || exit 2; echo "unit: $$uid"; \
	 EMU_DEVICE="$$uid" $(BIN)/hal-restart-test

$(BIN)/hal-restart-test: tests/integration/hal-restart-test.c tools/hal-check/main.c
	@mkdir -p $(BIN)
	@clang $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation
