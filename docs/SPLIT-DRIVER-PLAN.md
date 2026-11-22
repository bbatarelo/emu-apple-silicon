# Optional split E-MU audio driver

## Completed milestone — a persistent, hardware-qualified development driver

**Passed November 22, 2026 on `73ad8f6`.** The two-part driver remains installed on the development Mac. UID-targeted tests passed on the Tracker Pre and 0404, including 140 restart cycles across the two devices and the Tracker Pre's 45 analog checks. An unprivileged engine reload preserved the Core Audio and HAL host PIDs. The report includes a separate 0404 capture-underrun observation. [Hardware results and daily commands](issue-investigations/SPLIT-DRIVER-VERIFICATION.md#development-milestone-passed-on-november-22-2026)

Install the optional split driver on the development Mac and leave it there for use. The README's plain `make` and `make install` remain the one-part release workflow. The first development install registers the service and loads the HAL. After that, an ordinary engine edit must build, reload and run against a named interface under the developer account while the Core Audio daemon and its HAL host keep their PIDs. A change to the installed HAL or shared ABI is a distinct deployment. Defer latency tuning and further A/B installs until this milestone passes.

Claude Fable 5.2 owns driver implementation. Codex owns independent verification. Review the [November 18 hardware evidence](issue-investigations/SPLIT-DRIVER-VERIFICATION.md) and reproduce the two observed failures before choosing fixes. One compares different-time counter snapshots. The other is a Tracker Pre transport still running after its test IOProc stops. Trace the actual owning client and start/stop/remove sequence, including legitimate concurrent clients and late stops. Keep the existing assertions and one-part behavior. Diagnostics must count actual submitted, bound, unmapped and dropped work. A delayed, lost or unbound real frame must remain visible. Qualify stream teardown and the observed capture underrun separately.

| Acceptance | Evidence needed |
|---|---|
| Out-of-box split install | Both attached serial UIDs publish, the selected device streams input and output, MIDI remains available on the 0404, and the system input/output/default sound-effect selections are preserved. |
| Existing tests | `make test` passes. Run the existing ordinary restart and injected-startup suites (20 cycles each), recovery checks and the applicable existing analog checks on the Tracker Pre via its UID. Report failures as failures. Exercise the 0404 by UID for hardware-independent-of-cabling checks without using it as a substitute for the Tracker Pre. Analog and DIN loopback need their respective physical cables. |
| No manual device switching | Open an IOProc on the selected UID, including the stream needed by the recovery test. Start and stop the test's own stream. Automate any required rate/buffer settings and restore them. Keep other clients and the system defaults intact. |
| Daily engine-only loop | Reload the installed engine as the developer, record the new engine PID and identical Core Audio/HAL PIDs, then rerun the selected-device checks on that new engine. Leave the split driver installed. |
| Release default | Plain `make` still builds the one-part HAL and MIDI driver. Run its existing offline checks. |

For the first hardware diagnosis, use the loaded host's client lifecycle events and unit diagnostics. Distinguish an actual second client from a stale client entry. Check the 50 ms engine statistics publication against the HAL-local counter and reproduce the disagreement with the actual two-process path. Keep regression coverage for real dropped frames, a client disappearing, another valid client still playing, late stops and concurrent devices. Run the existing loopback and recovery fixtures directly with the selected UID. Record the revision and individual results. A hardware-free pass or a recorded chirp alone does not qualify the driver.

### Milestone work by Claude Fable 5.2, 18 November 2026

At the November 18 handoff, the changes were in the working tree on `22e69ba` and subsequently committed as `73ad8f6`. Fable reported passing `make`, `make MODE=two-part`, `make test` and ASan/UBSan checks on the regression and two-process suites. The install and hardware qualification followed.

**The stale client was my design, not a lost callback.** Measured on this Mac against the installed one-part `9b96335-dirty`, with two processes running silent IOProcs on the Tracker Pre: `startIOCalls` went 0 → 1 when the first started and stayed 1 through the second starting and the first stopping; the transport stopped when the last one stopped. coreaudiod sends one `StartIO` for the first client and one `StopIO` for the last, not a pair per client. The per-client ID set added in `3a17426` waited for the starter's own stop. When another client overlapped a restart cycle, the single `StopIO` carried a different ID, was ignored as "not a starter", and the session ran on with one counted client — the cycle-12 state. Which IDs coreaudiod puts on the two calls is not yet observed; the plug-in now logs every add, remove, start and stop with client ID and PID so the first hardware run shows it.

The rule now: a stop ends the session if it comes from the starter **or from any client registered on this adoption of the slot**; it is ignored only from a client that is neither, which is what a stop left over from before an unplug looks like. A removed client's outstanding start is dropped with it. If it was the only start and other clients are still registered, the session is left to them (they may be the ones playing) and ends on their stop or when none is left. The existing `clients_and_sessions` assertions are unchanged and pass.

**The counter skew was two clocks, and one-part had it too.** `framesToOutput` was the plug-in's live count; `framesBound` came from the engine — mid-write in one-part (the 1/20 A1 failure, one 512-frame buffer), up to 50 ms old in two-part (10 of 12 B cycles). Now the engine keeps the books itself and they are read as of one moment between writes:

| Diagnostic | Meaning |
|---|---|
| `framesToOutput` | Frames whose fate is known: offered to a session + `framesRefused` + `framesDropped`. Same reading as `framesBound`, so the hardware suites' `bound == written` compares like with like. |
| `framesRefused` | Handed to an engine with no session to carry them (stopped, tearing down, between a fault and its rebuild). |
| `framesDropped` | Two-part: never reached the engine — store full, oversized, discarded by the pump, or no engine. |
| `framesQueued` | Two-part: written, not yet taken by the engine. |
| `framesUnpublished` | Two-part: taken by the engine, not yet in its published figures (≤ 50 ms). |
| `framesUnreconciled` | Two-part: taken by an engine whose session ended before it published figures covering them. Stays. |
| `framesSubmitted` | The plug-in's own count of every frame Core Audio wrote; never reset. Codex's review asked for it. |

A lost, refused or late frame is in `framesToOutput` and not in `framesBound`, so the unchanged assertion still fails on real loss. A counter reset rebases the books from one coherent reading instead of zeroing them under a write (Codex's reset-race finding). Shared ABI is now 2: HAL and engine must be installed together once.

**UID-targeted workflow.** `build/bin/hal-stream` opens a silent duplex IOProc on the unit `EMU_DEVICE` names, refuses to choose when several match, and restores any rate or buffer it was asked to set. `make test-recovery` starts and stops that stream itself instead of asking for the device to be selected and played to; its existing checks are unchanged, and it adds a fourth step (reopen, audio flows, transport idle and `ioClients 0` afterwards). `make test-integration` resolves the full UID first. `EMU_DEVICE=<uid> make test-device` runs restart, injected startup and recovery in order. `make install MODE=two-part` records the system input, output and sound-effects selections by UID and puts back any that moved.

**New regression coverage.** `test-usb-abort`: 20,000 stats readings against a continuously writing thread, refused and unmapped frames visible, reset under a concurrent write. `test-hal-surface`: one start and a stop from another registered client; a starter removed with another still playing; a sole client removed. `test-split`: one write read across the process boundary before and after publication; a write with no session; an engine that dies after binding a write and before publishing it (256 submitted, 0 written, 256 unreconciled). One existing split assertion changed: the deliberately oversized write is now part of what was handed over, so the post-stop sum includes `framesDropped`.

**Install request (Fable → Codex).** I have no sudo. From this checkout, with nothing playing on either unit:

```sh
make MODE=two-part && make install MODE=two-part     # once; restarts coreaudiod
make engine-status                                    # both units listed, halPid non-zero
log stream --predicate 'subsystem == "net.batarelo.EMUTrackerPre"' --info   # client lifecycle
EMU_DEVICE=3F0A make test-device                      # Tracker Pre: restart ×20, injected ×20, recovery
EMU_DEVICE=3F04 make test-device                      # 0404, cabling-independent only
make reload && EMU_DEVICE=3F0A make test-device       # daily loop: new engine PID, same coreaudiod/HAL PIDs
```

Leave it installed. The first thing to read after a restart suite is `staleStops`, `foreignStops` and `lastStartClient`/`lastStopClient` from `hal-check`: `foreignStops > 0` with the transport idle confirms the cycle-12 mechanism; `staleStops > 0` with the transport still running would mean coreaudiod stops with an ID it never registered, and the rule needs widening. Stream teardown and the 120-frame capture underrun are not addressed here and are still to be qualified separately on hardware.

## Decision and starting point

Keep the README's `make` and `make install` as the release path. The user should get today's in-process audio HAL, separate CoreMIDI bundle and tools, with the existing prerequisites. Add an opt-in development build with a resident HAL and a replaceable, unprivileged E-MU USB engine. The development benefit is the ability to replace that engine while Core Audio and unrelated audio devices stay up. Audio clients using the E-MU may need to reopen.

This design is feasible at the control boundary. Output placement is the main engineering risk. Success means real duplex hardware operation and an observed engine-only replacement. Latency impact is an experimental result.

The source baseline is E-MU `fbf9595`. The local generic template at `29d81c7` supplies an XPC service, process lifetime, deployment and tests. Its example is a singleton 8×8, fixed-48-kHz silent backend. The local FTU checkout is at `0f1f91a` with further working changes. The FTU qualification numbers below come from its **committed** `docs/USBHOST-LATE-FILL.md`. Review and implement in the E-MU feature checkout. The two reference projects are sources for comparison.

## Implementation sequence

The user's architectural point 1 spans stages 1–5 below. Fable owns the implementation through a working optional two-part build, engine reload, and offline tests. Stage 1 only establishes the baseline. Codex owns stage 6 and the Tracker Pre comparison.

The playback-ownership, client-replacement and multi-device clock checks passed on the committed implementation. The [independent verification log](issue-investigations/SPLIT-DRIVER-VERIFICATION.md) records the original failures, retests and outstanding hardware checks.

| Stage | Work | Evidence required |
|---|---|---|
| 1. Establish the baseline | Build the existing Makefile from a clean checkout and run `make test`. Inventory the published UIDs, rates, buffer ranges, clock/input controls, custom properties and per-device diagnostics. | Record source and installed identities separately. Check release defaults again after the split. |
| 2. Define the boundary | Keep HAL object publication and host configuration in `driver/plugin.c`. Provide compile-time local and remote implementations of its engine-facing calls. Include hotplug inventory (`emu_enumerate_units` currently bypasses `usb_engine.h`), gain, fault injection, final counters, running/streaming distinction, hardware time and start/stop. Reuse `driver/usb_engine.c`, the Rust CA0189 core and 0404 MIDI interface ownership. | One-part builds without a service or remote audio staging. Compare symbols, bundle identity, entry point, properties, settings and the default install recipe. Factor common logic only where the two implementations need it. |
| 3. Implement and test the process boundary | Adapt the template's native connection and lifecycle patterns to a versioned, checked E-MU stereo ABI. Give each enumerated unit its own session and generation. Use control IPC outside realtime callbacks. Exchange PCM and coherent device clock observations through preallocated shared memory. Capture is addressed by device sample frame. Playback is a FIFO of the HAL's writes, each tagged with its sample time: a store indexed by sample frame hands the engine another lap's samples once the pump falls a lap behind. | A two-process fixture exercises stereo sample placement, two simultaneous devices, fractional-rate packet cadence, wraparound, startup at negative sample times, incompatible ABI, disconnection, stale clients and death during publication. The HAL must bound all reads, produce defined silence on missing capture, and safely retire a mapping while callbacks are active. |
| 4. Establish the output and capture paths | One-part keeps `WriteMix → emu_engine_write_output → write_output_bind` into engine-owned, scheduled IOUSBLib buffers. For split mode the HAL queues each `WriteMix` (frames, sample time, the time it was written) in shared memory, and an engine-side pump replays the queue into the same `emu_engine_write_output`, so the bind map, its seqlock, its gate and its counters stay the one-part ones. The pump runs on its own real-time timer (500 µs), never from a USB completion: FINDINGS measured a completion-driven fill needing a 10 ms offset where the direct bind needs 4. Capture is written into the shared ring by the existing completion handler, at its original sample position, with no added stage. Preserve current USB packet planning, ZTS period, source, stream policies and declared offsets. | Demonstrate where fresh PCM is copied, when the controller can still consume it, how same-lap races are excluded, and what happens on late/missing PCM. Protect against overwriting a recycled request. Instrument missed fills, late capture, queue depth, bind races, wake-to-copy timing and the furthest interval copied. A fixed-period forwarder that consumes a full extra buffer would change the measurement. |
| 5. Deploy and reload | Offer an explicit selector such as `MODE=two-part` and a split-only reload command. Isolate build artifacts so alternating modes cannot install a stale bundle. Reuse the template's peer checks, non-root engine service and positive shutdown acknowledgement. Use an E-MU-specific service identity. Install exactly one E-MU audio owner, preserve serial-derived UIDs and existing CoreMIDI behavior. Support switching back through the documented release workflow, including from today's installed bundle. | Verify first install, that either mode's install leaves exactly one owner (the bundle name is shared and the release install removes the engine service, so there is no collision to reject), reload with stopped/reopened clients, failed drain, daemon disappearance, and rollback. A reload changes the engine PID/build identity, leaves the Core Audio and HAL host PIDs stable and produces working capture/playback again. A stale stop cannot affect a newer session. Test while the FTU development service is also installed. |
| 6. Independently qualify the branch | Codex reviews the diff against the baseline, rebuilds both variants, and runs the offline and hardware checks below. Keep latency tuning proposals for a subsequent change. | Report hardware-free, live HAL, silent USB, analog, and untested cases separately. Retain failed runs with their counters. |

The USB buffers created by `LowLatencyCreateBuffer` are process-local pointers in the existing binding table. Sending their numeric addresses over IPC is invalid. A genuine cross-process alias of those buffers is a possible optimization only with demonstrated mapping, lifetime and controller-visibility behavior. The shared-PCM fallback has an extra scheduling deadline inside the existing output safety window. Test that deadline before attempting broad refactoring. A version mismatch, missing service or unplug must never block the audio thread. A fresh session after device loss gets a fresh clock seed. An ordinary bus-schedule recovery within an existing session preserves the current continuity rules.

### Review requested from Claude Fable

Before implementation, trace an actual `WriteMix`, capture completion, `GetZeroTimeStamp`, device arrival, start, teardown and engine reload through the proposed boundary. Identify required adjustments in this plan, especially the proposed playback wake/fill schedule, late/recycled request access, post-stop stats, multi-device lifetime and existing HAL mutex scope. Record the model, conclusions and corrections in this file. Then implement point 1 on the feature branch and run its tests. Keep a stable engine-only iteration path that works with the physical Tracker Pre. Codex performs the independent verification.

### Review by Claude Fable 5.2, 18 November 2026

Model `claude-fable-5-2`. Read: `driver/plugin.c`, `driver/usb_engine.h`, `driver/ring.h` and the Makefile in full; in `driver/usb_engine.c` the hot-plug watch, bind map, timeline, statistics, submission, completions, rescheduling, teardown and start/stop, but not its device-open and session-setup code; FINDINGS on the output path. From the references: the template's `docs/ARCHITECTURE.md`, `INVARIANTS.md`, `DEPLOYMENT.md`, `LESSONS.md` and its native `connection.c`, `engine.c`, `control.c`, `peer_policy.h`, `install.py` and XPC tests at `29d81c7`. In FTU at `0f1f91a` only the passages this plan cites were checked: the figures in the committed `docs/USBHOST-LATE-FILL.md` and the reload entry in the local `docs/TEMPLATE-UPSTREAM.md`. The plan's reference facts hold: the FTU figures are in the committed file as quoted, the template is a singleton 8-channel 48 kHz backend with a 16384-frame zero-timestamp period, and its reload-acknowledgement race is the one FTU's local note names.

**Scope.** "Point 1" was read as stage 1 until the sentence above the table appeared in this file during the session. That reading matched the request ("leave installation and the A/B to Codex"), so stages 1–5 are implemented. Nothing was installed and coreaudiod was not touched.

#### Traces through the boundary

| Path | One-part | Two-part, as built |
|---|---|---|
| `WriteMix` | IO thread → `emu_engine_write_output` → gate → `write_output_bind` packs 24-bit into whichever submitted requests cover the range. The IO thread has the whole output safety offset (4 ms) until the packet transmits. | IO thread copies the cycle into the shared FIFO and queues a record: no message, no lock, no wait. The engine's pump takes the record within one pump interval and makes the same `emu_engine_write_output` call. The IO thread's allowance is the offset less the pump's delay. |
| Capture completion | Engine thread unpacks into its ring at `in_cursor`; the IO thread reads by sample time and zeroes behind itself. | The same handler writes the same ring type, which now lives in the shared region (`emu_engine_set_input_ring`). Same thread, same moment, no added deadline. |
| `GetZeroTimeStamp` | Reads the engine's (frames, host time) seqlock, quantises to the 8192-frame period. | Reads the same pair from the shared region; the pump republishes it whenever it changes, and the engine publishes the first one before it answers `start`. A reader gives up after 8 attempts, so an engine that died mid-publication cannot hold it; the plug-in then extrapolates on the host clock under the same seed, which is its existing behaviour for a dead engine. |
| Device arrival | IOKit notification → `reconcile_devices` → `emu_enumerate_units` reads the registry. | The engine watches IOKit and sends its inventory at hello and on change; the HAL's `emu_enumerate_units` returns that. With no engine nothing is published; when one connects the devices appear. |
| Start | `StartIO` → `emu_engine_start` blocks until streams are on the bus (bounded at 8 s). | `StartIO` → `start` over XPC, bounded at 10 s; the engine runs the same `emu_engine_start`, publishes the anchor, starts the pump, replies. No engine: fails at once. |
| Teardown | `StopIO` → `emu_engine_stop` joins the engine thread after the USB drain. | `stop` names the session's generation. The engine stops the pump, then the transport, publishes final figures, replies. A stop for another generation is refused. |
| Engine reload | — | `emu-engine ctl shutdown`: every session stops (USB drained), the reply is sent, and the process exits only once the reply is on its way. launchd starts the new image. The HAL reconnects within about a second and keeps the inventory for 10 s meanwhile, so idle devices, their object IDs and their settings are untouched. A device that was streaming goes not-alive, as it does when the engine gives up one-part. |

#### Corrections to the plan

1. **The fill must not be driven by USB completions.** The plan's "bounded engine-side late-fill" is the staged fill this project already measured and abandoned: under Exposé the completion-to-callback delay exceeds 4 ms a few times a minute, which is why that path needed 10 ms and the direct bind needs 4 (FINDINGS, "What Exposé costs the helper's threads"). The pump therefore runs on its own time-constraint thread against `mach_wait_until`. What the split costs is then the pump interval plus its wake error, taken out of the IO thread's allowance: 4.0 ms becomes about 3.5 ms. Sample placement, declared offsets and the timeline do not change, so **the A/B should show no latency shift; the difference to look for is `outputUnderruns` and `unfilledPlayback` under load at the same offset.** A quiet-machine comparison of medians cannot show it.
2. **Playback cannot be stored by sample frame.** Codex showed the pump binding the next lap's samples after a stall longer than the ring. The store is now a FIFO: frames sit where they were appended, the HAL refuses a write that would reach frames the pump has not released, and a refused write is dropped whole before anything is bound (`halDropped`, `halDroppedFrames`). Sample times may repeat, go backwards or jump a lap; the record carries them.
3. **`gStateMutex` was the wrong lock for start and stop.** `StartIO` held it across `emu_engine_start`, and `GetZeroTimeStamp` takes it on every device's IO thread, so one unit starting froze the clock of another that was playing: 248.7 ms measured against the original plug-in with a 250 ms start. That is a release-driver defect with two units attached, and the remote path would have made it seconds. Start and stop now hold a per-slot lifecycle lock; `gStateMutex` is never held across an engine call that can block. Rate changes and retiring a slot's engine take the same lifecycle lock.
4. **A client count cannot say whose stop is arriving.** With the engine gone and a client still counted, the next `StartIO` returned success without starting anything; and after an unplug and replug, a late `StopIO` from an old client stopped the new client's session. The plug-in now keeps the started client IDs: a session is started for the first client *or whenever the engine is not running*, and a stop from a client that did not start on this adoption of the slot is ignored. Both are one-part behaviours too.
5. **Post-stop statistics have to live on the HAL side.** One-part keeps the last session's figures inside the engine. Here the engine publishes a snapshot into the shared region 20 times a second and at stop; the HAL copies it when a session ends, and keeps its own half-second-old copy of a running session for the case where the engine dies inside a publication. `emuD` therefore reads the same after a stop, a crash or a reload.
6. **The shared region is never unmapped.** "Safely retire a mapping while callbacks are active" is answered by not retiring: one region per unit, owned by the HAL for the life of the host process, with a generation per session. A region whose engine never confirmed stopping (a start or stop that timed out, a pump that stopped beating) is abandoned in place and a fresh one made; it is counted and logged.
7. **A hung engine raises no XPC error.** The pump writes a heartbeat; the HAL takes a session as lost after 2 s without one. Without this the split reintroduces "playing into a void".
8. **Volume must be in force before the first frame.** A new engine starts at unity. The first version applied the HAL's gain after forwarding the first records, which the placement test caught as one full-level buffer after an engine restart. Gain is now applied at start and ahead of the records in every pass.
9. **"Rejected collisions" is replaced by one owner by construction.** Both modes install a bundle of the same name and identifier at the same path. `make install` removes the engine service before installing a HAL that owns USB; `make install MODE=two-part` registers the service before installing a HAL that has no USB code. An idle engine holds no interface.
10. **The engine must never be removed from under its service.** The service runs the engine from this checkout's `build/two-part`. `make clean` now refuses while an installed service points there, and `make reload` refuses unless the registered executable is the one it just rebuilt.

#### What was built

| Piece | Where |
|---|---|
| Shared ABI: region layout, FIFO and record queue, timeline and stats seqlocks, version 1 | `driver/engine_shared.h` |
| HAL side of the boundary: `usb_engine.h` and `emu_enumerate_units` over XPC and shared memory | `driver/engine_remote.c` |
| Engine process: listener, peer policy, per-unit sessions, pump, `ctl status`/`ctl shutdown` | `driver/engine_service.c` |
| Capture ring made replaceable (`emu_engine_set_input_ring`); nothing else in the transport changed | `driver/usb_engine.c`, `.h`, `driver/ring.h` |
| Lifecycle lock, client IDs, restart when the engine is not running | `driver/plugin.c` |
| `MODE=two-part`, `make reload`, `make engine-status`, `make identity`, clean guard, `SANITIZE=` | `Makefile`, `scripts/engine-service.sh`, `scripts/bundle-identity.sh` |

`driver/plugin.c` is one file for both modes with no conditional compilation; the modes differ only in what it links. The two-part HAL links CoreFoundation and CoreAudio, has no IOKit device symbols, and declares `net.batarelo.EMUTrackerPre.Engine` under `AudioServerPlugIn_MachServices`. The one-part HAL has no XPC symbols and gained one exported symbol. The engine refuses to run as root; launchd runs it as the installing user. HAL-role peers must satisfy the template's code-signing requirement for the two Apple audio hosts; a same-user peer may only ask for status or shutdown.

```sh
make MODE=two-part            # build/two-part/EMUTrackerPre.driver and build/two-part/emu-engine
make install MODE=two-part    # registers the engine service, installs the split HAL, restarts coreaudiod
make reload                   # rebuild and replace the engine only; no password, no coreaudiod restart
make engine-status            # pid, build, units, pump and queue figures
make install                  # back to the release driver; removes the engine service
```

The split's own figures are in `make engine-status` rather than in `emuD`, so the published property surface is identical in both modes: `pumpPasses`, `pumpRecords`, `queueDepthMax`, `wakeLateMaxNs`, `copyDelayMaxNs` and `copyDelayBuckets` (HAL finished writing → engine began binding; under 125 µs, 250, 500, 1 ms, 2, 4, 8, over), `halDropped`, `halDroppedFrames`, beside the engine's `outputUnderruns`, `unfilledPlayback`, `bindRaces` and `writeLeadMax`.

#### Offline evidence, working tree on `33fe611`

| Check | Result |
|---|---|
| Baseline: fresh clone of `33fe611`, `make all && make test` | Passed: 74 Rust tests, analyser self-test, USB abort regression. Rust 1.95.0, Apple clang 21.0.0, macOS 26.7.1. |
| `make test` after the change | Passed: the above, `test-hal-surface`, `test-split`. |
| `make test-hal-surface` | The production plug-in against a fake host, bus and engine; 222 lines of published surface, notifications and engine calls compared with `tests/regression/hal-surface.expected`. It is the stage 1 inventory in re-runnable form, and its fakes are the complete list of engine calls the plug-in makes. Its client and cross-device clock cases fail against the original `plugin.c` (6 checks, 248.7 ms) and pass now. |
| `make test-split` | 131 checks, 0 failures; 8 consecutive clean runs of the final code, no late frame in any. Production plug-in and engine link in one process; production engine service over a pretend device in another, under launchd in the user domain. Covers the list in stage 3, plus a clean reload, a crash mid-publication, a hang, a second HAL, hot-plug, faults and volume. Forwarding delay 0.36–0.51 ms worst per stream, queue depth never above 1. |
| ASan + UBSan over the regression and split tests | Clean. |
| ThreadSanitizer | No report in the new code that is a real race: two on state serialised by one dispatch queue through XPC handlers, which TSan does not model. It does report the plug-in's device registry (below). |
| Alternating `make`, `make MODE=two-part` | Each leaves the other's artifacts alone; all four products pass `codesign --verify --strict`. A build made to fail is rebuilt on retry. |
| Real engine binary as a temporary user-domain service | Listed the attached Tracker Pre and 0404 without opening them, acknowledged `ctl shutdown`, and came back under a new pid. |

The pretend device runs on the host clock, loops playback into capture at the same sample index and goes through the production 24-bit packing, so "read at N what was written at N" tests placement across the boundary. It says nothing about USB timing.

#### Baseline, recorded separately

| | Source | Installed on this Mac |
|---|---|---|
| Identity | `0.1.1 (33fe611-dirty)` in this working tree; baseline tests ran on a clean `33fe611` | `0.1.1 (9b96335-dirty)`, executable SHA-256 `248dab7f…466ba`, signed by team `T59L53M882` |
| Published | UID `net.batarelo.EMUTrackerPre.<serial>`, model UID the product name, six rates 44.1–192 kHz, zero-timestamp period 8192, no buffer-size property (the HAL derives a 3072-frame ceiling), safety 5000 µs in and 4000 µs out, clock source `device`, input mode `on`, seven custom properties, 51 diagnostics keys | Tracker Pre and 0404 both at 48 kHz, safety 240/192 frames, latency 154/116, schedule 81 requests; Tracker Pre volume 0.560, 0404 1.000 |

`EMU_DEVICE=3F0A build/bin/hal-check` resolved the Tracker Pre from this session, read-only. `make identity` prints both columns' identities for either mode. A `-dirty` stamp cannot be mapped back to a source, so the A and B legs should both be built from a committed tree into an empty build directory.

#### Found in the baseline and left as it is

- `notify_input_visibility_changed` always notifies slot 0. With both units attached, changing the 0404's input mode tells Core Audio about the Tracker Pre's streams. Pinned in the surface test so the fix shows.
- `kAudioDevicePropertyRelatedDevices` reports 4 bytes and returns none.
- The device registry (`gDevices`, `gDeviceCount`) is read without the lock by the property and IO paths while hot-plug rewrites it. ThreadSanitizer reports it.
- One-part frees an engine on unplug while an IO thread may still be inside it. The two-part proxy is never freed.
- The one-part and MIDI bundle recipes have the failed-build-looks-built weakness fixed for the split bundle.

#### For the hardware stage

Not established offline, in the order they would be met: that the sandboxed driver host can look the service up and map the region (the template does this on this Mac for FTU); that the engine can claim the audio interfaces as a launchd daemon running as a user; that its real-time threads are honoured there; the 0404's MIDI interface beside it; and the real transport under the pump, which offline only the pretend device stood in for.

For the A/B: build A from this branch, since the plug-in changed. Keep the 0404 idle during measurement legs unless simultaneous operation is the thing measured. Read `make engine-status` after each B run beside `hal-check`; `copyDelayMaxNs` against the 4 ms offset is the margin the split leaves. Reload with clients stopped. If a device was streaming across a reload it reads `alive 0` until a client reopens it or `hal-check fault none` is run for it.

Two experiments that would change the design, neither attempted: aliasing the low-latency USB buffers into the HAL (the engine would publish its bind map and the IO thread would write the packets itself, removing the pump from the data path; it needs proof that a second mapping is visible to the controller), and waking the pump from the IO thread instead of polling.

## Tracker Pre verification

The November 18 hardware run verified engine-only reload and stereo forwarding. The split transport stayed active between recordings, so the observed latency difference still needs a comparison across fresh engine sessions. [Hardware results and raw evidence](issue-investigations/SPLIT-DRIVER-VERIFICATION.md)

1. Rebuild without mode selection and explicitly in split mode. Run `make test`, the new IPC/PCM tests, package/signature checks, and install/reload rehearsal. Inspect the split HAL for USB ownership and the default HAL for accidental runtime service dependencies. Repeat builds in alternating order. Include both device contexts in offline lifecycle races and genuine client/engine process termination.
2. On the Mac with the Tracker Pre, record the exact installed bundle and engine hashes, unit UID, mode, reported offsets, volume, rate and **accepted** buffer size. Save the original installation and settings for restoration. The 0404 is also attached, so a first-device fallback is unsuitable. Verify both UIDs and, when available, simultaneous operation and 0404 MIDI coexistence. The existing silent integration tests exercise ordinary restarts and the one-shot stale-start fault.
3. Use the same source revision for one-part **A**, two-part **B**, then one-part **A** again. Record Core Audio and HAL host identities across engine-only reload. Check that each target is live via its own startup identity and advancing streaming counters. Use the Tracker Pre's actual UID. Keep direct monitor off, gains, USB port and loops identical. Verify right out → input 2 and left out → input 1, then run the frozen forwarding probe in [the existing measurement procedure](issue-investigations/LATENCY-2026-11-16.md). Only input 2 is forwarded to output 1.
4. Start at 48 kHz/512 frames for a stable reference and 48 kHz/128 for a sensitive case. Use three independent starts per setting per leg and eight chirps per run. If time permits, add 48 kHz/64 and 96 kHz/256. Report each leg's median, range, discontinuities, missing chirps, USB errors, missed fills and capture underruns. Retain failed runs and actual frame counts. Compare the B shift with the A-to-A variation. Report frames and milliseconds. At 48 kHz, an extra 128 frames is 2.667 ms. Similar medians with new dropouts fail parity. Restore the accepted rate/buffer, original installation, and any controls or defaults the test modified.

## Latency opportunity after parity

The November 16 E-MU experiment measured actual **software forwarding** on Tracker Pre. At 48 kHz/128 it measured 18.484–18.525 ms, and both runs had timestamp discontinuities. At 48 kHz/512 it measured 34.525–35.484 ms with three clean runs. These are historic controls from installed `9b96335-dirty`. The FTU guarded profile measured **raw analog round trip** of 11.872295–11.872307 ms at 48 kHz/64 over ten minutes. A more aggressive FTU profile reached 9.8723 ms in a shorter run. These measure different paths on different converters.

E-MU already reserves USB transfers ahead and fills playback late. FTU's strongest candidates for E-MU are shorter capture service intervals (E-MU groups 2 ms per request), measured completion-to-publication delay, phase-by-phase deadline replay, and independently sizing reservation depth and PCM safety. Use tails and failure counters to decide whether HAL safety can be reduced. For the historic 48 kHz setting, lowering a real safety margin by 1 ms would remove 48 frames **if** the HAL adopts it and playback and capture still meet deadlines. The two HAL buffers in the measured callback separation and the converter/device path remain part of the total. Change one parameter at a time after the architecture comparison and remeasure accepted settings.

## Contributions between repositories

| Direction | Candidate and practical test |
|---|---|
| E-MU → template and FTU | Adapt the two-channel one-way forwarding probe as an explicit software-monitor measurement alongside raw RTL. Verify routing, inverted polarity, missing chirps, timestamp discontinuities and restoration. E-MU's existing capture includes the source, raw logs and settings. |
| E-MU → template and FTU | Generalize the delayed-abort, partial-start and duplicate-submit cases in `tests/regression/usb-abort-test.c`, plus the installed-driver silent restart/fault pair. Port the ownership assertions to each native transport. |
| E-MU → template | Use the four-slot E-MU registry, two simultaneous devices and fractional rates as test cases for extending the singleton backend ABI. Include serial identity, hotplug, independent epochs and per-unit cleanup. |
| Template → E-MU and FTU | Reuse its positive reload-reply ordering, versioned session/lifetime checks, install collision handling, and real two-process tests. FTU's local `docs/TEMPLATE-UPSTREAM.md` already identifies the reload acknowledgement race as a candidate. Retain the template's MIT notice for reused code. |
| FTU → E-MU | Adapt USB/HAL phase replay and capture-publication histograms. Correlate controller time, callback entry, PCM availability and HAL read for the same request. Import its CPU and loss reporting so lower latency can be weighed against CPU and stalls. Keep device-specific transport assumptions local. |

Implementation and measurement evidence should record the tested commit, configuration, commands, count of passing and failed runs, and remaining hardware checks. The initial branch baseline passed `make all && make test` from a fresh checkout. The installed HAL reports `0.1.1 (9b96335-dirty)`. The USB registry lists Tracker Pre and 0404. The read-only `EMU_DEVICE=3F0A build/bin/hal-check` preflight from the Codex shell could not resolve the Tracker Pre through Core Audio.
