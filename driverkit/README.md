# The DriverKit version

**This does not work, and is not part of the build.** `make` in the repository
root ignores this directory entirely.

It is kept because it answers a question that took real effort to settle, and
because the working driver may eventually want to become this.

## Why the shipping driver is not a dext

A DriverKit system extension is the modern, Apple-sanctioned way to write a
driver. It is also gated: `com.apple.developer.driverkit`,
`.family.audio` and `.transport.usb` are restricted entitlements that Apple must
grant to your team by hand, and the review takes weeks.

The Core Audio HAL plug-in in `driver/` needs none of that. It works today, on an
ordinary machine, with no entitlement and no lowered security. So that is what
ships.

## What was established here

**A `no_std` Rust static library links into an Apple Silicon dext and runs.**
That was not obvious, and the obvious approach fails.

- Linking a normal `aarch64-apple-darwin` archive into a dext is rejected: `ld`
  refuses to mix macOS platform metadata into a DriverKit image. That archive
  also pulls in `_sysctlbyname`, `_rust_eh_personality` and `___stack_chk_fail`,
  none of which DriverKit provides.
- A custom `aarch64-apple-driverkit` target works, on stock nightly, with no
  compiler fork, no LLVM patch and no suppressed linker checks. Setting
  `os = "driverkit"` also stops `compiler_builtins` compiling its macOS paths:
  the archive drops from 368 members to 3, and every libSystem and Rust-runtime
  dependency disappears.
- `is-like-darwin` must be **false**, or rustc hits `unreachable!()` in
  `deployment_target()` for an OS it does not know. That cascades into `vendor`
  and `linker-flavor` changes through rustc's own consistency rules.

Details in [rust-driverkit-target.md](rust-driverkit-target.md), results in
[milestone-0-results.md](milestone-0-results.md).

## What was never verified

The dext **links** and contains the Rust code. It has never **loaded**, because
that needs the entitlement. Linking is not running: a dext that links can still
fail at load, at `Start`, or during teardown.

## Building it

Needs nightly Rust and `-Z build-std`, which is the main reason it is kept out of
the default build — everything else in this repository builds on stable.

```bash
rustup toolchain install nightly --component rust-src
cd driverkit
./scripts/build-rust.sh          # freestanding core for the DriverKit target
./scripts/build-driverkit.sh     # dext and host app, unsigned
```

Signing is off by default, since without the entitlement there is no provisioning
profile to sign against. The result compiles, links and validates, and cannot
load.

## If the entitlement ever arrives

[driverkit-entitlement-request.md](driverkit-entitlement-request.md) describes
what to ask Apple for and how to argue it. The entitlements files and
provisioning configuration are already in place, so:

```bash
./scripts/build-driverkit.sh SIGNING=1
./scripts/install.sh SIGNING=1
```

The protocol core in `rust/emu-ca0189` is shared and needs no changes; it already
compiles `no_std`. The USB transport would move from IOKit's `IOUSBLib` to
USBDriverKit, and Core Audio presentation from an `AudioServerPlugIn` to
`IOUserAudio` objects. Everything in
[docs/FINDINGS.md](../docs/FINDINGS.md) about the device still applies.

## preliminary.md

The original command-line and signing workflow policy, from before any of this
was built. Kept for context; largely superseded by what the repository actually
does now.
