# Adding another E-MU device

The 0202 USB, 0404 USB and Tracker Pre are all built on the CA0189, so they very
likely speak the same protocol.

**The 0404 USB has now been checked, and it does** — the clock unit, the rate
codes and the whole transport carried over unchanged. It took one parser fix,
for a MIDI interface the Tracker Pre does not have. What that looked like is
written up in [FINDINGS.md](FINDINGS.md#the-0404-usb), and it is a fair guide to
what the 0202 will cost.

Only the **0202 USB** is left. If you own one, an hour with `emu-probe` settles
it, and the results are worth having even if you go no further.

None of this requires modifying the driver, and none of it can damage the
hardware — with one exception, called out below.

## 1. Find out what it is

```bash
make
ioreg -p IOUSB -w0 | grep -i "e-mu\|0x041e"
```

Note the product ID. `shared/device.h` lists what the tools know about:

```c
static const EmuDeviceIdentity kEmuDevices[] = {
    { 0x3f0a, "E-MU Tracker Pre",  true  },
    { 0x3f02, "E-MU 0202 USB",     false },
    { 0x3f04, "E-MU 0404 USB",     true  },
};
```

The 0202's product ID is an educated guess. If yours differs, correct it —
correcting a wrong guess is itself a useful contribution.

Nothing else needs changing to probe a device in that table. The tools and the
driver look for each known product in turn and take whichever is attached;
`EMU_DEFAULT_PRODUCT_ID` only decides which one wins if you have two plugged in
at once. Multi-device support in the plug-in is not written yet — it publishes
one Core Audio device whichever hardware it finds.

## 2. Read its descriptors

```bash
./build/bin/emu-probe descriptors captures/descriptors/<your-device>.bin
```

If it fails to parse, the raw bytes are still written to the file you named.
Send those — a descriptor this parser rejects is exactly the interesting case,
and it is how the 0404's MIDI interface was found.

Everything the driver needs to know is here. Compare against the Tracker Pre's
table in [FINDINGS.md](FINDINGS.md#rates-and-service-intervals) and look for:

- **Interface class.** The Tracker Pre reports `0xFF` on all three interfaces.
  If yours reports `0x01`, it is a genuine USB Audio Class device and macOS may
  already drive it without any of this.
- **The clock extension unit.** Tracker Pre has exactly one, code `0xe301`, unit
  ID 12; the 0404 has the same one plus three others. A different unit ID is
  fine — the driver reads it from the descriptors. A *missing* one means clock
  control works differently and needs investigation.
- **Channel count.** The driver is stereo throughout: `CHANNELS` is fixed at 2 in
  `driver/plugin.c` and `EMU_RING_CHANNELS` in `driver/ring.h`. The 0404 also
  advertises four-channel alt settings, which is fine — alt settings are
  selected by channel count as well as rate, so the stereo ones are used and the
  rest ignored. A device offering *only* more than two channels would need real
  work.
- **Alt settings and rates.** Note which use `bInterval 3` versus `4`; that
  distinction matters more than it looks.

## 3. Check clock control

Read-only first:

```bash
./build/bin/emu-probe clock
./build/bin/emu-probe clock-support
```

If those answer sensibly, the extension-unit protocol is shared and most of the
work is already done.

**Then the one risky step.** `clock-sweep` writes to the device:

```bash
./build/bin/emu-probe clock-sweep
```

It sets every rate, verifies each by read-back, and restores what it found. On
the Tracker Pre this is safe and repeatable.

The hazard is documented in FINDINGS: selecting an alternate setting whose rate
disagrees with the active clock wedges the device into refusing all control
transfers, and only a physical replug recovers it. `emu-probe` sets the rate
before any alt setting and refuses to continue if the rate did not verify, so it
should not happen — but if the device goes unresponsive, **unplug it and plug it
back in**. Do not use `emu-probe reset`; re-enumeration was observed to lose the
device entirely.

## 4. Check streaming

```bash
./build/bin/emu-probe capture 48000 4000 /tmp/trace.csv
```

Look for `deviation` within a few ppm and a sane packet size distribution. Then
the real test — this plays a 440 Hz tone, so turn the volume down:

```bash
./build/bin/emu-probe play 48000 5000 440 15
```

`STABLE` with a small tracking error, and an audible clean sine, means the
transport works and the device is supportable.

## 5. MIDI, if it has it

If `descriptors` printed a `MIDI interface` line, the CoreMIDI driver should
already handle it — the transport is descriptor-driven. Connect a DIN cable
from the device's MIDI OUT to its MIDI IN and run:

```
./build/bin/emu-probe midi-loopback
```

`PASS` means the pipes, the packet framing and the physical ports all work.
(If the MIDI driver is already installed, `make uninstall-midi` first — it
holds the interface.) Then `make install` and verify the CoreMIDI path the
same way: `./build/bin/midi-check dump` in one terminal, `send` in another.

## 6. Make it official

If all of that worked, two changes finish the job:

- set `verified` to `true` for the device in `shared/device.h`
- commit the raw `.bin` under `captures/descriptors/` and add a test over it in
  `rust/emu-ca0189/tests/descriptor_tests.rs`

The fixture is the part that lasts. The 0404's tests pin down the alt-setting
count, the clock unit and the fact that a stereo alt exists at every rate — all
things a future refactor could quietly break with no hardware in the room.

## What to send back

Even a partial result is useful:

- the `descriptors` output, and the raw `.bin` — parsed or not
- what `clock` and `clock-support` said
- whether `clock-sweep` verified every rate
- whether `play` produced a clean tone
- the product ID, and the exact model name printed on the box

## What would need writing

If the descriptors match the shape of the two known devices, probably very
little — the parser, feedback model and transport are all descriptor-driven
already, and the 0404 needed no changes to any of them.

If they differ, the likely work is:

- **More than two channels.** Currently fixed at 2 throughout. The ring, the
  format description, and the Core Audio stream configuration all assume stereo.
  A device that offers stereo alongside more channels costs nothing; one that
  offers only more does.
- **Multi-device support in the plug-in.** It publishes one Core Audio device
  even when several are attached. Supporting them all means one device object
  per USB device, and an engine instance per device rather than the single
  global one in `driver/usb_engine.c`.
- **Different extension units.** E-MU's header defines codes `0xe302` through
  `0xe306` for clock source, digital I/O, device options, direct monitoring and
  metering. The Tracker Pre has none of them; the 0404 has three, still unread.
  They are the route to exposing a device's own controls.
- **MIDI.** Written, for devices that advertise USB-MIDI 1.0 the way the 0404
  does: the parser picks up the interface and the CoreMIDI plug-in drives it
  with no per-device code. A device whose MIDI is *not* standard USB-MIDI —
  the Tracker Pre advertises no MIDI interface at all despite having DIN
  ports — is unexplored territory.
