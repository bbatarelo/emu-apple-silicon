# Issue #7: cancelled requests during startup retry

Investigated 2026-09-26 against public repository commit `9b96335`.
Discussion: https://github.com/bbatarelo/emu-apple-silicon/issues/7
Detailed hypothesis: https://github.com/bbatarelo/emu-apple-silicon/issues/7#issuecomment-5638763139

## Verdict

The unsafe callback behavior is reproducible in the production C handlers with
an in-memory USB interface. The intermittent YouTube silence is **not** reproduced
on this machine, and its causal connection remains a hypothesis. Model provenance
is not evidence; the source and the tests below are the basis for this finding.

The initial investigation did not change or install the production driver.
The fix and trial instructions below describe subsequent branch changes.
All development is in this public repository; the original `emu` repository is
archived and receives no backports.

## Source findings

In `driver/usb_engine.c`, the startup queue loop in `stream_session` responds to
`kIOReturnIsoTooOld` by aborting submitted capture/playback, pumping callbacks for
50 ms, calling `reschedule`, and submitting the same request array again.
Neither `stopping` nor a separate draining state suppresses those callbacks.

Both `capture_complete` and `playback_complete` count frames before their stop
checks. A fully aborted future transfer is counted as if its intervals elapsed.
When stopping is false, both handlers also submit it again. The retry loop can
then prepare/zero and submit a Request whose frame list and buffer belong to the
callback's newly submitted transfer. A later IOKit rejection cannot undo the
buffer writes already performed in submit_playback/submit_capture.

There is no outstanding-transfer count establishing that abort/drain is complete.
A fixed run-loop timeout alone is not an ownership proof. Failure after partial
startup submission calls teardown directly, which destroys buffers before
closing the interface, without the normal abort/drain sequence.

The related runtime recovery path also drains with `faulted=true` but
`stopping=false`; the same callbacks can replenish that queue during recovery.
The callback hazard predates the new startup loop; adding another unsafe caller
exposes it in an additional situation.

## Corrections to the posted analysis

- The ordinary stopping path prevents resubmission, but its checks are **after**
  frame accounting. They are not early returns before all processing.
- Runtime-fault teardown does not set stopping, so not every shutdown drain is safe.
- Returning setup failure to the existing engine thread is insufficient for an
  initial start: `!announced` fails the StartIO handshake immediately. An outer
  startup retry must be designed explicitly if this approach is chosen.
- The bus start is chosen after clock setup, alt selection and allocation; it
  can go stale during the submission burst or scheduling delays afterwards.
- Duplicate queue ownership is demonstrated by the fake controller, not observed
  on the live host controller. Kernel callback order, delay, and the audible
  consequence require a dedicated integration test.

## Reproduce without hardware

```sh
make test-usb-abort
```

The test includes the real usb_engine.c, uses its production completion and
submission functions, and replaces only USB submission methods with an ownership
tracking fake. It does not run a complete stream_session or emulate the controller's
real AbortPipe implementation. It delivers a fully aborted, never-played request,
then models the retry's next submission of that same slot.

Observed at 48 kHz, two packet entries per request:

| State | Phantom frame advance | Callback resubmits | Next retry reuses owned request |
|---|---:|---:|---:|
| Startup, playback | 96 | yes | yes |
| Startup, capture | 96 | yes | yes |
| Recovery fault, playback/capture | 96 each | yes | not tested |
| Stopping, playback/capture | 96 each | no | not tested |

The normal successful playback control passes: 96 frames accounted and queue
replenished. Twelve safety assertions fail on current production code. The command
intentionally exits nonzero and is separate from `make test` until the defect is
fixed. These are twelve assertion failures, not twelve independent bugs.

## Installed-driver smoke test

```sh
EMU_DEVICE=3F04 make test-hal-restart
```

Requires the selected device to be idle. Creates a Core Audio IOProc, submits
silence, stops and waits for the engine to stop, twenty times. It leaves the
sample rate, volume and default output unchanged. It verifies advancing output,
matching framesBound/framesToOutput deltas, zero new unmapped frames, and streaming
state after 400 ms settling. Per-cycle snapshots are approximate concurrent
measurements; an isolated mismatch merits a longer trace, not an automatic root
cause claim. This cannot prove analog output quality or simulate YouTube seeking.

Live run: installed 0404, 48 kHz, input on, version `0.1.0 (git 7baf923-dirty)`:
20/20 passed, startIOCalls increased 1 through 20, resyncs stayed zero, and each
measured window had framesBound == framesToOutput and unmappedFrames == 0.
The installed binary is not identified as a clean current checkout. This is
baseline evidence for that installed build, not validation of a rebuilt fix.
No matching stale/failure/rebuild messages were returned from the hour preceding
the test; absence of retained logs is not proof that the path never ran.

## Recommended fix and next integration coverage

1. Give every accepted request explicit outstanding ownership. Route setup
   failure, setup retry, runtime recovery and StopIO through one quiesce path.
   Prevent accounting/resubmission for cancelled future requests while draining,
   and account any already completed portion according to its actual bus interval.
2. Wait for outstanding completions, rather than assuming a 50/300 ms delay is
   enough. Do not reuse/free buffers while callbacks or the controller own them.
   On drain failure, use a verified cancellation/close policy rather than freeing
   and hoping. Treat feedback transfers with the same ownership rules.
3. Preserve the adaptive schedule lead, but restart from consistent cursors and
   anchor after quiescence. If replacing the inner retry with a new session,
   add bounded initial-start retries and preserve the StartIO handshake semantics.
4. Add a one-shot test fault for a stale setup after N successful submissions
   (capture, playback, and playback-only). Also simulate delayed abort callbacks
   past 50 ms in the fake backend. Verify no duplicate submit, no free while owned,
   no cancelled-future frame accounting, and resumed successful binding.
5. Repeat live silent restarts, existing recovery tests, concurrent devices and
   analog loopback after installing the fix. Log setup attempts, requested lead,
   pending transfers and drain outcome with the per-device identity.

No CPU-stress or new installed-driver fault injection was run. A targeted startup
fault is more reproducible than trying to force scheduling delays with load.

## Fix branch and owner trial (0.1.1)

Branch: `codex/fix-usb-abort-drain`. No backports and no driver installation by
the agent. The earlier results above describe the unfixed baseline.

The fix tracks ownership per request and in aggregate, guards submits before
buffer mutation, and returns ownership before processing completions. Draining,
stopping, faulted and aborted completions cannot account or resubmit transfers.
All teardown paths close the IO writer gate and drain capture, playback and
feedback before destroying buffers. Async sources are owned by the engine so
partial setup failures release them too; released resources are cleared even
between ordinary stop/start cycles.

Startup retains four bounded attempts and adaptive 4/16/64/256 ms lead. After
quiescing it resets the planned cursors, feedback planner and anchor together at
the same resume frame. Runtime recovery preserves its frame epoch. Explicit
feedback cancellation still works after that endpoint has been disabled.

The hardware-free regression is now included in `make test` and passes. It also
executes the actual startup retry with completions delayed by 75 ms (beyond the
old 50 ms pump), duplex and playback-only, exhausted retries, and feedback drain.

Build/install from this branch:

```sh
make all
make test
make install
```

With other playback/recording stopped, run against either connected device:

```sh
make build/bin/hal-restart-test
EMU_DEVICE=3F04 build/bin/hal-restart-test --startup-stale
EMU_DEVICE=3F0A build/bin/hal-restart-test --startup-stale
```

Each test arms a one-shot fault after the first accepted playback request,
provoking partial-start abort/drain/retry on each of twenty silent stream starts.
It checks resumed binding and consumption of the fault. No rate, volume or default
output changes. An older installed driver rejects this new fault command.

For a single manual trial, arm `build/bin/hal-check fault startup-stale` while
idle, then start playback. `fault none` disarms it. Arming during playback waits
until a later setup; it does not interrupt the existing stream. Logs should show
injection, a stale schedule/drain, then startup retry success under the unit name:

```sh
/usr/bin/log show --last 10m --info --predicate 'subsystem == "net.batarelo.EMUTrackerPre"'
```

Then play music and run `make test-recovery` for the existing fault lifecycle test.
Use normal listening, seeking, rate changes, and unplug/replug during the trial.
Record the exact driver version, device, rate and fault counters if audio stops.

### Drain policy limitation

The driver does not free buffers just because a timer expires. It waits until
all accepted requests return via callbacks, with a periodic error log if stalled.
If the USB stack never returns a completion, StopIO can remain blocked. This is
an explicit safety tradeoff, not a claim of bounded shutdown; hardware trials
must include unplug/replug and recovery. A future bounded failure policy must
retain ownership of outstanding allocations until cancellation is guaranteed.

### Independent review

Claude Opus 5.5 (`claude-opus-5-5`, high effort) reviewed the patch and source
without editing tools. It judged the request ownership/drain design sound and
found two concrete defects: partial source setup could remove a source from a
NULL run loop, and callback-count-based warnings could label a healthy drain as
stalled. Both were corrected. Teardown uses the engine thread's current run loop;
warnings begin after two elapsed seconds and repeat every five seconds.

The review also identified startup lead persisting across separate starts and
idle fault readback hiding an armed injection. These were corrected: each new
StartIO begins with the default lead and idle diagnostics read the current fault
mode. StopIO no longer reads the engine thread's unsynchronized run-loop pointer;
it relies on the existing 250 ms wake interval. A regression now exercises partial
source cleanup and idle fault readback. All automated tests and the full build
passed after these changes. The reviewer has not reviewed these follow-up edits.

Remaining hardware checks: repeated injected starts on both models, recovery,
concurrent starts, unplug during setup/playback, and sleep/wake. The installed
baseline smoke test does not validate this new binary. Recovery timeline transients
and the unbounded-drain limitation above remain hardware-test considerations.
