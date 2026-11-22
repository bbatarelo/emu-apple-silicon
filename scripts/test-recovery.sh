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
# Needs the driver installed. A fault can only be injected into a stream that
# exists, so this opens one: a silent duplex IOProc on the unit itself, by UID
# (build/bin/hal-stream). Nothing has to be playing, the system's default
# devices are not touched, and other clients of the unit are left alone --
# though they will hear the faults, which is the point of them.
#
#   EMU_DEVICE=<UID or part of it>      which unit; required when several are attached
#   EMU_RATE=<Hz> EMU_BUFFER=<frames>   run at these settings; both are restored

set -u
BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/bin"
CHECK="$BIN/hal-check"
STREAM="$BIN/hal-stream"
[[ -x "$CHECK" && -x "$STREAM" ]] || { echo "build the tools first: make test-build" >&2; exit 1; }

# One unit, by its whole UID, for everything below.
EMU_DEVICE="$("$STREAM" --uid)" || exit 2
export EMU_DEVICE
echo "unit: $EMU_DEVICE"

stream_pid=""
stream_out="$BIN/../recovery-stream.log"     # overwritten by each run
stream_stop() {
    if [[ -n "$stream_pid" ]]; then
        kill -TERM "$stream_pid" 2>/dev/null
        wait "$stream_pid" 2>/dev/null
        stream_pid=""
    fi
}
# Starts the test's own stream and waits until the engine is on the bus.
stream_start() {
    "$STREAM" ${EMU_RATE:+--rate "$EMU_RATE"} ${EMU_BUFFER:+--buffer "$EMU_BUFFER"} > "$stream_out" 2>&1 &
    stream_pid=$!
    for _ in $(seq 1 100); do
        if ! kill -0 "$stream_pid" 2>/dev/null; then
            cat "$stream_out" >&2; stream_pid=""; return 1
        fi
        if grep -q '^streaming' "$stream_out" && [[ "$(counter engineStreaming)" == 1 ]]; then
            sed 's/^/  /' "$stream_out"; return 0
        fi
        settle 0.1
    done
    cat "$stream_out" >&2
    return 1
}
finish() {
    stream_stop
    "$CHECK" fault none >/dev/null 2>&1      # never leave a fault armed behind a failed run
}
trap finish EXIT
trap 'exit 130' INT TERM

counter() { "$CHECK" 2>/dev/null | awk -v k="$1" '$1 == k { print $2 }'; }
settle()  { python3 -c "import time,sys; time.sleep(float(sys.argv[1]))" "$1"; }

FAILURES=0

# Audio actually moving, not merely counters that look plausible.
#
# This exists because a refactor once left the engine streaming, recovering and
# reporting every health counter correctly while playing pure silence: Core
# Audio's writes and the USB requests were on different timelines, so every
# frame was dropped as unmapped. The whole suite passed. framesBound is the
# frames that reached a request, and it has to keep up with what Core Audio
# handed over and keep moving.
expect_audio() {
    local label="$1" a b u0 u1
    a="$(counter framesBound)";     a="${a:-0}"
    u0="$(counter unmappedFrames)"; u0="${u0:-0}"
    settle 1.5
    b="$(counter framesBound)";     b="${b:-0}"
    u1="$(counter unmappedFrames)"; u1="${u1:-0}"

    # Both figures are deltas over the window, deliberately. unmappedFrames is
    # cumulative and a rebuild legitimately adds to it: Core Audio keeps writing
    # across the gap where no request exists to carry the audio, so those frames
    # have nowhere to go and are counted. Requiring a lifetime zero would fail a
    # correct recovery. What must be true afterwards is that it stops growing.
    if [[ "$b" -gt "$a" ]] && [[ "$((u1 - u0))" -eq 0 ]]; then
        printf "  \033[32mPASS\033[0m  %-34s framesBound +%s, unmapped +%s\n" \
               "$label" "$((b - a))" "$((u1 - u0))"
    else
        printf "  \033[31mFAIL\033[0m  %-34s framesBound +%s (want >0), unmapped +%s (want +0)\n" \
               "$label" "$((b - a))" "$((u1 - u0))"
        FAILURES=$((FAILURES + 1))
    fi
}

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
if ! stream_start; then
    echo "could not start a stream on $EMU_DEVICE" >&2
    exit 1
fi

echo "Preconditions"
"$CHECK" fault none >/dev/null; "$CHECK" reset >/dev/null; settle 0.5
expect "engine is streaming"      engineStreaming eq 1
expect "device is alive"          engineAlive     eq 1
expect "no recoveries yet"        recoveries      eq 0
expect_audio "audio is reaching the device"

echo
echo "1. Transient fault: the stream should rebuild itself"
"$CHECK" fault transient >/dev/null; settle 5
expect "rebuilt at least once"    recoveries      ge 1
expect "streaming again"          engineStreaming eq 1
expect "still alive"              engineAlive     eq 1
expect_audio "audio is flowing again"

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

# A stream that was running when the device died does not come back by
# itself: its client has to open it again. Do that, rather than leave it as
# an instruction.
echo
echo "4. A stream opened again should play"
stream_stop
if stream_start; then
    expect "streaming"                engineStreaming eq 1
    expect_audio "audio is reaching the device"
else
    printf "  \033[31mFAIL\033[0m  %-34s\n" "the stream did not start again"
    FAILURES=$((FAILURES + 1))
fi
stream_stop
for _ in $(seq 1 40); do [[ "$(counter engineStreaming)" == 0 ]] && break; settle 0.1; done
expect "transport idle afterwards"  engineStreaming eq 0
expect "no client left behind"      ioClients       eq 0

echo
if [[ $FAILURES -eq 0 ]]; then
    echo "all checks passed."
else
    echo "$FAILURES check(s) failed" >&2
    exit 1
fi
