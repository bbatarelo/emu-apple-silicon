#!/usr/bin/env bash
#
# Says what a driver bundle is: which build, what it exports, what it links,
# and whether it talks to USB itself or to a separate engine service.
#
# Read-only, and it works on any bundle path, so the tree's build and whatever
# is installed are reported separately and can be diffed:
#
#     scripts/bundle-identity.sh build/EMUTrackerPre.driver
#     scripts/bundle-identity.sh /Library/Audio/Plug-Ins/HAL/EMUTrackerPre.driver
#
# The hash covers the executable only. Signing changes it, so two builds of the
# same source signed differently do not match; compare the version line for
# "same source" and the hash for "same file".

set -euo pipefail
bundle="${1:?usage: bundle-identity.sh <bundle>}"

if [[ ! -d "$bundle" ]]; then
    echo "bundle: $bundle"
    echo "  not present"
    exit 0
fi

plist="$bundle/Contents/Info.plist"
key() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || echo "(none)"; }
exe="$bundle/Contents/MacOS/$(key CFBundleExecutable)"

echo "bundle: $bundle"
echo "  identifier: $(key CFBundleIdentifier)"
echo "  version: $(key CFBundleShortVersionString), build $(key CFBundleVersion)"
echo "  factories: $(key CFPlugInFactories | sed -e '1d;$d' | tr -s ' \n' ' ' | sed 's/^ //;s/ $//')"
echo "  mach services declared: $(key AudioServerPlugIn_MachServices | tr -s ' \n' ' ')"

if [[ ! -f "$exe" ]]; then
    echo "  executable: missing"
    exit 1
fi

echo "  executable sha256: $(shasum -a 256 "$exe" | cut -d' ' -f1)"
echo "  architectures: $(lipo -archs "$exe")"
echo "  signature: $(codesign -dv "$bundle" 2>&1 \
        | awk -F= '/^(Signature|TeamIdentifier|Identifier)=/ {printf "%s%s=%s", sep, $1, $2; sep=", "}')"
# The Rust core exports its whole runtime, so the list itself is too long to
# read: its size and digest say whether two builds export the same things, and
# the C-level names are the ones a boundary change would move.
exported="$(nm -gU "$exe" | awk '{print $3}' | sort)"
factory="$(key CFPlugInFactories | sed -e '1d;$d' | awk '{print $3}' | head -1)"
echo "  exported symbols: $(wc -l <<<"$exported" | tr -d ' '), list sha256 $(shasum -a 256 <<<"$exported" | cut -c1-16)"
echo "  factory exported: $(grep -q -x "_$factory" <<<"$exported" && echo yes || echo NO)"
echo "  engine and transport symbols exported: $(grep -c -E '^_emu_(engine|enumerate|find)_' <<<"$exported" || true)"
echo "  links:"
otool -L "$exe" | sed '1d' | awk '{print "    " $1}'

# Who owns the hardware. A one-part HAL reaches IOKit's USB plug-in interface
# itself and needs no service; a split HAL should show the reverse.
undefined="$(nm -u "$exe")"
count() { grep -c -E "$1" <<<"$undefined" || true; }
echo "  USB ownership: $(count '_IOCreatePlugInInterfaceForService|_IOServiceAddMatchingNotification') IOKit device symbols"
echo "  service dependency: $(count '_xpc_|_bootstrap_look_up|_launch_') XPC/launchd symbols"
