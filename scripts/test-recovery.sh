#!/usr/bin/env bash
#
# Integration test for the engine's fault recovery, against real hardware.
#
# This exists because the failure it covers -- the USB engine dying while Core
# Audio carries on streaming into requests nobody transmits -- took a day and a
# half to notice and produced no error anywhere. It cannot be reproduced by
# unplugging something on cue, so the engine can be told to fail submissions on
# purpose, and this checks it does the right thing about it.
#
#   1. transient fault  -> the stream is rebuilt, audio continues
#   2. persistent fault -> the engine gives up and the device is marked dead
#   3. fault cleared    -> the device comes back
#
# Needs the driver installed and something playing to it: a fault can only be
# injected into a stream that exists.

set -u
BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/bin"
CHECK="$BIN/hal-check"
[[ -x "$CHECK" ]] || { echo "build the tools first: make" >&2; exit 1; }

counter() { "$CHECK" 2>/dev/null | awk -v k="$1" '$1 == k { print $2 }'; }
settle()  { python3 -c "import time,sys; time.sleep(float(sys.argv[1]))" "$1"; }

FAILURES=0
expect() {                       # expect <label> <key> <op> <value>
    local label="$1" key="$2" op="$3" want="$4" got
    got="$(counter "$key")"; got="${got:-0}"
    if { [[ "$op" == eq ]] && [[ "$got" == "$want" ]]; } ||
       { [[ "$op" == ge ]] && [[ "$got" -ge "$want" ]]; }; then
        printf "  \033[32mPASS\033[0m  %-34s %s = %s\n" "$label" "$key" "$got"
    else
        printf "  \033[31mFAIL\033[0m  %-34s %s = %s (wanted %s %s)\n" \
               "$label" "$key" "$got" "$op" "$want"
        FAILURES=$((FAILURES + 1))
    fi
}

streaming="$(counter engineStreaming)"
if [[ -z "$streaming" ]]; then
    echo "this driver has no engineStreaming counter -- install the current build:" >&2
    echo "  make install" >&2
    exit 1
fi
if [[ "$streaming" != "1" ]]; then
    echo "the engine is not streaming." >&2
    echo "Select the device as output, play something, and run this again." >&2
    exit 1
fi

echo "Preconditions"
"$CHECK" fault none >/dev/null; "$CHECK" reset >/dev/null; settle 0.5
expect "engine is streaming"      engineStreaming eq 1
expect "device is alive"          engineAlive     eq 1
expect "no recoveries yet"        recoveries      eq 0

echo
echo "1. Transient fault: the stream should rebuild itself"
"$CHECK" fault transient >/dev/null; settle 5
expect "rebuilt at least once"    recoveries      ge 1
expect "streaming again"          engineStreaming eq 1
expect "still alive"              engineAlive     eq 1

echo
echo "2. Persistent fault: the engine should give up and say so"
"$CHECK" fault persistent >/dev/null
settle 14                        # six attempts with the engine's backoff, plus slack
expect "not streaming"            engineStreaming eq 0
expect "marked NOT alive"         engineAlive     eq 0
expect "rebuilds were attempted"  recoveryFailures ge 1

echo
echo "3. Clearing the fault should bring the device back"
"$CHECK" fault none >/dev/null; settle 2
expect "alive again"              engineAlive     eq 1

echo
if [[ $FAILURES -eq 0 ]]; then
    echo "all checks passed. Play something again to confirm audio is back."
else
    echo "$FAILURES check(s) failed" >&2
    exit 1
fi
