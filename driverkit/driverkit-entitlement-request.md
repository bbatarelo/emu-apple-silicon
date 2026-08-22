# DriverKit entitlement request

**Status as of 2026-08-20: not requested.** This is the single blocking item for
Milestone 0 Tests 3 and 4, and for every milestone after it.

## Why the build currently cannot be signed

The dext declares restricted entitlements
(`TrackerPreDriver/TrackerPreDriver.entitlements`). Xcode can only issue a
matching *DriverKit App Development* provisioning profile if the team has been
granted those entitlements by Apple. Without the grant:

```
error: No profiles for 'net.quantum-bit.TrackerPreDriver' were found:
Xcode couldn't find any DriverKit App Development provisioning profiles
matching 'net.quantum-bit.TrackerPreDriver'.
```

This is why `make build` defaults to `SIGNING=0` (`CODE_SIGNING_ALLOWED=NO`).
That still compiles, links and validates the dext — which is enough to have
settled the Rust/C++ architecture question — but the result cannot be loaded.

## What to request

File the DriverKit / system extension entitlement request from
<https://developer.apple.com/contact/request/> (the form covering DriverKit and
System Extension entitlements) for team **T59L53M882**.

Ask for all three:

| Entitlement | Why this project needs it |
|---|---|
| `com.apple.developer.driverkit` | Base entitlement. Required for any dext to load at all. |
| `com.apple.developer.driverkit.family.audio` | Required to publish `IOUserAudio` objects to Core Audio. Without it the driver cannot present an audio device. |
| `com.apple.developer.driverkit.transport.usb` | Required to claim USB interfaces, set alternate settings, and run control and isochronous transfers. |

`com.apple.developer.driverkit.userclient-access` is also declared, so the
SwiftUI control app can read diagnostics over a user client. It is normally
granted alongside the base entitlement.

The host app's `com.apple.developer.system-extension.install` is **not**
restricted and needs no approval.

## Useful detail to include in the request

Apple asks what the driver does and why an existing class driver is not enough.
The honest and accurate answer here:

- Device: **E-MU Tracker Pre**, USB audio interface, Creative VID `0x041e`,
  PID `0x3f0a`, built on the CA0189 chipset.
- The vendor abandoned macOS support in 2011; the last driver was a KEXT, and
  KEXTs no longer load on Apple Silicon. The hardware is otherwise functional.
- The device presents **no USB Audio Class interfaces at all**. All three of its
  interfaces report `bInterfaceClass = 0xFF` (vendor-specific), confirmed by
  reading the descriptors off the hardware — see
  [milestone-1-2-results.md](milestone-1-2-results.md). macOS's `AppleUSBAudio`
  driver consequently never attaches, and the device produces no audio on a
  stock system. Sample rate is selected through a vendor extension unit
  (`0xe301`) rather than any standard clock mechanism.
- This is the substantive justification: a class-compliant device would not need
  a custom driver, and this one is not remotely class compliant.
- This is a hardware preservation project for hardware the owner already has.

## Expected timeline

Apple's review is manual and has historically taken weeks. Request early.

## What unblocks the moment it is granted

Nothing in the repository needs restructuring. The entitlements files and the
provisioning configuration are already in place, so once a DriverKit profile is
available:

```bash
make build SIGNING=1
make install SIGNING=1
make logs
```

The driver's `Start()` already runs the Milestone 0 probe and logs
`Rust-in-DriverKit probe PASSED`, which is exactly the Test 3 success criterion.

## If the request is refused

Approval is not guaranteed for an individual account. If it is refused, the
fallback is to disable SIP and run with
`systemextensionsctl developer on`, which permits locally-signed development
extensions. That is a viable development path but is not a distributable one,
and it materially changes the security posture of the machine. Treat it as a
decision to make deliberately, not a default.
