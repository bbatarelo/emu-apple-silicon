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

### Four unexplained bytes at `bInterval 3`

At 176.4 and 192 kHz every **capture** packet is exactly 4 bytes longer than a
whole number of sample frames — 100% of packets, never at `bInterval 4`.

Playback does **not** share this framing: sending `frames × 6 + 4` produces no
audio at all.

Taking only whole frames and ignoring the remainder works. What the bytes are
remains unknown. An earlier conclusion that they are not a header was drawn from
a payload dump that was itself misaligned, so it should not be relied on.

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

For whoever implements MIDI: one embedded jack each way, on **bulk** endpoints
`0x05` OUT and `0x85` IN, with a standard `MS_HEADER` and the usual
in-jack/out-jack pairs. Ordinary USB-MIDI 1.0, unlike the audio side.

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

**A standing offset between frames played and frames delivered is not an
error.** It is buffer occupancy: ring depth plus everything in flight. Here that
is ~1000 + 8 requests × 8 ms × 48 frames/ms ≈ 4072, and the measured figure sits
at 3940–4331. What matters is whether it *grows*, not what it is.

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

Two general lessons:

1. **Print the frame list and the buffer.** Reasoning about layout produced
   wrong answers repeatedly; dumping bytes produced right ones in minutes.
2. **Listen.** Three misalignments were identified from the *character* of the
   noise — "it changes with sample rate", "each rate differently", "like the
   192k problem" — while every counter read clean.

---

## Open questions

- What the 4 bytes per capture packet at `bInterval 3` are.
- What triggers the intermittent silent `SET_CUR` failure.
- What alt 11 is for, given it duplicates alt 3 exactly.
- Whether `kAudioDevicePropertyDeviceIsRunning` needs a change notification; it
  currently reads false while IO is running.
- What the 0404's `0xe302`–`0xe304` extension units accept and report, and
  whether they are the way to reach its S/PDIF and clock-source controls.
- Whether the 0202 USB (`041e:3f02`) matches either shape. Still untested.
