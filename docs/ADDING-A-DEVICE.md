# Adding another E-MU device

The 0202 USB, 0404 USB and Tracker Pre are all built on the CA0189, so they very
likely speak the same protocol. Nobody has checked. If you own one, an hour with
`emu-probe` settles it, and the results are worth having even if you go no
further.

None of this requires modifying the driver, and none of it can damage the
hardware — with one exception, called out below.

## 1. Find out what it is

```bash
make
ioreg -p IOUSB -w0 | grep -i "e-mu\|0x041e"
```

Note the product ID. Then tell the tools about it — `shared/device.h` already
lists the two siblings with `verified` set to false:

```c
static const EmuDeviceIdentity kEmuDevices[] = {
    { 0x3f0a, "E-MU Tracker Pre",  true  },
    { 0x3f02, "E-MU 0202 USB",     false },
    { 0x3f04, "E-MU 0404 USB",     false },
};
```

Those product IDs are educated guesses. If yours differs, correct it — and
correcting a wrong guess is itself a useful contribution.

To probe a different device, change `EMU_DEFAULT_PRODUCT_ID` in the same file and
rebuild. Multi-device support in the plug-in is not written yet; the tools take
whichever device that constant names.

## 2. Read its descriptors

```bash
./build/bin/emu-probe descriptors captures/descriptors/<your-device>.bin
```

Everything the driver needs to know is here. Compare against the Tracker Pre's
table in [FINDINGS.md](FINDINGS.md#rates-and-service-intervals) and look for:

- **Interface class.** The Tracker Pre reports `0xFF` on all three interfaces.
  If yours reports `0x01`, it is a genuine USB Audio Class device and macOS may
  already drive it without any of this.
- **The clock extension unit.** Tracker Pre has exactly one, code `0xe301`, unit
  ID 12. A different unit ID is fine — the driver reads it from the descriptors.
  A *missing* one means clock control works differently and needs investigation.
- **Channel count.** The Tracker Pre is 2 in / 2 out. The 0404 has more inputs,
  which the current driver does not handle: `CHANNELS` is fixed at 2 in
  `driver/plugin.c` and `EMU_RING_CHANNELS` in `driver/ring.h`.
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

## 5. What to send back

Even a partial result is useful:

- the `descriptors` output, and the raw `.bin`
- what `clock` and `clock-support` said
- whether `clock-sweep` verified every rate
- whether `play` produced a clean tone
- the product ID, and the exact model name printed on the box

## What would need writing

If the descriptors match the Tracker Pre's shape, probably very little — the
parser, feedback model and transport are all descriptor-driven already.

If they differ, the likely work is:

- **More than two channels.** Currently fixed at 2 throughout. The ring, the
  format description, and the Core Audio stream configuration all assume stereo.
- **Multi-device support in the plug-in.** It opens `EMU_DEFAULT_PRODUCT_ID` and
  publishes one device. Supporting several at once means one Core Audio device
  object per USB device, and an engine instance per device rather than the
  single global one in `driver/usb_engine.c`.
- **Different extension units.** E-MU's header defines codes `0xe302` through
  `0xe306` for clock source, digital I/O, device options, direct monitoring and
  metering. None appear on the Tracker Pre. A device with S/PDIF will have more,
  and they are the route to exposing its controls.
