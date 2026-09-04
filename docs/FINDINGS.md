# Findings

What was learned getting this device working, organised by subject rather than
by the order it was discovered. This is the file to read before changing
anything, and the one to add to when something new bites.

Most of it was expensive to learn. Several of these produce *no error at all* —
correct byte counts, zero failures, and wrong audio.

---

## The device

E-MU Tracker Pre, USB `041e:3f0a`, CA0189 chipset, High Speed, bus-powered
(500 mA). Descriptors captured in `captures/descriptors/`.

### It is not a USB Audio Class device

All three interfaces report `bInterfaceClass = 0xFF` (vendor-specific), so
macOS's `AppleUSBAudio` never attaches. That is why the device is silent on a
stock system — and also why an unprivileged process can drive it, since nothing
else has claimed the interfaces.

It does keep audio-class *subclass* numbering (1 = control, 2 = streaming) and
audio-class descriptor layouts, which is why a UAC-shaped parser works on it.

| | |
|---|---|
| Interface 0 | control, interrupt status endpoint `0x83` |
| Interface 1 | playback, `0x01` OUT, feedback endpoint `0x81` |
| Interface 2 | capture, `0x82` IN, no feedback endpoint |

Every alternate setting is **2 channels, 24-bit, 3 bytes per sample**, so 6 bytes
per sample frame in both directions. That the two directions agree is a
coincidence of this device, not a rule — packet sizes must still be derived from
each direction's own `bytesPerFrame`.

### Rates and service intervals

| Rate | alt (iface 1 / 2) | bInterval | wMaxPacketSize | Frames per interval |
|---|---|---|---|---|
| 44100 | 1 / 1 | 4 (1 ms) | 274 | 44 or 45 |
| 48000 | 3 / 3 | 4 | 298 | 48 |
| 88200 | 5 / 5 | 4 | 538 | 88 or 89 |
| 96000 | 7 / 7 | 4 | 586 | 96 |
| 176400 | 9 / 9 | **3 (0.5 ms)** | 538 | 88 or 89 |
| 192000 | 10 / 10 | **3** | 586 | 96 |

**176.4 and 192 kHz exist only at `bInterval = 3`.** Every other rate offers both
a 1 ms and a 0.5 ms variant. Nothing may assume a given service interval is
available at a given rate.

**Alt 11 duplicates alt 3 exactly** on both interfaces — same rate, packet size
and interval. Purpose unknown. Rate alone does not uniquely identify an alt
setting, so selection needs a tie-break rule.

### Clock control

One extension unit, **unit ID 12, code `0xe301`**, on interface 0. E-MU's header
defines five more (`0xe302` clock source through `0xe306` metering); **none are
present** on this device. Treat that header as a vocabulary, not an inventory.

Standard UAC1 extension-unit requests, no proprietary framing:

```
bmRequestType  0xA1 get / 0x21 set     class, interface recipient
bRequest       GET_CUR 0x81 / SET_CUR 0x01
wValue         selector << 8           = 0x0300 for clock rate
wIndex         (unitID << 8) | iface   = 0x0C00
wLength        1
```

Rates are opaque **codes**, not frequencies: 0=44100, 1=48000, 2=88200, 3=96000,
4=176400, 5=192000.

### `SET_CUR` can silently fail

Observed once: setting 192 kHz returned `kIOReturnSuccess` while the device
stayed at 44.1 kHz. Roughly 330 subsequent transitions never reproduced it. Cold
start, the specific rate pair, and interference from other processes were all
tested and ruled out; the trigger is unknown.

It returned the *default* rate, not the previous one, which suggests the device
abandoned the change rather than lagging behind it.

**Always verify with `SET_CUR` → `GET_CUR` → compare.** E-MU's own driver does
not, and rewrites the failure as success:

```c
result = usbAudioDevice->setExtensionUnitSettings(kClockRate, kClockRateSelector, ...);
if (kIOReturnSuccess == result) { ... } else {
    result = kIOReturnSuccess;      // failure rewritten as success
}
```

### Selecting a mismatched alt setting wedges the device

Choosing an alternate setting whose `tSamFreq` disagrees with the active clock
returns `kIOReturnTimeout`, and the device then **refuses every control transfer**
with `kIOUSBTransactionTimeout`. It stays enumerated and configured; it simply
stops answering.

`USBDeviceReEnumerate` does **not** recover it — the device left the bus and did
not return. Only a physical replug does.

Set the clock rate and verify it *before* selecting any alt setting. E-MU's
`startUSBStream` calls `SetSampleRate` before `SetAlternateInterface` for this
reason.

### The startup phase-alignment ramp

When a stream starts, the device inserts **one extra sample frame every tenth
service interval** until phase is recovered, then holds exact nominal. Measured
at 48 kHz: 164 corrections, every gap exactly 10 intervals, from interval 27 to
1657.

- Duration varies: 830 ms in one capture, 1630 ms in another.
- It does not always happen — roughly one stream start in four.

Averaging across the ramp reports the device hundreds of ppm fast. The worst
observed was +808 ppm at 44.1 kHz, and a 48 kHz capture that should read
*exactly* zero read +327 ppm. Discard about 2000 ms before measuring the clock.

Consequence: **packet sizes are never guaranteed**, not even at 48/96/192 kHz
where the settled size is constant.

### Capture packets at `bInterval 3` lead with a 4-byte length word

At 176.4 and 192 kHz every **capture** packet is exactly 4 bytes longer than a
whole number of sample frames — 100% of packets, never at `bInterval 4`. The
four bytes sit at the **front** and hold the packet's own total length,
little-endian, counting themselves. E-MU's Windows driver reads the first
`ULONG` of each IN packet and steps past it, for exactly these product IDs and
only where the endpoint is serviced more than once per millisecond
(`m_UseEmbeddedPacketLength`, `Audio.cpp`). Read on this hardware, the word
equals `frActCount` on 11968 of 11968 packets through a clean 192 kHz stream.

The driver steps over them by remainder rather than by rate:

```c
uint32_t lead = f->frActCount % e->bytes_per_frame;
```

which is zero at every rate up to 96 kHz, so those paths are untouched, and
which keeps the read inside the packet by construction. Taking the frames from
the packet's first byte instead puts every sample two thirds of a frame early
and scrambles all of them: a −12 dBFS tone at 192 kHz comes back at −49 dBFS
with the peak pinned at 1.0000 and +47 dB THD+N, where four bytes in it reads
−17.6 dBFS at −62 dB, which is what the cable delivers.

Playback has no such framing. An OUT packet of `frames × 6 + 4` bytes yields
**silence** (−131 dBFS), not displaced audio, and oversizing only one packet in
four silences the whole stream: the device rejects a size it dislikes
wholesale, with every transfer reported successful and every byte delivered.
Nothing on the host can see the device discard audio.

### A `bInterval 3` stream leaves the next stream corrupt

Observed on @dnadlinger's development 0404 USB: after a stream at 176.4 or
192 kHz, exactly one following stream is corrupt — a −12 dBFS tone returns
near −38 dBFS with the peak pinned at 1.0000 and the *other* channel's tone
stronger than the wanted one — and the stream after that is clean. It follows
the 0.5 ms service interval, not the rate: a 96 kHz stream on its 0.5 ms
alternate setting does the same, and 48 kHz is corrupted as readily as 96 when
it runs after 192 kHz.

| sequence | result |
|---|---|
| 48 → 192 → **96** → 96 → 96 | broken, clean, clean |
| 48 → 192 → **48** → 48 → 48 | broken, clean, clean |
| 48 → **96** → 96 → 96 | clean throughout |

Only running a stream clears it; waiting ten seconds, or changing the rate in
between, does not. The length word shows what it is: in the corrupt stream the
word turns up two bytes into the packet instead of at its front, so the IN
stream is shifted by two bytes — the shape of a stale partial packet left in
the device's IN FIFO, which the next stream drains as misaligned data. It can
outlast a short stream by a few seconds. Every fault counter reads zero
throughout.

For measuring anything at these rates: prime with two streams, not one, and
when one rate is broken and the same rate is clean a moment later, look at what
ran *before* the broken stream. Consecutive streams at 192 kHz alternate broken
and clean, so a per-rate failure rate taken over consecutive runs measures this
and nothing else.

Whether every unit does this is not known.

### Duplex at 176.4 and 192 kHz drops playback packets on some setups

On @dnadlinger's development setup — a 0404 USB on a MacBook Pro M1 Max
(macOS 15.7.4), on a direct port and behind a USB 2.0 hub alike — full duplex
at the two `bInterval 3` rates loses about one playback packet a second,
audible as crackle:

| rate | silent playback packets |
|---|---|
| 44.1 – 96 kHz | none |
| 176.4 kHz | 2.75 / s |
| 192 kHz | 1.50 / s |

Each is a single whole packet on the packet grid. The tone resumes in phase, so
the timeline never moved: silence replaced the audio rather than displacing it.

**Not every setup shows it.** Bruno Batarelo reports duplex at both rates
working on a 0404 USB and on a Tracker Pre. It is therefore a property of some
combination of unit, firmware and host, not of the device family, and the
workaround below is opt-in rather than the default.

**It follows the presence of IN transactions on the bus.** Playback with the
capture interface never claimed is clean at both rates, in the driver
(`hal-check input off`) and in `emu-probe play-only`. So is `emu-probe
play-idle`, which claims interface 2, selects the streaming alternate setting
— bandwidth reserved, ADC clocked, FIFO filling — and never queues a read, so
the host issues no IN token. Only full duplex drops packets. The device's own
feedback value says the same without anyone listening: identical to the bit in
either no-capture mode, 14–38 excursions per 8 s window in duplex. (Excursion
counts spread widely between runs; this order-of-magnitude separation is the
one feedback statistic that survived repetition.)

**Everything on the host has been varied without effect.** The rate stays at
1.1–1.7 per second at 192 kHz throughout:

| ruled out | how |
|---|---|
| Core Audio not writing the frames | `framesToOutput == framesBound`, exactly |
| the write missing the buffer that transmitted | a marker fill in place of the submit-time zeros never came back; every dropout was silence |
| the transfer failing | `usbErrors` 0 |
| the bus carrying fewer bytes than asked | `shortPlayback` 0 |
| the packet size | every packet is 96 frames at 192 kHz; 44.1 kHz changes size about 110 times a second and never drops |
| the 0.5 ms service interval | 48 and 96 kHz on their own 0.5 ms endpoints drop nothing |
| the write lead | a 20 ms safety offset, 5× the default: unchanged |
| the playback architecture | a staging ring, the direct bind and `emu-probe`'s own engine all drop at the same rate |
| the request granularity | 1, 2 and 8 ms requests all drop |
| the schedule phase between the directions | playback started 0–7 ms after capture: unchanged. E-MU's Windows driver's +3 ms offset primes its output FIFO; any whole millisecond is a whole number of 0.5 ms intervals and cannot move the phase |
| the planner source | sizing from `0x81` instead of capture: identical over six paired loopback runs |
| the port and the electrical path | direct port and behind a hub: the same |

**The loss is past the host, and the driver cannot see it.** The buffer held
audio when the packet went out, the transfer succeeded, the bus carried every
byte, and the DAC went quiet. The device can be watched discarding audio with
every counter clean (the oversize OUT packet above), so there is no counter to
add: the information does not cross the bus. What finds it is `hal-loopback`,
which listens.

Untried: a different USB host controller, and a different Mac. Given the
report above, that or the unit itself is where the difference lives.

**The workaround is the input mode.** `hal-check input auto` leaves capture
unopened at 176.4 and 192 kHz; playback is then sized from the feedback
endpoint, and the two rates go from unusable to playback-only. A five-minute
playback-only run at 176.4 kHz is clean by ear and delivers −2.3 ppm against
the host clock. `off` does the same at every rate.

### The explicit feedback endpoint

`0x81` is a real endpoint, not just a `bSynchAddress`: every playback alt
setting carries a 4-byte isochronous IN descriptor for it at `bInterval` 4. The
driver reads it always and sizes playback from it while capture is off.

**Units.** Q16.16 sample frames per playback **service interval**, the reading
E-MU's Windows driver uses — not frames per millisecond, which the same
vendor's macOS kext assumed and which differs by a factor of two on a
`bInterval 3` endpoint. The hardware settles it: at 192 kHz on a 0.5 ms
endpoint the value is `0x00600000`, 96.0000 frames.

**Cadence.** One value every 32 service intervals — 32 ms at `bInterval` 4,
16 ms at `bInterval` 3, the period the kext hard-codes as `kEMURefreshRate` —
and nothing in between. Resolution 1/128 of a frame.

**Scaling.** The value is a constant, exact at the integer rates and 53.1 ppm
low at every 44.1-family rate:

| rate | interval | nominal | device says | | vs nominal |
|---|---|---|---|---|---|
| 44100 | 1.00 ms | 44.1000 | `0x002c1900` | 44.0977 | −53.1 ppm |
| 44100 | 0.50 ms | 22.0500 | `0x00160c80` | 22.0488 | −53.1 ppm |
| 48000 | 1.00 ms | 48.0000 | `0x00300000` | 48.0000 | 0 |
| 48000 | 0.50 ms | 24.0000 | `0x00180000` | 24.0000 | 0 |
| 88200 | 1.00 ms | 88.2000 | `0x00583200` | 88.1953 | −53.1 ppm |
| 88200 | 0.50 ms | 44.1000 | `0x002c1900` | 44.0977 | −53.1 ppm |
| 96000 | 1.00 ms | 96.0000 | `0x00600000` | 96.0000 | 0 |
| 96000 | 0.50 ms | 48.0000 | `0x00300000` | 48.0000 | 0 |
| 176400 | 0.50 ms | 88.2000 | `0x00583200` | 88.1953 | −53.1 ppm |
| 192000 | 0.50 ms | 96.0000 | `0x00600000` | 96.0000 | 0 |

The fraction bytes say why: `0x0c80` = 3200 for .05, `0x1900` = 6400 for .1,
`0x3200` = 12800 for .2 — each the fraction times **64000**, where Q16.16 wants
65536. It is the firmware's arithmetic, not the clock: in one duplex stream at
176.4 kHz, capture measures the device producing 88.2000 frames per interval,
nominal to 0.1 ppm, while the endpoint says 88.1953. `emu_feedback_true_q16`
in the Rust core undoes the scaling, and everything that sizes packets from
`0x81` goes through it. The same constant scales the excursions: a raw step of
`0x1000` reads as 1/16 of a frame and means 4096/64000 = 0.064 frames.

The correction only matters when the endpoint is the sole clock. Playback-only
at 176.4 kHz delivers 88.2000 frames per interval corrected; uncorrected it
delivers 88.1997, only 3.4 ppm short rather than 53 because the device servos
against the shortfall, hunting through about 100 trims per 20 s.

**The excursions are the servo.** Playback-only at 176.4 kHz sits on nominal
with the demand quiescent for the first minute, then trims by one low value
every 5.04 s: one trim is 0.064 frames held for one reporting period, which at
one per 5 s is 2.3 ppm — the −2.3 ppm the run delivers. The device's crystal
sits about that far from the host clock and the loop absorbs it.

**Capture stays the planner while capture runs.** Capture sizing keeps
`in_cursor` and `out_cursor` advancing by the same amount for the same interval
by construction, and it is a measurement, where the endpoint is a rounded
constant that moves a sixteenth of a frame a few times in ten seconds. Values
further than one frame from nominal are refused: through a corrupt stream
(above) the endpoint returns nonsense along with everything else.

---

## The 0404 USB

E-MU 0404 USB, `041e:3f04`, verified 2026-08-22. Same CA0189 protocol as the
Tracker Pre — the clock extension unit, the rate codes, the alt-setting layout
and the feedback model all carried over untouched. Descriptors in
`captures/descriptors/0404-usb-*`.

What it took: **one parser fix**, described below. Nothing in the transport,
the clock code or the feedback model needed changing.

Measured on the hardware:

| | |
|---|---|
| `clock` / `clock-support` | unit ID **12**, code `0xe301`, interface 0 — identical to the Tracker Pre. Rate bitmap `0x3f`, all six rates |
| `clock-sweep` | 6/6 rates set and verified by read-back, original restored |
| `capture` | +0.0 ppm at 48 and 96 kHz, uniform packet sizes, no errors |
| `play` | `STABLE` at 44.1, 48 and 96 kHz, tracking error ≤ 1.1 ppm, clean tone |

### It has a MIDI interface, and that broke the parser

Four interfaces, not three: control, playback, capture, and a **MIDI-streaming
interface (subclass `0x03`)** at interface 3.

The parser treated *every* non-control interface as audio streaming. MIDI's
class-specific descriptors reuse the same subtype numbers for entirely different
things — subtype `0x02` is `MIDI_IN_JACK`, six bytes long, where audio streaming
has a 11-byte `FORMAT_TYPE` — so the first MIDI jack descriptor was read as a
truncated format descriptor and the whole configuration was rejected with
`BadDescriptorLength`.

**Which subclass an interface belongs to has to be tracked, not inferred from
"not control".** Interfaces that are neither `0x01` nor `0x02` contribute no alt
settings and their class-specific descriptors are skipped.

The Tracker Pre has MIDI ports on the box but no MIDI-streaming interface in its
descriptors, which is why this went unnoticed.

The interface itself is ordinary USB-MIDI 1.0, unlike the audio side: one
embedded jack each way on **bulk** endpoints `0x05` OUT and `0x85` IN, with a
standard `MS_HEADER` and the usual in-jack/out-jack pairs. It is now driven —
the parser records it in the model, the Rust core owns the event-packet
framing, and a CoreMIDI plug-in (`midi-driver/`) publishes the ports.

Measured on the hardware, with a DIN cable looped from MIDI OUT to MIDI IN
(`emu-probe midi-loopback`): a 29-byte test stream covering both channel
message lengths, running status, system common, real-time and a
packet-spanning SysEx came back **byte-for-byte intact** (30 bytes returned —
the USB framing legitimately expands running status, since every event packet
carries its status byte).

Two transport facts worth recording:

- The bulk pipes coexist with the audio driver. coreaudiod claims interfaces
  1 and 2 and MIDIServer claims interface 3, and neither needs the *device*
  opened, so audio and MIDI run from different processes without contention.
- USB interface claims are exclusive, so `emu-probe`'s raw `midi-*` commands
  fail with `kIOReturnExclusiveAccess` once the CoreMIDI driver is installed.
  `make uninstall-midi` frees the interface for probing.

### Four extension units instead of one

| Unit | Code | |
|---|---|---|
| 12 | `0xe301` | clock rate — the one the driver drives |
| 13 | `0xe302` | clock source |
| 14 | `0xe303` | digital I/O status |
| 15 | `0xe304` | device options |

The extra three are the route to the S/PDIF and clock-source controls. Nothing
reads them yet. The Tracker Pre has only `0xe301`, so this is the first hardware
on which the other codes in E-MU's header have been seen to exist.

The terminals reflect the same: `0x0602` (digital audio interface) appears as
both an input and an output terminal, alongside the analog `0x0601`, speaker
`0x0301` and headphone `0x0302` terminals the Tracker Pre has.

### Four-channel alt settings make rate alone ambiguous

36 streaming alt settings against the Tracker Pre's 24. Every rate is offered in
stereo, and 44.1 through 96 kHz are *also* offered in four channels:

| | Stereo | Four-channel |
|---|---|---|
| 44100, 48000 | `bInterval` 3 and 4 | `bInterval` 3 and 4 |
| 88200, 96000 | 3 and 4 | 3 only |
| 176400, 192000 | 3 only | — |

The Tracker Pre already showed that rate does not uniquely identify an alt
setting (alt 11 duplicates alt 3). Here the duplicates are not harmless: picking
a four-channel alt while the ring and the published format are stereo decodes
every packet at half the correct stride. **Alt selection filters on channel
count as well as rate**, in the driver and in `emu-probe play`. The four-channel
modes are otherwise unused — using them means widening `EMU_RING_CHANNELS` and
the Core Audio stream configuration.

Two 16-bit alt settings also appear on the playback interface (alts 17 and 18,
44.1 and 48 kHz). The driver ignores them; 24-bit is available at every rate.

### It is self-powered

`bMaxPower` reads 2 mA, against the Tracker Pre's 500 mA. The 0404 has its own
supply. Nothing depends on this, but it is why the two descriptors differ in the
configuration header.

---

## macOS platform behaviour

The device was the easy half. These cost more.

### Isochronous buffers are laid out by `frReqCount`

Not by what arrived, and **not** at a fixed `wMaxPacketSize` stride either. Frame
*i*'s payload begins where frame *i−1*'s `frReqCount` ended.

- **Writes** set `frReqCount` to the real packet size, so the buffer is packed.
- **Reads** set `frReqCount` to `wMaxPacketSize`, so the stride is fixed and a
  short packet leaves a gap.

Getting this wrong on either side produces audio that is *tonal but wrong* —
noise that tracks the signal you asked for. It changes character with sample
rate, because the gap size changes with the packet size. No counter shows it.

E-MU's driver states it plainly:

```c
source += mInput.maxFrameSize; // each frame's frReqCount is set to maxFrameSize
```

### The classic isochronous API cannot drive a `bInterval 3` endpoint

`ReadIsochPipeAsync` / `WriteIsochPipeAsync` deliver **one frame-list entry per
USB frame**. An endpoint serviced twice per frame therefore only ever gets half
its audio, which sounds like buzzing.

`LowLatencyReadIsochPipeAsync` / `LowLatencyWriteIsochPipeAsync` deliver **one
entry per service interval** — measured with `emu-probe lltest`: 62 entries over
~31 ms of streaming at `bInterval 3`, so 2 per millisecond.

Buffers for the low-latency calls must come from `LowLatencyCreateBuffer`;
ordinary `malloc`'d ones are rejected.

Packing two intervals into one classic entry does *not* work as a substitute. The
stack splits an oversized entry at `wMaxPacketSize`, which at 176.4 kHz is
`89 × 6 + 4` — not a whole number of frames — so both halves land misaligned.

### `kIOReturnUnderrun` is the normal case

An isochronous IN frame that delivers fewer bytes than `wMaxPacketSize` reports
underrun. On an asynchronous endpoint that is *every packet*. Treating it as an
error discards the entire capture.

### Custom properties must be declared, and cannot be integers

The HAL will not forward a selector a plug-in has not published through
`kAudioObjectPropertyCustomPropertyInfoList` — it answers `'who?'`
(`kAudioHardwareUnknownPropertyError`).

Custom properties may only carry a `CFString` or a `CFPropertyList`. Never a raw
integer.

### coreaudiod caches the safety offset

Setting `'emuS'` to 10000 and restarting the stream left
`kAudioDevicePropertySafetyOffset` (output scope) reading 288 frames = the old
6 ms, from a fresh client process, while the plug-in's own `safetyOffsetUS`
diagnostic said 10000 and the engine bound data 10 ms ahead. So coreaudiod
keeps its own copy of the device's safety offset, and neither the driver's
`PropertiesChanged` for that selector nor `kAudioDevicePropertyDeviceHasChanged`
(tried, backed out) nor a stream restart refreshes it; a coreaudiod restart
does. `hal-check safety` prints the HAL's view next to the plug-in's
and flags a disagreement. A mismatch is harmless at large IO buffers (Core
Audio writes buffer + offset ahead, the sweep binds only written data) and
makes the knob a silent no-op at small ones — so `make install` after
changing the default, and treat the runtime knob as an experiment aid.

### Control objects need to appear twice

A volume or mute control must be listed in `kAudioObjectPropertyControlList`
**and** in `kAudioObjectPropertyOwnedObjects` on its scope. Listing only the
streams in `OwnedObjects` makes the controls invisible, and the size calculation
must account for them or the array is truncated.

Setting either the scalar or the decibel value must notify that **both**
changed, or the slider will not follow the volume keys.

### A HAL plug-in does not run in coreaudiod

It loads into `com.apple.audio.Core-Audio-Driver-Service.helper`, which carries
`com.apple.security.cs.disable-library-validation`. That is why a plug-in signed
with an ordinary Apple Development certificate — or ad-hoc — is accepted.

That helper **can open USB devices directly**, which is why this driver is a
single component with no helper daemon.

### Timing

Anchor `GetZeroTimeStamp` to frames the device has actually consumed, not to
`mach_absolute_time`. The difference between two clocks has to go somewhere, and
where it goes is the ring — slowly filling or emptying. At a few ppm that takes
hours, which is what makes it the kind of bug that ships.

Two things then have to be right, and getting either wrong is audible.

**Do not timestamp in the completion callback.** `mach_absolute_time()` there
records when the *callback ran*, which can only ever be later than the transfer,
never earlier. A one-sided error does not average out: it biases the anchor late,
so the device reads slow, so Core Audio delivers slower than the device consumes,
so the ring starves — permanently. Measured at **1880 ppm**, with callback delays
reaching **29 ms**.

`IOUSBLowLatencyIsocFrame` carries `frTimeStamp`, recorded by the USB stack when
the frame actually completed. Using it is the entire reason the low-latency API
reports it. That one change took the drift from 1880 ppm to about 1.4 ppm.

**Update the anchor only when the period advances**, strictly greater rather than
greater-or-equal. Core Audio calls `GetZeroTimeStamp` many times inside one
period and derives the device's rate from consecutive anchors, so re-deriving
`hostTime` for an unchanged `sampleTime` hands it scheduling jitter dressed up as
clock behaviour. Most gets smoothed away, which is why the artifact appears every
few minutes rather than continuously — it only survives when a hiccup makes one
wobble large enough.

Measured after both fixes, over 3.7 minutes at 48 kHz: **zero ring underruns**,
anchor jitter **49 µs** peak against 27 ms before, and 94 frames across 6.2
million — which is oscillation in the pipeline, not drift.

**A FIFO between Core Audio and USB has no fixed phase.** With the rings as
FIFOs, where a frame landed depended on which side had run first: the engine's
in-flight burst drained an empty ring at start, and every later underrun
slipped the phase for good — a burst of crackle ~0.3 s into every stream while
the ring "found" a workable phase, and a ratchet of exactly 8064 frames per
session. Addressing the rings by absolute sample time (`slot = frame mod
size`, an erase head behind the consumer) pins the phase to the published
safety offset by construction, and an underrun becomes one silent packet.
The output side later stopped needing a buffer at all — Core Audio writes into
the USB request that carries the frames — but it is the same discipline: the
map *is* the phase.

**A spliced timeline freezes the IO thread.** Anchoring `GetZeroTimeStamp` to
the host clock until the first completion and then to the device clock, same
seed, made Core Audio absorb the discrepancy by stalling its IO thread for its
length: `hal-trace` shows a 57 ms IO-cycle freeze and a matching burst of ring
underruns, reliably ~0.3 s into every stream. The anchor for sample 0 now comes
from the scheduled bus time of the first packet, published before `StartIO`
returns.

**A timestamp filter that slews through a stall oscillates for minutes.** With
the snap threshold at 25 observation steps (50 ms), a 16–50 ms scheduling gap
was slewed through in the filter's clamp regime, where the damping term's
input is zeroed and the critically damped spring is nearly undamped; the
anchor wobbled for many seconds and dragged Core Audio's write phase back and
forth across the engine's output cursor — `outputLead 0`, hundreds of thousands
of underruns, `usbErrors 0`, a single `tsResets`, cured only by a stream
restart. Hardware timestamps jitter by microseconds, so the threshold is
3 steps: anything past that is a real schedule move.

**A seed change is a glitch of its own.** Measured with a schedule rebuild
that paused the sample clock (cursors held, host time jumped, new seed) after
one ~10 ms Exposé stall with 8 ms of schedule in flight: `unfilledPlayback 2`,
`resyncs 1`, `anchorJitterMaxNs 24282041` (the jump: rebuild lead plus dead
bus time, for an unchanged frame count), and `outputUnderruns 183` against an
`outputLead` of 749 — the bus ran ~19 ms past Core Audio's writes, which is
coreaudiod's IO thread freezing to resynchronise. Three separate glitches
across 50 ms from one stall. With the rebuild skipping the dead bus time on
the same timeline instead (no seed change), the same provocation reads
`resyncs 0, outputUnderruns 0, anchorJitterMaxNs` in the microseconds, and the
only counter that moves is `unfilledPlayback`, for stalls past the offset.

**What Exposé costs the helper's threads, and why the audio does not go
through them.** Under repeated Exposé the engine thread's
completion-to-callback latency exceeds 4 ms a few times a minute and never
8 ms. An earlier output path that converted a staging buffer into the USB
requests on that thread therefore needed an 8 ms tolerance on top of its 2 ms
fill cadence: at a 6 ms offset `unfilledPlayback` rose 8 in ~19 s, and only at
10 ms did it stay 0 over ~37 s. Thread policy cannot widen this — it bounds
when a runnable thread gets the CPU, not when IOUSBLib delivers the completion
that wakes it.

Binding from Core Audio's own IO thread removes that thread from the data path
entirely, and with it the reason for the tolerance. Same 45 s Exposé
provocation, same 4 ms offset, same schedule: `unfilledPlayback 0`,
`framesBound` equal to `framesToOutput`, `bindRaces 0` — against 5 for the
staged path at the same offset. The completion jitter has not gone anywhere;
it is absorbed by schedule depth, which costs wired memory and no latency. A
440 Hz tone was indistinguishable between the two: the counters cannot see a
wrong byte offset, only listening can.

**Core Audio writes further ahead than the safety offset, and by a
predictable amount.** Measured with `bindWriteLead`, the high-water mark of
(cycle end - frames played), swept across every IO buffer size the HAL will
grant:

| buffer | 64 | 128 | 256 | 512 | 1024 | 2048 | 3072 |
|---|---|---|---|---|---|---|---|
| `bindWriteLead` | 624 | 656 | 944 | 1424 | 2448 | 4496 | 6576 |
| less 2 x buffer | 496 | 400 | 432 | 400 | 400 | 400 | 432 |

so

    writeLead ~ 2 x bufferFrames + safetyOffset + ~208 frames

The offset term is exact: the same 512-frame buffer reads 1424 at a 4 ms
offset and 1712 at 10 ms, a difference of 288 frames for 288 frames of
offset. The `2 x` is the HAL's own structure -- it computes an IO cycle's
output time one buffer period ahead of presentation and hands over a
buffer-length range, so the far end lands at offset + 2 x buffer. The residual
~208 frames (4.3 ms) is peak cycle jitter; it is a high-water mark, not a mean.
4096 frames is refused, the HAL capping the buffer at 3072.

A staging buffer never had to care -- 32768 frames long, it absorbs writes for
bus time that has not been scheduled yet. Binding Core Audio's writes to
*already-submitted* USB requests cannot absorb them at all: there is no buffer
for a frame no request covers. With a 32 ms schedule this dropped 5.3% of the
stream at a 512-frame buffer (`unmappedFrames` 7584 of 141824, every one past
the end of the schedule) and 62% at 2048 -- silently, with zero USB errors,
which is why `unmappedAhead` exists as its own counter.

**The write lead follows coreaudiod's cached safety offset, not the driver's.**
The two are normally equal, but `'emuS'` changes only the driver's copy --
coreaudiod refreshes its own on a restart and nothing else (above). Since
`writeLead ~ 2 x buffer + safetyOffset + 208` is Core Audio's arithmetic, it
uses coreaudiod's value: lower the offset at runtime and the HAL keeps writing
at the old, larger one. A schedule sized from the driver's copy would then be
short by exactly the difference and drop it as `unmappedAhead`, silently and
with no USB errors. `schedule_depth` therefore sizes from
`EMU_OUTPUT_SAFETY_MAX_US`, which is also why that ceiling is 20 ms rather
than something generous: it is carried as feedback-servo lag on every session.

**The HAL's cap on a client's IO buffer comes from the driver's own
zero-timestamp period.** Nothing in `AudioHardware.h` or `AudioServerPlugIn.h`
documents how `kAudioDevicePropertyBufferFrameSizeRange` is derived for an
`AudioServerPlugIn`, and this driver never implements the property -- the HAL
synthesises it. Reading it off every device on a test machine:

| device | ZeroTimeStampPeriod | max buffer |
|---|---|---|
| WH-1000XM4 | 2732 | 1024 |
| E-MU 0404 | 8192 | 3072 |
| DELL S2722QC | 12288 | 4096 |
| MacBook Pro Speakers | 14553 | 4096 |
| Teams Audio | 40960 | 4096 |

    maxBufferFrames = min(4096, ZeroTimeStampPeriod * 3/8)

fits all of them, and setting `RING_FRAMES` to 4096, 2048 and 16384 moved the
published maximum to exactly 1536, 768 and 4096 as predicted. So the 3072 this
device advertises is not a Core Audio constant -- it is our own 8192 coming
back, and the minimum (15) has no such fit and was not chased.

This matters because `ZeroTimeStampPeriod` and the output schedule depth are
coupled: the period sets the largest buffer a client can ask
for, the buffer sets how far ahead Core Audio writes, and the schedule has to
reach past that. `EMU_ZERO_TIMESTAMP_PERIOD` lives in `usb_engine.h` for that
reason, and the engine derives `HAL_MAX_IO_BUFFER` and its depth from it
rather than from a measured constant, so changing one moves the other. It is
also the lever for capping client buffers, should that ever be wanted: there
is no need to publish a property the HAL would ignore.

**The fix is depth, and depth is cheap.** The two laws together bound the
worst case, so the depth is computed at stream start from the rate, the
safety offset and `HAL_MAX_IO_BUFFER` (`schedule_depth`) rather than
tuned: 81 requests at 48 kHz, 87 at 44.1 kHz and 31 at 192 kHz against the
20 ms ceiling, with `MAX_REQUESTS` 128 well clear of any of them. Measured at
64, 512, 2048 and 3072 frames it does: `unmappedFrames 0` throughout, `framesBound` equal to what
the client delivered, `bindRaces 0`, `unfilledPlayback 0`. The cost is wired
memory -- 2 entries x 298 bytes per request per direction at 48 kHz, ~120 KB
for 96 requests both ways, under half a megabyte at 192 kHz where `maxpkt` is
586 -- and feedback-servo lag, which does not show:
`frameDeficit` stayed 0 and flat over 30 s and `feedbackStarved` froze at 194,
a startup transient from the initial submissions drawing an empty queue, not a
steady-state figure.

**A standing offset between frames played and frames delivered is not an
error.** It is what is in flight: the output lead plus every scheduled packet
not yet transmitted. What matters is whether it *grows*, not what it is.

### Round-trip latency, measured

The latency published past the safety offset is the stretch USB cannot see:
bus to DAC on the way out, ADC to bus on the way back. A cable measures it
directly. Write a chirp into the output buffer at sample time `So`,
cross-correlate it back out of the input buffer at sample time `Si`, and
`Si - So` is that quantity and nothing else — both are positions on the
timeline the driver publishes, and the safety offsets and the client's buffer
decide *when* Core Audio hands frames over, not where on the timeline they sit.

The measurement says so itself. It is repeatable to **0.00 frames** across six
chirps in a run, identical on both channels to 0.01 frames, and **unchanged
across IO buffers from 64 to 2048 frames** — 273 ± 2 frames throughout — while
the output cycle's lead over the input cycle moves from 560 to 4528, exactly
`2 × buffer + safetyIn + safetyOut`. Whatever it is measuring belongs to the
hardware and not to the buffering above it.

| rate | measured | model | residual |
|---|---|---|---|
| 44.1 kHz | 252.5 frames, 5.726 ms | 5.769 ms | −0.043 ms |
| 48 kHz | 273.2 frames, 5.693 ms | 5.644 ms | +0.049 ms |
| 88.2 kHz | 442.6 frames, 5.018 ms | 4.999 ms | +0.019 ms |
| 96 kHz | 471.6 frames, 4.912 ms | 4.937 ms | −0.025 ms |

Least squares over the four rates gives

    round trip = 67.9 frames + 4.229 ms

**Two terms, because the path holds two kinds of delay.** A converter's group
delay is a filter: a fixed number of *frames* whatever the rate. The device's
own buffering is a fixed *time*. Fit either alone and it misses by
milliseconds at one end of the range — which is what makes the pair of them,
rather than the pair's value at any one rate, the thing worth keeping.

The frame term lands on **67.9** against the 68 the original kext derived from
the DAC and ADC specifications (15 and 53), a tenth of a frame apart, so that
split between the two directions is kept. The time term is halved between them
for want of anything better: a loopback yields only the sum. The driver carries
it as `INTERNAL_ROUND_TRIP_US` 4230 and converts at the current rate, the same
way the safety offsets are specified.

Re-measured against the driver once it carried these figures — a fresh set of
streams, not the ones fitted — the declared latency lands within **0.10 ms** of
the cable at every rate: +0.060 ms at 44.1 kHz, +0.102 at 48, +0.039 at 88.2,
−0.010 at 96. The residual few frames are a per-stream-start effect: the round
trip is exact to 0.00 frames *within* a run and moves by one to four frames
between runs, which is the granularity at which a stream binds to the bus.

The kext's own published round-trip table (`resources/…/Latency.md`, Reaper
with a loopback cable) is an independent check on all of this. Fit
`total = a·(N/rate) + b` to its two DAW buffer sizes and `a` comes out 1.8–2.1,
leaving `b` as that rate's fixed round trip: 5.400 ms at 44.1 kHz, 5.400 at 48,
4.700 at 96 — within 0.33 ms of the figures above, from a different rig, a
different driver and a different decade.

---

## Mistakes that look like hardware faults

Every one of these produced **zero errors, zero short writes, and correct byte
totals** while the audio was wrong or absent. `frActCount == frReqCount` proves
the USB layer moved the bytes. It proves nothing about whether they were the
right bytes in the right place.

| Symptom | Cause |
|---|---|
| Noise that tracks the requested tone, differing by sample rate | Buffer stride wrong in one direction |
| Buzzing at 176.4/192 kHz only | Classic isoc API delivering half the audio |
| Total silence, everything else nominal | Packet size wrong by a constant (`+4`) |
| Full-scale noise on a disconnected input | Producer moving the consumer's ring index |
| Measured rate exactly half nominal | Treating a 0.5 ms entry as 1 ms |
| Measured rate hundreds of ppm fast | Averaging across the startup ramp |
| Ring underruns growing forever, glitch every few minutes | Anchoring the timeline to a timestamp taken in the completion callback |
| Sustained crackle after a stall, every clock counter healthy, cured by restart | A packet the bus never carried moved one timeline cursor and not the others; the shear is permanent |
| One stall, three brief glitches spread over ~50 ms; `anchorJitterMaxNs` in the tens of ms, ring underruns with no HAL overload | The schedule rebuild paused the sample clock instead of skipping the dead bus time; the seed change made the HAL resynchronise, and the anchor jump landed inside the new seed |
| 176.4/192 kHz only: right byte counts, the other channel's tone stronger than this one's, peak pinned at 1.0000 | Frames taken from the packet's first byte, 4 bytes before they start |
| One rate broken and the same rate clean a moment later, no code or setting changed in between | The previous stream ran at `bInterval 3`; look at what came *before* the broken stream, not at the broken stream |
| Measured rate +1000 ppm or more, identical on the first run of every batch | Frames counted after the stream was told to stop: entries drained by `AbortPipe` add to the numerator and not the denominator |
| Capture rate reads high; playback against capture reads hundreds of ppm | Empty intervals dropped from the denominator: frames per *delivered packet* reported as frames per interval |

Two general lessons:

1. **Print the frame list and the buffer.** Reasoning about layout produced
   wrong answers repeatedly; dumping bytes produced right ones in minutes.
2. **Listen.** Three misalignments were identified from the *character* of the
   noise — "it changes with sample rate", "each rate differently", "like the
   192k problem" — while every counter read clean.

### Measuring these rates: one run is not evidence

A `bInterval 3` stream corrupts exactly one following stream, so the results of
consecutive runs alternate. Two consequences, both of which produced confident
wrong conclusions before they were understood:

- **A single warm-up run makes it worse.** It creates the corruption it was
  meant to absorb. Two throwaway runs land the measurement on the clean side;
  `hal-loopback` now does this itself at these rates, and `-P` disables it.
- **The first run after a rate or mode change is unreliable regardless.**
  Priming twice reduces this but does not remove it: measured across five
  repeats at 192 kHz, run 1 failed and runs 2 to 5 were clean, repeatedly.

So compare *proportions of clean runs*, never single runs. `hal-loopback -N`
repeats and reports the count. An A/B of the two input modes done as one run
each showed a large apparent difference that vanished under repetition.

### Self-loopback cannot judge 176.4/192 kHz

The device's own ADC is inside the fault at these rates: capture traffic is
what provokes the playback dropouts, and with the input stream disabled there
is no capture at all. Both make self-loopback either compromised or impossible.

`hal-loopback -i <name|uid>` listens on a second interface instead. Its clock is
its own, so phase and drift become measurements rather than faults; splices and
level still decide the result, and the analyser already separates a slow walk
from a sudden step. Measured against an external interface the noise floor is
also better -- mean THD+N about -70 dB at 48 kHz, against -54 dB through the
device's own converters -- so a dropped packet stands out further.

### A start needs enough lead to submit its whole queue

Starting two devices together reliably lost whichever finished setup second,
with `kIOReturnIsoTooOld` (`0xe00002ee`) on the first playback submission.

The schedule's start frame is chosen as `now + SCHEDULE_LEAD_MS` *before* the
slow part of setup -- setting the clock rate and verifying it by read-back,
selecting alternate settings, allocating the low-latency buffers. Putting the
queue on the bus is then scores of submissions: about 81 requests per direction
at 48 kHz, so roughly 160 calls. Four milliseconds of lead does not survive
that, and does not come close when a second engine is doing the same work on
the same bus.

It is not a failure, it is a stale schedule -- the same thing `reschedule()`
exists for mid-stream. Setup now aborts what reached the bus, rebuilds from the
current bus frame with the lead multiplied by four, and retries, up to four
attempts. The lead is pure startup latency: it shifts the timeline origin once
and costs nothing while running.

Two things this cost before it was understood. The failure is timing-dependent,
so runs of eight consecutive successes proved nothing -- only the log line says
which path executed. And the first fix rebuilt with the *same* lead, which was
always going to hit the same wall.

### A hot-plug watch that tracks the choice cannot serve several devices

The watcher notified the plug-in only when the *chosen* identity changed:

```c
if (was != found && notify && observer) observer();
```

Correct when the driver published one device and had to pick a model. With
several published it is the wrong question: while the preferred product is
attached the choice never changes, so a second device arriving or leaving was
invisible. It was never adopted, and unplugging it was never noticed.

The symptom is asymmetric and confusing: cycling the *preferred* device works
perfectly, cycling any other does nothing at all. The watch now fingerprints
the whole attached set and reports any change to it.

An idle hot-plug test passed before this was found, because that test happened
to cycle the preferred device.

### Nothing came back is one fact, not eleven

An unplugged loopback cable used to produce a level failure, a crosstalk
failure and a spray of "splices" -- all the same fact, none of them about the
driver. `hal-loopback` now says it once and stops for that channel.

The threshold is -100 dBFS, between the two regimes this rig actually shows: a
connected loopback returns -23 to -37 dBFS in use and about -120 dBFS with the
gain right down, while a disconnected one returns digital silence at -143 dBFS.
So it separates "no signal" from "very quiet signal", which is the distinction
that matters.

This cost a full measurement cycle before the check existed: the rig reports a
disconnected cable identically to a broken driver, and only the driver's own
counters (`framesToOutput == framesBound`, zero faults) told them apart.


---

## Open questions

- Why duplex at 176.4 and 192 kHz drops playback packets on @dnadlinger's
  development setup and not on another user's 0404 USB and Tracker Pre.
  Everything the driver controls is ruled out and the loss is past the host. A
  different host controller and a different Mac are the untried variables;
  whether the unit or its firmware differs is unknown.
- What a `bInterval 3` stream leaves behind that corrupts exactly one
  following stream, and why only running a stream clears it. The length word
  says the corruption is a two-byte shift of the IN stream, so something starts
  its read two bytes into the FIFO. Whether every unit does it is also unknown.
- How the internal path's 4.23 ms divides between input and output. A loopback
  yields only the sum, so the driver splits it evenly; the kext asked the same
  question ("Can we measure one-way directly?") and did not answer it.
- What triggers the intermittent silent `SET_CUR` failure.
- What alt 11 is for, given it duplicates alt 3 exactly.
- Whether `kAudioDevicePropertyDeviceIsRunning` needs a change notification; it
  currently reads false while IO is running.
- What the 0404's `0xe302`–`0xe304` extension units accept and report, and
  whether they are the way to reach its S/PDIF and clock-source controls.
- Whether the 0202 USB (`041e:3f02`) matches either shape. Still untested.
