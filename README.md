# E-MU Tracker Pre driver for Apple Silicon

A working macOS driver for the E-MU Tracker Pre and 0404 USB audio interfaces.

E-MU stopped supporting macOS in 2011. The last official driver was a kernel
extension, and kernel extensions no longer load on Apple Silicon, so the hardware
has been silent on modern Macs for years. This brings it back.

**It plays and it records.** The device appears in System Settings like any other
audio interface, with working volume and mute, at every sample rate the hardware
supports: 44.1, 48, 88.2, 96, 176.4 and 192 kHz. On the 0404 USB the DIN MIDI
ports work too, as ordinary CoreMIDI endpoints.

No kernel extension. No system extension. No disabling SIP, no lowered security
settings, and no Apple Developer account required.

---

## Installing

You need macOS on Apple Silicon, Xcode command line tools, and Rust.

```bash
xcode-select --install                                    # if you do not have it
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh   # if you do not have Rust
```

Then:

```bash
git clone <this repository>
cd emu-apple-silicon
make
make install
```

`make install` asks for your password, because the drivers go in
`/Library/Audio/Plug-Ins/HAL` and `/Library/Audio/MIDI Drivers`, and restarts
the audio and MIDI daemons. **All audio on the machine stops for a second or
two** — quit anything playing first.

Then open **System Settings → Sound** and choose the device — it appears under
its own name, **E-MU Tracker Pre** or **E-MU 0404 USB** — for output, input, or
both.

That is all. It survives reboots.

### Removing it

```bash
make uninstall
```

---

## Checking that it works

```bash
make check
```

Reports what the driver is doing: whether USB came up, frames played and
captured, buffer depth, and any errors. Run it while something is playing.

```bash
make record
```

Records five seconds from the inputs and tells you what arrived — peak level, RMS
and whether it was silent. Writes a WAV you can listen to. Useful for telling
"nothing is plugged in" apart from "the driver is broken".

If the input meter never moves, check **System Settings → Privacy & Security →
Microphone**. macOS requires permission for audio input, and that is separate
from whether the driver works.

```bash
make loopback
```

With a cable from the outputs back to the inputs, this closes the loop: it
plays a known signal, records what comes back, and says whether the two agree.
That is the only check here that can catch the failure this driver keeps
producing — right byte counts, no errors, wrong audio. It reports whether the
tone came back on the right channel, whether a single frame moved over the run,
and what the round trip actually measures against what the driver declares.

```bash
build/bin/hal-loopback smoke        # a tone goes out and comes back
build/bin/hal-loopback glitches     # a long tone: level, phase, splices, drift
build/bin/hal-loopback latency      # round trip by chirp
build/bin/hal-loopback sweep        # the glitch test at every rate
build/bin/hal-loopback selftest     # the analysers themselves; no hardware

# Listen on a different interface, which is the authoritative measurement and
# the only one possible at 176.4/192 kHz with the input stream disabled.
build/bin/hal-loopback -i M4 -r 192000 -N 5 glitches
```

`make loopback` runs at whatever sample rate the device is set to. To test
another rate, set it first — in Audio MIDI Setup, or by passing `-r <hz>` to
`hal-loopback`, which sets it before the run — or use `sweep`, which does the
glitch test at every rate the device offers.

Set the input gain so the returned tone does not clip; the tests report the
level they saw. 44.1 to 96 kHz pass everywhere. **176.4 and 192 kHz in full
duplex drop about one playback packet a second on some setups**
(@dnadlinger's development machine among them, not every unit reported), and
on that machine either rate leaves the *next* stream corrupt whatever rate it
runs at. See `docs/FINDINGS.md`, and the input mode under Status below.

```bash
./build/bin/midi-check
```

Lists every MIDI endpoint on the system and flags the ones this driver
publishes (0404 USB only — the Tracker Pre's descriptors expose no MIDI).
`midi-check send` plays a middle C out the DIN OUT port; `midi-check dump`
prints whatever arrives at the DIN IN. With a DIN cable looped from the
device's MIDI OUT to its MIDI IN, those two commands in two terminals verify
the whole path an application would use. `./build/bin/emu-probe midi-loopback`
runs the same loop against the raw USB interface instead — use it *before*
installing, or after `make uninstall-midi`, since the installed driver holds
the interface.

---

## Which devices?

**Tracker Pre** (`041e:3f0a`) and **0404 USB** (`041e:3f04`), both verified on
real hardware. One build serves either, and **both at once**: plug in two and
you get two Core Audio devices, each with its own name, controls and transport,
streaming independently at any rate either supports.

The **0202 USB** is the one still untested. It is built on the same CA0189
chipset and should speak the same protocol; the 0404 turned out to, needing a
single parser fix. If you own one, `tools/emu-probe` will tell us in a few
minutes. See **[docs/ADDING-A-DEVICE.md](docs/ADDING-A-DEVICE.md)** — no driver
changes needed to find out, and the results are genuinely useful even if you
stop there.

Up to `EMU_MAX_DEVICES` (4) are published. A fifth is ignored.

**MIDI is still single-device**: with two units attached, MIDI endpoints appear
for the preferred one only (`EMU_DEFAULT_PRODUCT_ID` in `shared/device.h`).
Audio is unaffected.

### Device names, and a one-time change

Each device is identified by its unit's serial number:

    net.quantum-bit.EMUTrackerPre.E-MU-69-3F04-07D8031A-0F419-STATION 03

A single fixed name cannot identify two boxes, so this replaced the old
`net.quantum-bit.EMUTrackerPre`. **Upgrading from a version before multi-device
resets your chosen output device once** — macOS keys that choice on the UID and
sees a new name. Select the device again and it sticks.

The serial is what makes the choice stick afterwards. It survives replugging and
moving to another port, so an interface you unplug and plug back in returns as
*the same device* and playback resumes on its own, rather than coming back as a
stranger you have to re-select.

### Pointing the tools at one device

With several attached, `EMU_DEVICE` selects by any part of the UID or serial —
the product ID is usually enough:

```bash
EMU_DEVICE=3F04 build/bin/hal-check           # the 0404
EMU_DEVICE=3F0A build/bin/hal-check           # the Tracker Pre
EMU_DEVICE=3F04 build/bin/hal-loopback -i M4 -r 192000 -N 3 glitches
```

Without it, the first device found wins, which is the single-device case
unchanged. `emu-probe` talks to the hardware rather than to Core Audio, so it
uses `EMU_PRODUCT=3f04` instead.

---

## How it works

A Core Audio HAL plug-in, which is the supported way to add an audio device from
userspace. It talks to the hardware over USB through IOKit, and does the
isochronous streaming, clock recovery and format conversion itself. MIDI is a
second, much smaller plug-in of the same shape loaded by MIDIServer, because
macOS keeps audio and MIDI in separate daemons; the two share the USB device by
each claiming only its own interface.

```
   application
        |
   Core Audio HAL           Float32, fixed buffers, host clock
        |
   AudioServerPlugIn        this driver
        |
   timeline-indexed buffers  the join between two clocks
        |
   USB engine thread        24-bit packed, packets sized by the device's clock
        |
   low-latency isochronous
        |
   the device
```

The interesting part is that the device's clock, not the computer's, decides how
fast audio moves. Capture normally runs even when only playback is wanted,
because the capture stream is how the driver measures what the hardware is
actually doing; with capture switched off, the device's own feedback endpoint
takes over.

More in **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

---

## Status

Working: playback, capture, all six sample rates, master volume and mute,
correct channel mapping, clock tracking anchored to the device, latency
reported from a loopback measurement, and MIDI in and out on the 0404 USB —
verified byte-for-byte through a DIN loopback cable.

Several devices at once: two interfaces publish separately and stream together
at 192 kHz, each with exact frame accounting. Unplugging one mid-stream leaves
the other untouched, and the removed device is re-adopted on replug — verified
with an application playing through it, which resumed by itself.

The engine also survives a transport fault: it rebuilds the stream with backoff
rather than dying, logs what happened, and if it exhausts its budget it tells
Core Audio the device is gone instead of streaming into nothing.
`build/bin/hal-check fault transient|persistent|none` exercises that path, and
`make test-recovery` checks it end to end.

Known issue: on some setups — @dnadlinger's development 0404 USB among them,
though another user's 0404 USB and Tracker Pre are reported clean — full
duplex at 176.4 and 192 kHz crackles, about one dropped playback packet a
second.
`build/bin/hal-check input auto` runs those two rates playback-only, which is
clean, at the cost of the input at those rates; `hal-check input on` restores
it. See `docs/FINDINGS.md`.

Not done:

- MIDI on the Tracker Pre — but there is nothing to do. It has no DIN
  connectors on the chassis and advertises no MIDI-streaming interface, so
  there is no port to reach. The 0404 has both, and its MIDI works.
- The devices' own controls — pad, phantom power, direct monitoring, and the
  0404's S/PDIF — are not exposed. The Tracker Pre advertises only one extension
  unit, so some may not be reachable; the 0404 has three more that nothing reads
  yet.
- Stereo only. The 0404 also offers four-channel modes, which are ignored.
- MIDI is single-device: with two units attached, endpoints appear for the
  preferred one only.
- More than four devices at once. A fifth is ignored silently.
- The 0202 USB is untested.

---

## Documentation

| | |
|---|---|
| [docs/FINDINGS.md](docs/FINDINGS.md) | What the hardware does, and what macOS does. **Read this before changing anything.** |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the driver is put together |
| [docs/ADDING-A-DEVICE.md](docs/ADDING-A-DEVICE.md) | Bringing up an 0202 or another sibling |
| [docs/DESIGN-GUIDELINES.md](docs/DESIGN-GUIDELINES.md) | The architectural reference the project was built against |
| [docs/provenance.md](docs/provenance.md) | Reference material in `resources/`, and its licensing |
| [driverkit/](driverkit/) | An unfinished DriverKit version, and why it is parked |

---

## Contributing

The most useful thing anyone can do is run `tools/emu-probe` against an 0202 and
send the output. It is the last member of the family nobody has checked.

After that: MIDI, the 0404's unread extension units, and reports of whether
176.4 and 192 kHz are clean in full duplex on your unit — `make loopback` with
a cable says, and FINDINGS explains why it matters.

---

## Licence

MIT — see [LICENSE](LICENSE).

`resources/` contains third-party material that is **not** covered by that
licence. See [docs/provenance.md](docs/provenance.md).
