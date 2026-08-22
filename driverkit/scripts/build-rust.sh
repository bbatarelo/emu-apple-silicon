#!/usr/bin/env bash
#
# Builds the freestanding Rust static library for the custom DriverKit target
# and verifies the resulting archive before it is allowed near xcodebuild.
#
# Corresponds to Test 1 in EMU_Tracker_Pre_Development_Guidelines.md section 23.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

bold "Building Rust core for $RUST_TARGET"

if ! rustup toolchain list | grep -q "^${RUST_TOOLCHAIN}-"; then
    fail "Rust toolchain '$RUST_TOOLCHAIN' is not installed.
       The custom DriverKit target needs -Z build-std, which is nightly-only.
       Install with: rustup toolchain install $RUST_TOOLCHAIN --component rust-src"
fi

# The SDK version is written into the target spec, so regenerate it rather than
# trusting a checked-in copy against whatever Xcode is currently installed.
"$(dirname "${BASH_SOURCE[0]}")/gen-rust-target.sh"

cd "$RUST_DIR"

cargo_args=(+"$RUST_TOOLCHAIN" build --target "$RUST_TARGET" -Z build-std=core,compiler_builtins)
[[ "$RUST_PROFILE" == "release" ]] && cargo_args+=(--release)

RUST_TARGET_PATH="$RUST_DIR/targets" RUSTFLAGS="-Zunstable-options" cargo "${cargo_args[@]}"

[[ -f "$RUST_ARCHIVE" ]] || fail "expected archive not produced: $RUST_ARCHIVE"

bold "Verifying archive"

arch_info="$(lipo -info "$RUST_ARCHIVE")"
info "$arch_info"
[[ "$arch_info" == *arm64* ]] || fail "archive is not arm64"

# Platform metadata is the whole reason the plain aarch64-apple-darwin target
# cannot be used: ld refuses to mix macOS objects into a DriverKit link.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
( cd "$tmp" && ar x "$RUST_ARCHIVE" )
member="$(find "$tmp" -name '*probe*cgu*.o' | head -1)"
[[ -n "$member" ]] || fail "could not find probe object inside archive"

platform="$(vtool -show-build "$member" | awk '/platform/ {print $2}')"
info "platform: $platform"
[[ "$platform" == "DRIVERKIT" ]] || \
    fail "archive objects are built for '$platform', not DRIVERKIT.
       Linking these into a dext will fail. Check targets/$RUST_TARGET.json."

# Read the symbol table once. Piping nm into `grep -q` in a loop would make nm
# die of SIGPIPE when grep exits early, and `set -o pipefail` would then report
# every successful match as a failure.
symbols="$(nm -gU "$RUST_ARCHIVE" 2>/dev/null || true)"

missing=0
for sym in emu_rust_probe_add probe_core_init probe_core_increment \
           probe_core_destroy probe_core_size probe_core_align \
           probe_counter_bump probe_counter_read; do
    if ! grep -q "T _$sym\$" <<<"$symbols"; then
        warn "missing exported symbol: $sym"
        missing=1
    fi
done
[[ "$missing" == "0" ]] || fail "archive does not export the expected C ABI surface"
info "all 8 C ABI symbols exported"

# A freestanding archive must not drag in libSystem or a Rust runtime. If these
# ever reappear, the FAIL criteria in guidelines section 24 are in play.
defined="$(mktemp)"; undefined="$(mktemp)"
nm -g "$RUST_ARCHIVE" 2>/dev/null | grep -E '^[0-9a-f]+ [A-TW]' | awk '{print $3}' | sort -u > "$defined"
nm -g "$RUST_ARCHIVE" 2>/dev/null | grep '^ *U ' | awk '{print $2}' | sort -u > "$undefined"
external="$(comm -23 "$undefined" "$defined" || true)"
rm -f "$defined" "$undefined"

# memcpy/memset/memcmp/bzero are emitted by LLVM for ordinary struct and slice
# operations and are provided by the DriverKit SDK, so they are expected. Any
# other unresolved symbol is not: it means std, libSystem or a Rust runtime
# dependency crept in, which is a FAIL condition under guidelines section 24.
expected='^_(memcpy|memset|memcmp|memmove|bzero)$'
unexpected="$(grep -Ev "$expected" <<<"$external" | grep -v '^$' || true)"

if [[ -n "$unexpected" ]]; then
    warn "archive has UNEXPECTED external dependencies:"
    printf '%s\n' "$unexpected" | sed 's/^/    /' >&2
    fail "a freestanding DriverKit archive must not need these.
       See guidelines section 24 (FAIL / prefer C++ production core)."
fi

if [[ -n "$external" ]]; then
    info "external symbols (all DriverKit-provided): $(tr '\n' ' ' <<<"$external")"
else
    info "no unresolved external symbols"
fi
info "no libc, no std, no Rust runtime dependency"

bold "Rust archive OK: $RUST_ARCHIVE"
