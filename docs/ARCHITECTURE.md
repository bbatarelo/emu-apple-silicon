# Architecture

```
   application
        |
   Core Audio HAL              Float32, fixed buffers, host clock
        |
   driver/plugin.c             AudioServerPlugIn: device, streams, controls
        |
   driver/ring.h               lock-free timeline-indexed ring (capture side)
        |
   driver/usb_engine.c         its own thread and run loop
        |
   shared/usb_util.c           IOKit interface and pipe helpers
   rust/emu-ca0189             descriptors, protocol, feedback, clock
        |
   low-latency isochronous
        |
   E-MU Tracker Pre
```

One process. The plug-in loads into `com.apple.audio.Core-Audio-Driver-Service.helper`
and opens USB directly, so there is no helper daemon and no IPC.

## Layout

| | |
|---|---|
| `driver/` | The HAL plug-in. This is the product. |
| `shared/` | Device identity and IOKit helpers, used by the driver and the tools |
| `rust/emu-ca0189/` | Descriptor parser, protocol model, feedback queue, clock estimator |
| `tools/` | `emu-probe` (USB diagnostics), `hal-check` (device inspector), `hal-record` (capture verifier), `hal-trace` (stream-start timeline), `hal-loopback` (closes the loop with a cable) |
| `captures/` | Descriptor and packet-trace fixtures, replayed by the tests |
| `driverkit/` | An unfinished DriverKit version, parked |

## The Rust core

`rust/emu-ca0189` holds everything that is about the *device* rather than about
macOS: parsing descriptors, the CA0189 request encoding, rate codes, the feedback
queue and the clock estimator.

It knows nothing about IOKit, Core Audio or DriverKit. That is deliberate — it
compiles `no_std` for a DriverKit dext and with `std` for userspace, and it is
tested against captured traces with no hardware attached.

```bash
make test        # 35 tests, replaying real captures
```

Its C interface is `rust/emu-ca0189/include/emu_ca0189.h`. The boundary is a
small C ABI: `#[repr(C)]` on everything shared, raw pointers validated once at
the edge, no panics crossing over.

## The two clocks

This is the heart of it.

Core Audio delivers fixed 512-frame buffers paced by the host clock. USB moves
variable packets — 44 or 45 frames at 44.1 kHz — paced by the *device's* clock.
The two never agree exactly.

The timeline removes the disagreement; both directions are addressed by it.

**The timeline** (`GetZeroTimeStamp`) anchors to frames the device has actually
consumed, not to `mach_absolute_time`, and it exists before the first sample
moves: the engine publishes the scheduled bus time of the first packet as the
anchor for sample 0 *before* `StartIO` returns (which is why `StartIO` blocks
through USB bring-up — it is off the IO path, and a slow start is the
sanctioned kind). Every completed request then re-anchors from the frame
list's hardware timestamps (`frTimeStamp`, stamped by the host controller, far
steadier than callback timing) through a critically damped filter in the Rust
core — the same mass-spring-damper the original EMUUSBAudio kext tuned on this
hardware. The anchor is published as a matched pair — frames consumed, and the
host time that count was true — through a seqlock. Two plain atomics would let
a frame count from one completion pair with a timestamp from the next, which
is worse than no anchor at all.

The one discipline above all: **never splice timelines.** Handing out a
host-clock guess first and switching to the device clock later, same seed,
makes Core Audio absorb the discrepancy by stalling its IO thread for its
length — an audible dropout ~0.3 s into every stream (FINDINGS has the
measurement).

The same discipline applies mid-stream, and there it is stricter: **the seed
never changes.** When a scheduling stall outlasts the in-flight window the
request queue goes stale and the engine rebuilds the bus schedule ahead of a
fresh frame number. The bus frames between the end of the old schedule and the
start of the new one carried no packets, but the device's clock ran through
them all the same, so the engine accounts them as what they were: frames the
device consumed while nothing reached it. Each direction's cursor skips exactly
that many frames, the timestamp filter is rebased (it keeps the rate it has
learned; only its prediction moves to the new schedule), completions still
draining from the old schedule are muted by generation, and the (frames, host)
pair Core Audio sees stays on the same straight line with a hole in the audio
where the dropout was. That is what any DMA engine looks like when it
underruns, and it is what the HAL's clock model is built for.

Pausing the sample clock instead — cursors held, host time jumping — would
need a new zero-timestamp seed, the HAL's mechanism for a genuinely new
timeline, and the HAL answers a seed change by resynchronising its IO thread:
a second glitch for every stall (FINDINGS has the measurement). `deadFrames`
reports the dropout length of every rebuild.

**The cursors keep bus time, not delivery.** `out_cursor`, `frames_played`
and `in_cursor` all count sample frames per scheduled bus interval from the
stream's first packet, and the safety offsets hold only while all three
advance by the same amount for the same interval. The device's clock runs
through an interval whether the packet in it was delivered, errored, arrived
empty, or was never scheduled at all, so the completions count every
interval: the planned frames for a playback entry the bus did not carry, a
nominal packet of silence for a capture entry that brought nothing
(`emptyCapture` counts these; a couple at every start is the ADC spinning up —
the packet traces show two), and a rebuild counts the intervals no request
covered. The one interval nobody counts is one that was never on the bus: a
failed playback submission rolls its cursor advance back. Skipping any of
these instead moves one cursor and not the others, and the offset never
heals — every skipped packet takes its size out of the safety margin for the
rest of the stream: persistent crackle with a perfectly healthy clock, cured
only by a stream restart (FINDINGS).

The filter itself snaps at 3 observation steps (~6 ms), because with hardware
timestamps anything past that is a real schedule move, not jitter — a
threshold wide enough to slew through a stall-sized gap puts the filter in
its clamp regime, where the damping term has no input and the anchor
oscillates for seconds (FINDINGS). A planned discontinuity (a rebuilt
schedule) is rebased, not snapped, so `tsResets` counts only surprises.
The `resyncs`, `deadFrames`, `tsResets` and
`unfilledPlayback` counters exist to make the next such event legible — and
the final counters of a session survive engine teardown, so a post-mortem
`make check` reads evidence instead of zeros.

**Everything is timeline-indexed, never a FIFO.** Output needs no staging at
all — Core Audio writes into the USB request that carries the frames its IO
cycle names, so the phase is the map. Capture does need a buffer, and
`driver/ring.h` is one addressed the same way: the engine writes at the frame
it just received, Core Audio reads at the sample time its cycle names
(`slot = frame mod ring size`), and the consumer zeroes slots behind itself,
so an unwritten slot reads silence and never a stale lap.

The phase between producer and consumer is therefore a constant fixed by the
published safety offset — not an accident of which side started first, which
is what a FIFO makes it. This is how IOAudioFamily sample buffers and the
original kext work, and it is what makes an underrun *one* glitch: an earlier
FIFO here slipped its phase permanently on every dropped frame, which both
grew latency without bound and was the mechanism behind the startup crackle.

## Latency

The driver's contribution is the output safety offset: how far ahead of the
play head Core Audio must deliver samples. It is specified in microseconds and
converted at the current rate (a fixed frame count would be wrong at every
other rate), published per direction, and it is taken literally — it is the
*only* budget on the output path, because only one thread is on that path.

That is the kext's central latency trick, reproduced here. Its fill ran on
coreaudiod's own IO thread (`clipOutputSamples` wrote straight into the USB
buffer), so no second thread ever touched the audio and the offset covered
only the IO thread's own jitter. Here **Core Audio's `WriteMix` converts
directly into the submitted USB request buffers**.

A request's frame list — packet count and sizes — must be fixed when the
request is queued, and sizes do come from the feedback servo that far in
advance (a rate servo does not mind the lag). The audio bytes do not: each
request's buffer goes out zeroed, the engine publishes the slice of the
timeline that request carries, and the IO thread converts into it afterwards.
Low-latency buffers are what make this legal — shared, wired memory the
controller reads at transmission time; `frTimeStamp` arriving in our frame
lists is the same memory working the other way. Zeroing at submit is also the
experiment's control: a USB stack that secretly snapshotted the buffer at
submit would play exact silence, so late binding is verified the moment
anything is audible at all. Verified on the 0404 USB.

What makes the map cheap is that a request's byte layout is **linear in the
timeline**. Entries are contiguous and `frReqCount` is the packet's real size
(`emu_output_packet_bytes` is frames × bytesPerFrame and nothing else), so the
offset of any frame the request covers is
`(frame - data_frame_start) × bytesPerFrame`, and the whole map is three
numbers per request: where its slice starts, where it ends, and the buffer.
Variable packet sizes never enter it. It is published through a per-request
seqlock as the request goes on the bus.

The hazard staging did not have is recycling. A slot is reused at the far end
of the schedule while the IO thread writes at `writeLead`, so the two are
normally the schedule's margin apart and cannot meet; under a stall they can.
The seqlock makes that a best-effort counted event (`bindRaces`) rather than
unexplained noise — it cannot take the bytes back, but the damage is one packet
inside a stretch that is already glitching. Its post-write check uses only a
compiler barrier: a full store/load hardware fence on every normal request
slice would cost more than the diagnostic is worth.

Both hazards live or die on memory ordering, and the orderings are not the
obvious ones. The writer bumps the sequence to odd with a *relaxed* store and
a release **fence**, because a release store orders what precedes it and would
let the memset and the new range float above the marker — a reader would then
see a clean sequence over a half-rewritten entry, which is the exact tear the
seqlock exists to catch.

Teardown's stronger hazard is represented separately by one atomic gate: its
high bit closes admission and its low bit records the single serialized
`WriteMix` callback. The writer claims the gate with acquire semantics and
leaves with release semantics; teardown sets the closed bit and waits for the
writer bit to clear.
Admission and closure therefore share one modification order, with no Dekker
protocol and no sequentially consistent operations on the IO path. Teardown
has no unsafe timeout: it never frees a low-latency buffer until the admitted
writer has left.

The default offset is 4 ms out, 5 ms in. The output value is tunable through
the `SafetyOffsetMicroSec` custom property (`'emuS'`, `hal-check safety`), the
same knob the original kext exposed. The engine takes the value at the next
stream start; coreaudiod's published copy only follows a coreaudiod restart
(FINDINGS), which `hal-check safety` makes visible. Reported presentation
latency past the offset is the converter path, measured with a loopback cable:
68 frames of converter group delay plus 4.23 ms of device buffering, split
between the two directions (FINDINGS).

**Where Core Audio actually writes is not the offset**, and this is what sizes
the schedule. Measured across every IO buffer the HAL will grant,

    writeLead ≈ 2 × bufferFrames + safetyOffset + ~208 frames

— the HAL computes an IO cycle's output time one buffer period ahead of
presentation and then hands over a buffer-length range, so the far end lands
at offset + 2 × buffer; the residual is peak cycle jitter. A frame no
submitted request covers has nowhere to go, so **the schedule must reach past
that**. It is bounded, because the buffer size is not the client's to choose
without limit: the HAL caps it at `min(4096, ZeroTimeStampPeriod × 3/8)`,
which is this driver's own constant coming back (FINDINGS has the fit and
three confirming predictions). So the depth is *derived*, not tuned —
`schedule_depth()` computes it per session from the rate, the offset and
`HAL_MAX_IO_BUFFER`. It is sized against the offset *ceiling*
(`EMU_OUTPUT_SAFETY_MAX_US`, 20 ms) rather than the offset in force, because
the lead depends on the offset **coreaudiod has cached** — which follows a
coreaudiod restart and nothing else. Lower `'emuS'` at runtime and the HAL
keeps writing at the old, larger offset; a schedule sized for the new value
would be short by the difference and drop it silently. Sizing for the ceiling
makes any cached value safe. `EMU_ZERO_TIMESTAMP_PERIOD` and
`EMU_OUTPUT_SAFETY_MAX_US` both live in `usb_engine.h` rather than staying
private to the plug-in precisely so these cannot drift apart, and
`scheduleClamped` reports the case where `MAX_REQUESTS` truncates the answer
anyway.

Depth costs wired memory — ~120 KB for both directions at 48 kHz, under half a
megabyte at 192 kHz — and feedback-servo lag, which does not show:
`frameDeficit` stayed flat at 0 over 30 s. It costs no latency at all, and it
is what a scheduling stall must outlast before the schedule goes stale and has
to be rebuilt (`resyncs`, `deadFrames`). A stall shorter than the offset is
nothing; one shorter than the schedule is silence of exactly its excess
(`unfilledPlayback` counts the requests, `outputUnderruns` the frames).

The engine thread still declares its cadence via
`THREAD_TIME_CONSTRAINT_POLICY` — it must keep the schedule ahead of the bus
and re-anchor the clock every couple of milliseconds. The period is
`REQUEST_MS`, and one arrival carries *both* directions' completions —
capture and playback requests go into identical bus frames, so the pair lands
together and shares the budget. The declared constraint is 1 ms rather than
the 2 ms period on purpose: the kernel forces `computation` up to
`constraint/2`, so declaring the period would reserve half a core every period
instead of a quarter. What it no longer has to buy is data-path punctuality:
nothing in a thread policy governs how promptly the USB stack delivers a
completion, and with the audio bound by Core Audio itself that jitter is
absorbed by schedule depth instead of by the offset. That is the whole reason
the offset can be 4 ms rather than 10.

## Packet sizing

Playback packet sizes come from the capture stream. Each capture service interval
reports how many sample frames the device produced; that count sizes the next
playback packet, through the feedback queue in the Rust core.

This is why **capture runs even when only playback is wanted** — it is how the
driver measures what the hardware is doing. It is also what E-MU's original
driver did.

There is an explicit feedback endpoint (`0x81`) on the playback interface which
this driver does not currently use.

## The USB engine

`driver/usb_engine.c` runs on its own thread with its own run loop. `StartIO`
creates it and blocks until the streams are scheduled and the timeline anchor
is published; `StopIO` joins it — outside the state lock, since holding a lock
across a thread join invites deadlock.

Before any of that, the engine's one standing job is knowing whether a device
is there. IOKit first-match and terminated notifications on a private queue
keep a per-product count of what is attached, and the plug-in publishes its
Core Audio device only while that count says something is. There is no
register/unregister call for a HAL plug-in — it *is* its device list, and
Core Audio adds and removes the device by re-reading the list when told it
changed — so presence lives in the answers to `kAudioPlugInPropertyDeviceList`
and the plug-in sends a change notification on every arrival and departure.
An unplugged Tracker Pre is therefore absent from every device menu rather
than listed and unable to start, and swapping one family member for another
renames the device in place.

The choice among several attached devices is made in exactly one place, the
watcher's resolve: the product the engine has open, while it stays attached;
else the preferred product; else the first in the table. `StartIO` opens
whatever that names rather than running a lookup of its own, so the name Core
Audio shows and the hardware behind it cannot disagree. The engine pins its
product before opening it and unpins in teardown; each is followed by a
resolve on the hot-plug queue, so a sibling arriving between the engine's
read and its pin is handled in the same order as any other arrival, and a
preferred sibling arriving mid-stream renames nothing until the stream ends.

The watch is armed in `Initialize`, all or nothing: if IOKit will not hand
out a notification port or a matching notification -- in practice only when
the process is out of Mach ports or memory -- whatever was armed is disarmed
again, `Initialize` fails with a log line saying so, and nothing is
published. There is deliberately no look-the-device-up-once fallback. It would
degrade to exactly the stale-device behaviour the watch exists to fix, under
conditions nobody could reproduce, without a word in the log.

Each direction keeps `num_requests` requests of `REQUEST_MS` (2 ms) in flight
— between `MIN_REQUESTS` (16) and `MAX_REQUESTS` (128), derived per session by
`schedule_depth()`; see Latency above. The request length sets the completion
cadence; the count sets how far ahead Core Audio may write and how long a stall
the schedule survives.

It uses **low-latency isochronous transfers**. The classic API delivers one
frame-list entry per USB frame, which silently halves the audio on the
`bInterval 3` endpoints used at 176.4 and 192 kHz. Buffers come from
`LowLatencyCreateBuffer`; ordinary allocations are rejected.

Startup order matters and is not negotiable: parse descriptors, set the clock
rate, **verify it by read-back**, then select alternate settings. Doing it in any
other order can wedge the device until it is physically replugged.

## Diagnostics

The plug-in runs inside a system daemon that exits when idle, so there is no
process to attach to, and log queries proved unreliable while developing it.

Instead the driver publishes counters through a declared custom property, and
`hal-check` enumerates whatever it finds rather than a list kept client-side.
Frames played, captured and bound, the output lead, underruns, USB errors,
feedback starvation.

This is not scaffolding. Every misalignment bug in this project produced correct
byte counts and zero errors; the counters are what made it possible to say a path
was *clean* rather than merely plausible — and what made clear when the answer
had to come from listening instead.
