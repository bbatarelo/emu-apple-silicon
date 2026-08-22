#!/usr/bin/env bash
#
# Builds the dext and the host app that carries it, then verifies that the Rust
# object code actually survived into the shipped binary.
#
# Corresponds to Test 2 in EMU_Tracker_Pre_Development_Guidelines.md section 23.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[[ -f "$RUST_ARCHIVE" ]] || fail "Rust archive missing. Run scripts/build-rust.sh first."

bold "Building $HOST_SCHEME ($CONFIGURATION)"

if [[ "$SIGNING" == "0" ]]; then
    info "signing disabled (SIGNING=0): builds and links, but the dext will not load"
fi

build_xcodebuild_args
xcodebuild "${XCB_ARGS[@]}" -scheme "$HOST_SCHEME" build

app="$(app_path)"
dext="$(embedded_driver_path)"
driver_bin="$dext/$DRIVER_BUNDLE_ID"

[[ -d "$app" ]]  || fail "host app not produced at $app"
[[ -d "$dext" ]] || fail "dext was not embedded at $dext"

bold "Verifying built products"
info "app:  $app"
info "dext: ${dext#"$app"/}"

platform="$(vtool -show-build "$driver_bin" | awk '/platform/ {print $2}')"
info "dext platform: $platform"
[[ "$platform" == "DRIVERKIT" ]] || fail "dext is not a DriverKit image"

# The whole point of Milestone 0: prove Rust code is really in the dext.
count="$(nm -m "$driver_bin" 2>/dev/null | grep -cE '_emu_rust_probe_add|_probe_core_|_probe_counter_' || true)"
info "Rust C ABI symbols in dext: $count"
[[ "$count" -ge 8 ]] || fail "expected at least 8 Rust symbols in the dext, found $count"

bold "Build OK"
