# Preliminary: Xcode, Build, Signing, and Command-Line Workflow

## Can this be built without opening Xcode?

There are two different meanings of “without Xcode”:

- **Without ever using the Xcode GUI:** yes, absolutely feasible.
- **Without having Xcode installed:** realistically, no.

For DriverKit development, the full Xcode toolchain is the practical choice because it provides the DriverKit SDK, AudioDriverKit/USBDriverKit headers and libraries, `xcodebuild`, interface-generation tooling, signing integration, and related Apple development infrastructure.

The recommended goal for this project is therefore:

> **Install Xcode, use it to bootstrap/configure the DriverKit project, then make normal development completely command-line driven.**

## Recommended Repository Layout

A sensible structure would be:

```text
repo/
├── DriverKit/
│   ├── EMUDriver.xcodeproj
│   ├── Driver/
│   └── HostApp/
│
├── rust/
│   └── emu-ca0189/
│
├── scripts/
│   ├── build.sh
│   ├── install.sh
│   ├── uninstall.sh
│   ├── logs.sh
│   └── inspect-signing.sh
│
└── Makefile
```

The normal developer workflow should ideally become:

```bash
make build
make install
make logs
```

Underneath, `make build` could perform both the Rust and DriverKit builds:

```bash
cargo build ...

xcodebuild \
    -project DriverKit/EMUDriver.xcodeproj \
    -scheme EMUDriver \
    -configuration Debug \
    -derivedDataPath build \
    build
```

## Use Xcode Once to Bootstrap the Project

The initial project should still be created/configured in Xcode because Apple provides the appropriate DriverKit templates and project metadata.

The initial structure would be approximately:

```text
EMUTrackerPre
    │
    ├── macOS host application
    │
    └── DriverKit driver extension
            ├── AudioDriverKit
            └── USBDriverKit
```

Configure once:

- bundle identifiers;
- Development Team;
- DriverKit entitlements;
- system-extension entitlement;
- USB matching information;
- embedding the `.dext` inside the host app;
- signing settings.

Then commit the resulting `.xcodeproj`.

After that, the Xcode GUI should be optional.

Xcode becomes primarily:

> **SDK + compiler/build infrastructure + occasional project configuration editor**

Source code can be edited in any editor.

## Command-Line Signing and Build

Once certificates and provisioning are configured, the build/signing workflow can be managed from Terminal.

Inspect available signing identities:

```bash
security find-identity -v -p codesigning
```

Build:

```bash
xcodebuild \
    -project DriverKit/EMUDriver.xcodeproj \
    -scheme EMUTrackerPre \
    -configuration Debug \
    -allowProvisioningUpdates \
    build
```

Inspect the host application signature:

```bash
codesign -dv --verbose=4 EMUTrackerPre.app
```

Inspect entitlements:

```bash
codesign -d --entitlements :- EMUTrackerPre.app
```

Likewise inspect the embedded driver extension:

```text
EMUTrackerPre.app/
└── Contents/
    └── Library/
        └── SystemExtensions/
            └── EMUTrackerPreDriver.dext
```

For eventual public distribution, notarization should also be scriptable:

```bash
xcrun notarytool ...
```

This means CI/CD is possible as well.

## The Driver Lives Inside a Host Application

Unlike old kernel extensions, DriverKit system extensions are normally packaged inside a macOS application.

Initially the host app can be extremely small:

```text
EMU Tracker Pre Driver

Driver status: Active

[ Activate Driver ]
[ Deactivate Driver ]
```

Later it can evolve into the SwiftUI diagnostics/control application.

The app can still be launched from the command line:

```bash
open build/Debug/EMUTrackerPre.app
```

## Rust Build Integration

For the proposed Rust-core architecture, the minimal test project should be fully automated.

Example:

```bash
./scripts/build-rust-driverkit-probe.sh
```

The script should perform:

```text
1. cargo/rustc
      ↓
   libemu_probe.a

2. verify archive architecture/symbols
      ↓

3. xcodebuild DriverKit target
      ↓

4. verify final dext
      ↓

5. inspect signing + entitlements
      ↓

6. package host .app
```

The eventual normal build should be:

```bash
make build
```

with dependencies arranged as:

```text
Cargo
  ↓
Rust static library
  ↓
C ABI
  ↓
C++ DriverKit adapter
  ↓
xcodebuild
  ↓
signed .dext
  ↓
embedded in signed .app
```

## Should the `.xcodeproj` Be Eliminated Entirely?

Technically, it may be possible to manually invoke the compiler and linker with a DriverKit target, for example using an Apple DriverKit target triple and DriverKit SDK paths, then manually run generated-interface stages, construct bundles, and sign everything.

That is **not recommended**.

Doing so would effectively mean reverse-engineering Xcode’s DriverKit build system while also developing the E-MU driver. That introduces unnecessary risk and complexity.

A better separation is:

```text
                      source/build interface
                              │
                 ┌────────────▼────────────┐
                 │  Makefile / scripts     │
                 │  cargo / command line   │
                 └────────────┬────────────┘
                              │
                ┌─────────────┴─────────────┐
                ▼                           ▼
             cargo                      xcodebuild
                │                           │
             Rust                       DriverKit
```

The `.xcodeproj` remains an implementation detail of the Apple build.

## Recommended Policy for This Project

The project should follow these rules:

1. Install the full Xcode toolchain.
2. Use Xcode to bootstrap the host app and DriverKit extension once.
3. Commit all project configuration to the repository.
4. Make the full build available from the command line.
5. Make signing and entitlement inspection scriptable.
6. Make Rust compilation part of the same build pipeline if Rust is adopted.
7. Avoid workflows that require opening Xcode and clicking buttons for normal development.
8. Treat “works from a fresh shell with one command” as an acceptance criterion for the minimal Rust/DriverKit probe.

A good target is:

```bash
make build
```

for a complete build, followed by separate scripted commands for installation, logging, diagnostics, and cleanup.

## Bottom Line

The recommended approach is:

> **Use Xcode as the installed Apple SDK/build backend, not as a required interactive IDE.**

The project should remain fully usable from Terminal after the initial bootstrap. This keeps the workflow reproducible, CI-friendly, automation-friendly, and much easier to maintain during low-level driver development.
