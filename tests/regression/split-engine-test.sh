#!/usr/bin/env bash
#
# Runs the two-process tests of the optional two-part driver.
#
# The engine under test is the production service over a pretend device,
# started by launchd under a throwaway name in this user's own domain and
# removed again afterwards. Nothing is installed, nothing runs as root, no USB
# device is opened and coreaudiod is not involved.
#
#     split-engine-test.sh <dir holding fake-engine and split-engine-test>

set -euo pipefail
build="$(cd "${1:?usage: split-engine-test.sh <build dir>}" && pwd)"
domain="gui/$(id -u)"

if ! launchctl print "$domain" >/dev/null 2>&1; then
    # No per-user launchd domain: an ssh session or a CI runner without a
    # login. The engine cannot be started as a service there, so these tests
    # did not run -- which is not the same as passing.
    echo "split-engine-test: NOT RUN -- no $domain launchd domain in this session" >&2
    [[ -n "${EMU_ALLOW_SKIP:-}" ]] && exit 0
    echo "  set EMU_ALLOW_SKIP=1 to let the rest of the suite pass without them" >&2
    exit 1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/emu-split-test.XXXXXX")"
service=""
cleanup() {
    [[ -n "$service" ]] && launchctl bootout "$domain/$service" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

# phase <mode> <start an engine: yes|no> [<ABI the engine should claim>]
phase() {
    local mode="$1" engine="$2" abi="${3:-}"
    service="net.batarelo.EMUTrackerPre.Test.$(uuidgen | tr -d - | cut -c1-12)"
    local bus="$work/bus-$mode" log="$work/engine-$mode.log" plist="$work/$mode.plist"
    printf '3f0a 14100000 E-MU-C7-3F0A-FAKE0001\n3f04 14200000 E-MU-69-3F04-FAKE0002\n' > "$bus"

    if [[ "$engine" == yes ]]; then
        cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>Label</key><string>$service</string>
    <key>ProgramArguments</key><array><string>$build/fake-engine</string><string>$service</string></array>
    <key>MachServices</key><dict><key>$service</key><true/></dict>
    <key>EnvironmentVariables</key><dict>
        <key>EMU_FAKE_BUS</key><string>$bus</string>
        ${abi:+<key>EMU_ENGINE_FAKE_ABI</key><string>$abi</string>}
    </dict>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>1</integer>
    <key>ProcessType</key><string>Interactive</string>
    <key>StandardErrorPath</key><string>$log</string>
</dict></plist>
EOF
        # Checked by hand: `set -e` does not apply inside a function whose
        # result is tested, and a client run against an engine that never
        # started proves nothing.
        if ! launchctl bootstrap "$domain" "$plist"; then
            echo "split-engine-test: NOT RUN ($mode) -- launchd would not start the test engine in $domain" >&2
            service=""
            [[ -n "${EMU_ALLOW_SKIP:-}" ]] && { not_run=1; return 0; }
            return 1
        fi
    fi

    local rc=0
    EMU_ENGINE_SERVICE="$service" EMU_FAKE_BUS="$bus" EMU_ENGINE_BIN="$build/fake-engine" \
        "$build/split-engine-test" "$mode" || rc=$?

    if [[ "$engine" == yes ]]; then
        launchctl bootout "$domain/$service" >/dev/null 2>&1 || true
        if [[ $rc -ne 0 && -s "$log" ]]; then
            echo "--- engine stderr ---"; cat "$log"
        fi
    fi
    service=""
    return $rc
}

failed=0
not_run=0
"$build/split-engine-test" shared || failed=1
phase main   yes     || failed=1
phase abi    yes 99  || failed=1
phase absent no      || failed=1

if [[ $failed -ne 0 ]]; then
    echo "split-engine-test: FAILED" >&2
    exit 1
fi
if [[ $not_run -ne 0 ]]; then
    echo "split-engine-test: the two-process phases were NOT RUN (EMU_ALLOW_SKIP); only the in-process ones passed"
    exit 0
fi
echo "split-engine-test: all phases passed (two real processes; no USB, nothing installed)"
