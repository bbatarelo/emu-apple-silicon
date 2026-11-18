# Optional split driver verification

## Tracker Pre hardware run on `14dd024`, November 18, 2026

**Engine-only reload worked on the Mac. Latency parity is still open.** The new launchd process claimed USB and streamed real stereo audio. During reload, the engine PID changed from 45593 to 45681. The Core Audio daemon and HAL host kept PIDs 45615 and 45628. The original audio and MIDI bundles matched the preflight file hashes after the run, and the engine service plist was removed. [Reload log](../../captures/latency/2026-11-18-split/reload.log)

The test measured the extra analog delay of forwarding input 2 through the Core Audio callback to output 1. All 18 recordings used 48 kHz, the requested buffer, device clock, 5 ms input and 4 ms output safety. All eight chirps returned on both channels in every recording. The two one-part legs used identical HAL binaries. The entries below give median delay in milliseconds and the number of recordings that passed every probe check. The last column uses the original frame estimates at 48 kHz to avoid millisecond rounding. [Raw recordings, settings and engine snapshots](../../captures/latency/2026-11-18-split)

| Buffer | A1 one-part | B two-part | A2 one-part | B minus A1 / B minus A2 |
|---|---|---|---|---|
| 512 frames | 34.525 ms · 3/3 | 35.609 ms · 0/3 | 35.484 ms · 3/3 | +52 / +6 frames (+1.083 / +0.125 ms) |
| 128 frames | 18.525 ms · 0/3 | 19.609 ms · 0/3 | 18.484 ms · 0/3 | +52 / +54 frames (+1.083 / +1.125 ms) |

The A-to-A medians differed by 46 frames at 512 and 2 frames at 128. The HAL output-to-input callback separation was identical in all three legs at each buffer size (1456 and 688 frames respectively). At 128, the B result lay about 1.1 ms above both controls. The run cannot isolate how much of that came from the split.

### Why the six B recordings are one transport session

The split silent restart suite stopped at cycle 12 when the transport failed to become idle. Before the first chirp, and after every B recording, the Tracker Pre still had one HAL IO client and the engine was streaming. The engine reported generation 12 throughout, with its played-frame count advancing between recordings. A1 and A2 had zero clients and a stopped engine at every post-run checkpoint. On B, the 0404 was also the Mac's default output and was streaming in four of six post-run engine snapshots. The recorded data do not identify the client retaining the Tracker Pre. An application-level IOProc start in this state does not exercise a new USB session. [B preflight](../../captures/latency/2026-11-18-split/B/preflight.txt) · [B engine state after the last run](../../captures/latency/2026-11-18-split/B/128-3-engine.json)

### What failed

- All six one-part 128-frame recordings had one input and one output sample-time discontinuity. All six B recordings had continuous callback sample times. B was already streaming before the first recording, so startup behavior differed.
- The live written/bound counter comparison failed in every B recording. The HAL reads its own written count live and the engine publishes its bound count roughly every 50 ms. Each later engine snapshot showed every pumped frame bound, zero HAL drops, zero bind races and a maximum queue depth of one. That establishes eventual equality at the checkpoint. It cannot prove every frame met its USB deadline. The largest observed per-recording pump delay was 0.482–0.542 ms. Playback underrun counters were already advancing with no benchmark IOProc running, so attributing the post-run totals to the chirps would overstate loss. [Counter comparison](../../captures/latency/2026-11-18-split/B/512-1.log) · [Engine readback](../../captures/latency/2026-11-18-split/B/512-1-engine.json)
- One B recording at 512 recorded **120 input-underrun frames during the probe window**. Later statistics confirm the event. It merits investigation separately from the written/bound sampling issue. [Recording](../../captures/latency/2026-11-18-split/B/512-3.log) · [Engine readback](../../captures/latency/2026-11-18-split/B/512-3-engine.json)
- Ordinary silent restarts passed 19/20 in A1. The failed check had one more 512-frame buffer bound than written in the sampled interval. In B, 2/12 completed cycles passed the live counter assertion, then the idle check timed out. The injected startup-fault suite never ran in either mode. The restart helper carried an older build stamp (`33fe611`), while the live HAL identified itself as `14dd024` and the helper source was identical across those revisions. [A1 restart log](../../captures/latency/2026-11-18-split/A1-silent-restarts.log) · [B restart log](../../captures/latency/2026-11-18-split/B-silent-restarts.log)

Before attributing a fixed latency cost to the architecture, measure fresh Tracker Pre generations with zero clients and a stopped transport at both boundaries. Keep the 0404 idle for the single-device comparison. Run the restart stress separately from the chirps. If the Tracker stays active, record its owning client and start/stop events. Compare submitted and bound frames over the same completed work before evaluating transport loss, while retaining the independent capture-underrun and waveform checks.

### Independent reproductions for the stability milestone

Two hardware-free checks against the unmodified implementation at `14dd024` narrow the first fixes.

- A 512-frame WriteMix was submitted through the real HAL and remote proxy. A simulated successful USB bind consumed the corresponding production queue record. Before the next engine stats publication, the public HAL dictionary returned 512 frames written and zero bound. Publishing the counters yielded 512/512 with no further audio. The two reads were made with the fixture paused. The diagnostic mismatch can therefore occur without audio loss.
- Two registered clients started on one device. Removing client 11 without an earlier StopIO correctly left client 22 playing. When client 22 then called StopIO, one client remained counted and the engine continued running. The current RemoveDeviceClient implementation ignores the removed client's ID. This reproduces a stale-owner path. The November 18 hardware capture does not establish that Core Audio delivered that callback sequence during cycle 12.

Retest with the ordinary and injected hardware suites, with genuine unbound frames still visible and with a second active client surviving the other client's departure.

## Independent results for commit `3a17426`, November 18, 2026

Both modes built from a fresh clone of the feature commit. The default HAL, split HAL, engine executable and MIDI plug-in all passed `codesign --verify --strict`. Alternating the build modes preserved both HAL binary hashes. The two HALs expose the same bundle identifier and factory. Inspection of their imports found USB device access only in the one-part HAL and Mach-service calls only in the split HAL.

| Check | Result |
|---|---|
| `make`, `make MODE=two-part`, then `make -j4 test-rust test-analysis test-regression test-build` | Passed. Includes 74 Rust tests, 15 signal-analysis checks, USB abort/startup invariants, the HAL surface snapshot and compilation of the hardware test programs. |
| Direct lookup through the one-part HAL factory | The attached Tracker Pre and 0404 returned the same serial-based UIDs, six sample rates and 48 kHz safety offsets as before the change. |
| Independent stalled-playback fixture | The production pump bound 32,768 correct frames. The producer rejected one 512-frame write when full. Zero frames reached the binder under the wrong timestamp. |
| Independent HAL client replacement and clock fixtures | A replacement client successfully started a running transport. A late stop from the old client left it running. Tracker Pre's clock call took under 0.01 ms during a 250 ms start on the 0404. |
| `make test-split` in Codex | Shared playback and missing-engine checks passed. The logged-in user's launchd namespace refused fixture bootstrap (error 5). The runner reported the affected cases as unexecuted and exited nonzero. |
| Fable's two-process run in the logged-in session | Fable reported eight consecutive runs of 131 checks with zero failures. Both ASan and UBSan runs passed. TSan reported the pre-existing registry race and two reports on queue-serialized state that Fable judged instrumentation artifacts. Those two reports have not been independently resolved. |

At the time of the offline checks the installed HAL was the earlier `9b96335-dirty` build. The subsequent hardware results are recorded above.

## Defects reproduced during implementation

These observations were taken during implementation. The three tests exercise production paths against a fake audio source or host without starting USB streaming.

**Independent retest after Fable's PCM ownership and per-device lifecycle changes:** all three cases below passed. On the same 65-write stall, 64 records delivered 32,768 correct frames and one 512-frame write was refused when full. No incorrectly placed frame reached binding. `StartIO` for the new client returned success with an actually running transport, and the deferred old stop left the replacement running. The Tracker Pre clock call returned in under 0.01 ms while the 0404 start was still held for 250 ms. These are hardware-free correctness tests.

| Case | Observed result | Acceptance criterion |
|---|---|---|
| Playback pump resumes after its PCM ring wraps | With 65 sequential stereo writes of 512 frames held before the first pump pass, 65 records reached the real pump's output-binding call. Of 33,280 frames, **512 had the next lap's samples** at the earlier timestamp. The stale counter incremented after binding. | Each timestamp handed to the USB binding code has the samples originally published for it. Count or drop missed frames before binding. The producer cannot overwrite PCM the consumer is still using. |
| A new HAL client starts after engine loss | The driver returned `noErr` from the new client's `StartIO` while the fake transport's running flag was false. In a second scenario the device was withdrawn and republished, a new client started, and a late `StopIO` from the old client stopped the new transport. | A successful new start represents a live engine session. Stops and IO from the previous session cannot change its replacement. Exercise the public HAL entry points with distinct old and new client IDs. |
| Clock of a streaming unit while another unit starts | With Tracker Pre already running, an independent fake transport held the 0404 start for 250 ms. The Tracker Pre's public `GetZeroTimeStamp` call took **261 ms**. `StartIO` holds the process-wide `gStateMutex` across the blocking engine start. The new remote path can spend several seconds waiting for IPC. | An in-flight control operation on one unit must allow the other unit's clock and realtime callbacks to continue. Exercise the actual public entry points with a delayed 0404 start and a concurrently streaming Tracker Pre. |

The playback check traced actual samples through `emu_shared_write` and `pump_thread` to the function that binds samples into a USB request. Each stereo frame had a unique value based on its original frame index. This deliberately stalls the pump longer than the shared ring coverage to exercise its recovery. The client check used the production HAL interface and the feature's fake host. It first delivered the proxy's engine-loss callback while client 11 was open and started client 22. It then exercised registry withdrawal/republication, started client 33, and delivered client 11's deferred stop. The existing release HAL also has a shared-client-count assumption. A process split introduces an additional way for a session to end while the client remains alive.

### Build and deployment

- **A retry accepted an incomplete split bundle.** In a fresh, disposable build directory an intentionally invalid compiler flag made the split HAL recipe exit 2. Retrying the same target with ordinary flags exited 0 and reported the bundle directory up to date. `Contents/MacOS/EMUTrackerPre` was missing. A build failure must leave a target that Make retries. Validate by failing once, retrying with valid flags and verifying the complete signed bundle before installation.
- **The installed service depends on an executable removed by ordinary cleanup.** The split installer points launchd at `$(abspath $(ENGINE))` inside `build/two-part`. The inspected `make -n clean` deletes that tree unconditionally. Preserve the launchd executable on cleanup or refuse cleanup for the checkout an installed service uses. On reload, check that the registered executable belongs to the checkout being reloaded.

In the first independent split build, the production HAL, engine, default HAL and MIDI bundle built and verified with `codesign --verify --strict`. The real-device one-part inventory matched the pre-change baseline, and the split HAL declared the E-MU Mach service. A split process suite completed in Fable's GUI session. An independent attempt from the Codex shell reported `launchctl bootstrap` error 5. Its client continued after setup failure and later exited with a segmentation fault. Treat this run as a failed fixture setup. The test runner should exit the phase as soon as bootstrap fails.

**Independent build and deployment retest:** after the compiler failure, a normal retry produced the complete split bundle and engine, and strict signature verification passed. With a disposable service plist, the actual cleanup guard rejected its owning build directory and accepted an unrelated directory. The revised XPC test runner reported the launchd setup failure and skipped the affected client. Its missing-engine cases passed. Both modes built in a second isolated checkout with matching baseline one-part device properties. The ordinary Rust, analyzer, USB and HAL surface checks passed there.

## Baseline independently verified

- Clean default audio and MIDI build, release bundle signatures, 74 Rust tests, 15 signal-analysis checks and the USB abort/startup regression passed. The early HAL property snapshot also passed from a separate build directory.
- A standalone host loaded the production one-part HAL and called its public factory, initialization and property methods. The actual attached Tracker Pre and 0404 each published their distinct serial-based UID, six rates from 44.1 to 192 kHz, and 240/192 input/output safety frames at 48 kHz.
- The installed older audio bundle on this Mac identifies itself as `0.1.1 (9b96335-dirty)`. Its executable SHA-256 is `248dab7fb1d6048f281c920ec487eaa29fa20ce79412a36bf32b634b6dc466ba`. It is not a matched control for the feature-branch binary.

The standalone host above exercises device discovery. Actual IOProc scheduling, split deployment, engine replacement on the live host and the analog A/B comparison are separate checks.
