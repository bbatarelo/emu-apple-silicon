# E-MU Tracker Pre driver for Apple Silicon

A working macOS driver for the E-MU Tracker Pre USB audio interface.

E-MU stopped supporting macOS in 2011. The last official driver was a kernel
extension, and kernel extensions no longer load on Apple Silicon, so the hardware
has been silent on modern Macs for years. This brings it back.

**It plays and it records.** The device appears in System Settings like any other
audio interface, with working volume and mute, at every sample rate the hardware
supports: 44.1, 48, 88.2, 96, 176.4 and 192 kHz.

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

`make install` asks for your password, because the driver goes in
`/Library/Audio/Plug-Ins/HAL`, and restarts the audio daemon. **All audio on the
machine stops for a second or two** — quit anything playing first.

Then open **System Settings → Sound** and choose **E-MU Tracker Pre** for output,
input, or both.

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

---

## Does it work with the 0202 or 0404?

Not yet, but it is the obvious next step. All three are built on the same CA0189
chipset and should speak the same protocol.

If you own one, `tools/emu-probe` will tell us in a few minutes whether it does.
See **[docs/ADDING-A-DEVICE.md](docs/ADDING-A-DEVICE.md)** — no driver changes
needed to find out, and the results are genuinely useful even if you stop there.

---

## How it works

A Core Audio HAL plug-in, which is the supported way to add an audio device from
userspace. It talks to the hardware over USB through IOKit, and does the
isochronous streaming, clock recovery and format conversion itself.

```
   application
        |
   Core Audio HAL           Float32, fixed buffers, host clock
        |
   AudioServerPlugIn        this driver
        |
   lock-free ring           the join between two clocks
        |
   USB engine thread        24-bit packed, packets sized by the device's clock
        |
   low-latency isochronous
        |
   E-MU Tracker Pre
```

The interesting part is that the device's clock, not the computer's, decides how
fast audio moves. Capture runs even when only playback is wanted, because the
capture stream is how the driver measures what the hardware is actually doing.

More in **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

---

## Status

Working: playback, capture, all six sample rates, master volume and mute,
correct channel mapping, clock tracking anchored to the device.

Not done:

- MIDI. The Tracker Pre has MIDI in and out; this driver ignores them entirely.
- The device's own controls — pad, phantom power, direct monitoring — are not
  exposed. Some may not be reachable; the descriptors only advertise one
  extension unit.
- Latency is reported as an estimate rather than a measurement.
- Only the Tracker Pre is supported, though the 0202 and 0404 are likely close.

---

## Documentation

| | |
|---|---|
| [docs/FINDINGS.md](docs/FINDINGS.md) | What the hardware does, and what macOS does. **Read this before changing anything.** |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the driver is put together |
| [docs/ADDING-A-DEVICE.md](docs/ADDING-A-DEVICE.md) | Bringing up an 0202, 0404 or another sibling |
| [docs/DESIGN-GUIDELINES.md](docs/DESIGN-GUIDELINES.md) | The architectural reference the project was built against |
| [docs/provenance.md](docs/provenance.md) | Reference material in `resources/`, and its licensing |
| [driverkit/](driverkit/) | An unfinished DriverKit version, and why it is parked |

---

## Contributing

The most useful thing anyone can do is run `tools/emu-probe` against an 0202 or
0404 and send the output. That is what decides whether this covers the family.

After that: MIDI, and the four unexplained bytes documented in FINDINGS.

---

## Licence

MIT — see [LICENSE](LICENSE).

`resources/` contains third-party material that is **not** covered by that
licence. See [docs/provenance.md](docs/provenance.md).
