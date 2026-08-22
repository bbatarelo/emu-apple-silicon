#!/usr/bin/env bash
#
# Captures toolchain fingerprints into toolchain-notes/.
#
# Guidelines section 23, Test 1: "The exact output varies by Xcode/Rust version.
# Capture it in the repository under a toolchain-notes/ directory so later
# toolchain upgrades can be compared rather than guessed about."
#
# The custom Rust DriverKit target depends on rustc internals that carry no
# stability promise, so when a future upgrade breaks the build, the diff against
# these files is the fastest way to see what moved.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

notes_dir="$REPO_ROOT/toolchain-notes"
mkdir -p "$notes_dir"
out="$notes_dir/$(date +%Y-%m-%d)-toolchain.txt"

{
    echo "Toolchain snapshot"
    echo "Captured: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo

    echo "== Host =="
    sw_vers
    echo "arch: $(uname -m)"
    echo

    echo "== Xcode =="
    xcodebuild -version
    echo "DriverKit SDK path:    $(xcrun --sdk driverkit --show-sdk-path)"
    echo "DriverKit SDK version: $(xcrun --sdk driverkit --show-sdk-version)"
    echo "iig: $(xcrun --sdk driverkit --find iig 2>/dev/null || echo 'n/a')"
    echo

    echo "== Rust =="
    rustc --version
    cargo --version
    echo "nightly: $(rustc +"$RUST_TOOLCHAIN" --version 2>/dev/null || echo 'not installed')"
    echo
    echo "-- installed toolchains --"
    rustup toolchain list
    echo

    echo "== Custom DriverKit target spec =="
    if [[ -f "$RUST_DIR/targets/$RUST_TARGET.json" ]]; then
        cat "$RUST_DIR/targets/$RUST_TARGET.json"
    else
        echo "(not generated; run scripts/gen-rust-target.sh)"
    fi
    echo

    if [[ -f "$RUST_ARCHIVE" ]]; then
        echo "== Rust archive =="
        echo "path: ${RUST_ARCHIVE#"$REPO_ROOT"/}"
        lipo -info "$RUST_ARCHIVE"
        echo
        echo "-- exported symbols --"
        nm -gU "$RUST_ARCHIVE" 2>/dev/null | grep -E ' T _' | sort -k3 || true
        echo
        echo "-- archive members --"
        ar t "$RUST_ARCHIVE" 2>/dev/null || true
        echo
        echo "-- platform metadata --"
        tmp="$(mktemp -d)"
        ( cd "$tmp" && ar x "$RUST_ARCHIVE" 2>/dev/null || true )
        for o in "$tmp"/*.o; do
            [[ -f "$o" ]] || continue
            echo "$(basename "$o"):"
            vtool -show-build "$o" 2>/dev/null | grep -E 'platform|minos' | sed 's/^/  /'
        done
        rm -rf "$tmp"
        echo
    fi

    driver_bin="$(driver_binary_path)"
    if [[ -f "$driver_bin" ]]; then
        echo "== Built dext =="
        lipo -info "$driver_bin"
        vtool -show-build "$driver_bin" | grep -E 'platform|minos'
        echo
        echo "-- Rust symbols present in dext --"
        nm -m "$driver_bin" 2>/dev/null | grep -E '_emu_rust_probe_add|_probe_core_|_probe_counter_' || true
        echo
        echo "-- undefined symbols (must all be DriverKit SDK) --"
        nm -u "$driver_bin" 2>/dev/null || true
    fi
} > "$out"

bold "Wrote $out"
info "$(wc -l < "$out" | tr -d ' ') lines"
