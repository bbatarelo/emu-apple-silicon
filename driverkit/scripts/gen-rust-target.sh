#!/usr/bin/env bash
#
# Generates the custom Rust target specification for DriverKit.
#
# Guidelines section 14 (Path B2) says the target JSON must not be frozen in the
# project, because it depends on the installed Xcode/SDK and on rustc's own
# target-spec schema. So it is derived, every build, from:
#
#   1. rustc's current aarch64-apple-darwin spec  (tracks schema changes)
#   2. the installed DriverKit SDK version        (tracks Xcode upgrades)
#
# and only the deltas that matter are applied here.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

sdk_version="$(xcrun --sdk driverkit --show-sdk-version)"
out="$RUST_DIR/targets/$RUST_TARGET.json"
mkdir -p "$RUST_DIR/targets"

base_spec="$(rustc +"$RUST_TOOLCHAIN" -Z unstable-options \
    --target aarch64-apple-darwin --print target-spec-json 2>/dev/null)" \
    || fail "could not read base target spec from rustc +$RUST_TOOLCHAIN"

SDK_VERSION="$sdk_version" BASE_SPEC="$base_spec" python3 - "$out" <<'PYEOF'
import json, os, sys

out_path = sys.argv[1]
spec = json.loads(os.environ["BASE_SPEC"])
sdk = os.environ["SDK_VERSION"]

# The one change that actually matters: emit LC_BUILD_VERSION with
# platform DRIVERKIT instead of platform MACOS. ld refuses to link macOS
# objects into a DriverKit image, which is what kills Path B1.
spec["llvm-target"] = f"arm64-apple-driverkit{sdk}"
spec["os"] = "driverkit"

# rustc asserts is_like_darwin <=> vendor == "apple", and when is_like_darwin
# is set it calls deployment_target(), which has a hardcoded list of Apple OSes
# and hits unreachable!() on "driverkit" (ICE). Turning the flag off routes
# llvm-target through verbatim, which is exactly what we want -- the version is
# already baked into the triple above. Mach-O output and the leading-underscore
# symbol prefix come from binary-format and the "m:o" data-layout, not from
# this flag, so nothing is lost.
spec["is-like-darwin"] = False
spec["vendor"] = "unknown"          # must be non-empty, must not be "apple"
spec["linker-flavor"] = "gnu-cc"    # must not be "darwin" when is_like_darwin is off
spec.pop("lld-flavor", None)
spec["binary-format"] = "mach-o"
spec["archive-format"] = "darwin"

# Freestanding: no std, no unwinding, no TLS, no dynamic linking.
spec["panic-strategy"] = "abort"
spec["has-thread-local"] = False
spec["dynamic-linking"] = False
spec["has-rpath"] = False
spec["crt-objects-fallback"] = "false"

# dSYM handling is a darwin-toolchain concept that no longer applies.
spec["debuginfo-kind"] = "dwarf"
spec["split-debuginfo"] = "off"
spec["supported-split-debuginfo"] = ["off"]

for key in ("dll-suffix", "link-env", "link-env-remove",
            "supported-sanitizers", "supports-xray", "target-mcount"):
    spec.pop(key, None)

spec["metadata"] = {
    "description": f"ARM64 Apple DriverKit {sdk} (freestanding, no_std)",
    "host_tools": False,
    "std": False,
    "tier": 3,
}

with open(out_path, "w") as f:
    json.dump(spec, f, indent=4, sort_keys=True)
    f.write("\n")

print(f"  generated {out_path} (llvm-target: {spec['llvm-target']})")
PYEOF

# rustc validates a number of internal consistency rules on custom specs; catch
# a bad spec here rather than inside a confusing cargo build failure.
RUST_TARGET_PATH="$RUST_DIR/targets" rustc +"$RUST_TOOLCHAIN" -Zunstable-options \
    --target "$RUST_TARGET" --print cfg >/dev/null 2>&1 \
    || fail "generated target spec was rejected by rustc. Run:
       RUST_TARGET_PATH=$RUST_DIR/targets rustc +$RUST_TOOLCHAIN -Zunstable-options --target $RUST_TARGET --print cfg"

# cargo caches target-spec probe results and will happily keep serving a stale
# (possibly ICE-ing) result after the spec changes.
rm -f "$RUST_DIR/target/.rustc_info.json"
