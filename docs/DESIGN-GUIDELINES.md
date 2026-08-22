# E-MU Tracker Pre Apple Silicon Revival

## Development Guidelines and Language/Architecture Options

**Companion to:** `EMU_Tracker_Pre_Apple_Silicon_Final_Spec_v1.0.md`  
**Target:** Apple Silicon macOS, AudioDriverKit + USBDriverKit  
**Status:** v1.0 development guidance  
**Date:** 2026-08-20

---

## 1. Purpose

This document records the recommended development strategy for the Apple Silicon E-MU Tracker Pre revival project, with particular emphasis on language choice and software boundaries.

It compares two viable implementation approaches:

1. **Conventional C++ DriverKit implementation** — the lowest-risk path to a working driver.
2. **Rust core + minimal C++ DriverKit adapter** — a more ambitious architecture that moves most device logic into safe, portable Rust while keeping Apple's C++ DriverKit boundary intentionally thin.

A Swift/SwiftUI management and diagnostics application is useful with either approach and is treated as a separate layer.

The document also defines a **minimal feasibility project** whose only purpose is to answer the most important unresolved tooling question before adopting Rust for the driver core:

> Can an Apple Silicon DriverKit dext reliably link and execute a small freestanding Rust static library under the DriverKit target/runtime constraints?

The project should not commit to the Rust architecture until this test succeeds cleanly.

---

## 2. Starting constraints

The project is not a normal macOS application.

The driver-facing APIs are Apple's DriverKit frameworks:

- AudioDriverKit for the Core Audio-facing driver objects and audio streams;
- USBDriverKit for USB interface ownership, alternate settings, pipes, control requests and isochronous transfers;
- DriverKit/IIG machinery for service objects, user-client communication and lifecycle.

Apple exposes this layer primarily as a **C++ object model**. Therefore an implementation that directly subclasses DriverKit classes naturally contains at least some C++.

The relevant historical implementation evidence is also C++:

- E-MU's released CA0188/CA0189 driver source;
- E-MU's final 2011 shipping KEXT, whose retained C++ symbols map directly to the released source;
- Wouter's later refactor and macOS compatibility work.

This makes C++ the lowest-friction language for the Apple boundary and also makes source-to-source auditing convenient.

Rust remains highly attractive for the protocol/state-machine/real-time core, but the current Rust platform list does **not** provide an officially supported `aarch64-apple-driverkit` target with normal `std` support. The Rust route should therefore be treated as a deliberate freestanding/FFI architecture rather than assuming an ordinary macOS Rust build can simply be dropped into a dext.

References checked for this guidance:

- Apple DriverKit documentation: <https://developer.apple.com/documentation/driverkit>
- Apple AudioDriverKit WWDC introduction: <https://developer.apple.com/videos/play/wwdc2021/10190/>
- Rust platform support: <https://doc.rust-lang.org/rustc/platform-support.html>

---

## 3. Common architecture regardless of language choice

The high-level design should remain the same under both implementation approaches.

```text
Core Audio HAL
      |
      v
+----------------------------------------------+
| TrackerPreDriver.dext                        |
|                                              |
| AudioDriverKit-facing device/stream objects  |
|                |                             |
|                v                             |
|          DuplexIsochEngine                   |
|           |      |      |                    |
|           |      |      +-- ring/timeline    |
|           |      +--------- clock estimator  |
|           +---------------- packet planner   |
|                |                             |
|                v                             |
|          CA0189 protocol                     |
|                |                             |
|                v                             |
|          USBDriverKit adapter                |
+----------------+-----------------------------+
                 |
                 v
          E-MU Tracker Pre
```

The language decision should **not** change the core protocol rules established in the main specification.

In particular:

- descriptors are authoritative for interface/alternate-setting/endpoint topology;
- E-MU-specific extension-unit semantics are modeled separately from generic USB transport;
- sample-rate changes are verified with `SET_CUR -> GET_CUR -> compare`;
- the capture stream is used internally even during playback-only Core Audio usage when required for hardware-clock feedback;
- feedback queue entries represent **sample frames per USB service interval**, not raw bytes;
- USB packet sizes are derived at the endpoint boundary using that direction's own `bytesPerFrame`;
- the real-time path performs no unbounded allocation and no blocking work;
- driver lifecycle is represented explicitly rather than through loosely related booleans;
- diagnostics are first-class, not an afterthought.

---

# Part I — Approach A: C++ DriverKit implementation

## 4. When to choose C++

Choose an all-C++ driver when the primary objective is:

> Get a stable Tracker Pre driver running on Apple Silicon with the least possible build-system, ABI and deployment uncertainty.

This is the safest first implementation path because:

- DriverKit itself is a C++ API;
- Apple's examples and expected subclassing model are C++;
- E-MU's published implementation is C++;
- Wouter's implementation is C++;
- there is no additional language ABI boundary in the streaming path;
- Xcode owns the entire dext build and signing process;
- source-level comparison with historical implementations is straightforward.

The C++ approach is therefore the recommended fallback even if the long-term preference is Rust.

---

## 5. Suggested C++ project decomposition

Do **not** recreate the historical monolithic KEXT design.

Keep Apple APIs at the edge and keep most protocol logic as ordinary testable C++.

```text
TrackerPre/
|
+-- Driver/
|   +-- TrackerPreAudioDriver.*
|   +-- TrackerPreAudioDevice.*
|   +-- TrackerPreInputStream.*
|   +-- TrackerPreOutputStream.*
|   +-- DriverKitUsbTransport.*
|   +-- DriverKitUserClient.*
|
+-- Core/
|   +-- DeviceProfile.*
|   +-- DescriptorModel.*
|   +-- EmuExtensionUnits.*
|   +-- ClockProtocol.*
|   +-- DuplexIsochEngine.*
|   +-- FeedbackQueue.*
|   +-- PacketPlanner.*
|   +-- HardwareClockEstimator.*
|   +-- AudioRingBuffer.*
|   +-- DeviceStateMachine.*
|
+-- Tests/
|   +-- DescriptorTests.*
|   +-- FeedbackTests.*
|   +-- PacketPlannerTests.*
|   +-- ClockEstimatorTests.*
|   +-- StateMachineTests.*
|
+-- ControlApp/
    +-- Swift / SwiftUI
```

`Core/` should avoid DriverKit types whenever practical.

For example, this is preferable:

```cpp
struct StreamFormat {
    uint32_t channels;
    uint32_t bytesPerSample;
    uint32_t bytesPerFrame;
    uint32_t sampleRate;
};

uint32_t outputPacketBytes(
    uint32_t sampleFrames,
    const StreamFormat& output)
{
    return sampleFrames * output.bytesPerFrame;
}
```

rather than embedding `IOUSBHost*`, `IOUserAudio*` or `OSObject*` types into protocol logic.

---

## 6. C++ real-time coding rules

The C++ implementation should voluntarily adopt many of the restrictions that Rust would otherwise help enforce.

### Hot path rules

Inside USB isoch completion handlers and AudioDriverKit real-time callbacks:

- no heap allocation;
- no exceptions;
- no blocking mutexes;
- no filesystem access;
- no logging that can synchronously block;
- no Objective-C/Swift calls;
- no operations with unbounded execution time;
- no object graph mutation that could trigger complicated lifetime behavior.

Prefer:

- fixed-size rings allocated during device configuration;
- bounded single-producer/single-consumer queues;
- atomics for counters and state publication;
- trivially copyable data on the hot path;
- explicit ownership and teardown rules.

### Recommended type discipline

Do not represent frames, bytes and USB intervals as interchangeable integers.

Even in C++, use lightweight wrappers when possible:

```cpp
struct SampleFrames { uint32_t value; };
struct ByteCount    { uint32_t value; };
struct UsbFrame     { uint64_t value; };
```

This directly guards against the historical bytes-vs-sample-frames ambiguity found while comparing E-MU and Wouter.

---

## 7. Advantages of Approach A

- Lowest DriverKit integration risk.
- Direct use of Apple's intended APIs.
- Easiest initial Xcode signing/deployment workflow.
- Easy comparison against E-MU and Wouter source.
- No C ABI bridge in the real-time path.
- No custom Rust target/toolchain work.
- Easier debugging when stepping from DriverKit callback to transport code.

## 8. Disadvantages of Approach A

- More lifetime and memory-safety burden on the implementation.
- State-machine invariants are easier to violate accidentally.
- Descriptor parsing operates on externally supplied byte streams and therefore benefits strongly from safer bounds-checked modeling.
- Concurrency mistakes require coding discipline rather than language enforcement.
- Reusing the protocol engine outside the Apple driver may accumulate Apple-specific dependencies unless boundaries are maintained aggressively.

---

# Part II — Approach B: Rust core + thin C++ DriverKit adapter

## 9. When to choose the hybrid architecture

Choose the Rust-core architecture when the objective is broader:

> Build a clean and reusable CA0189 implementation with strong type/lifetime/state guarantees, while accepting some build-system and FFI complexity at the Apple boundary.

This becomes especially attractive if the project may later cover:

- E-MU 0202;
- E-MU 0204;
- E-MU 0404;
- diagnostic CLI tools;
- protocol simulators;
- Linux or other experimental backends;
- additional old-device revival projects using the same architectural pattern.

The proposed split is:

```text
                         Core Audio
                            |
                            v
                  C++ DriverKit shell
                +----------------------+ 
                | IOUserAudioDriver    |
                | IOUserAudioDevice    |
                | USBDriverKit objects |
                | IIG/user client      |
                +----------+-----------+
                           |
                         C ABI
                           |
                +----------v-----------+
                |       Rust core      |
                |                      |
                | descriptor parser    |
                | CA0189 model         |
                | state machine        |
                | packet planner       |
                | feedback queue       |
                | clock estimator      |
                | ring bookkeeping     |
                | diagnostics model    |
                +----------------------+
```

The architectural rule should be:

> Rust does not directly subclass or own DriverKit objects in the first implementation.

That boundary deliberately contains Apple's C++ ABI and keeps the Rust side ordinary and testable.

---

## 10. Why Rust is attractive for the CA0189 core

### 10.1 Strong semantic types

The feedback mechanism should be impossible to confuse with raw packet bytes.

```rust
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SampleFrames(pub u32);

#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ByteCount(pub u32);

#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UsbFrameNumber(pub u64);
```

Then packet sizing becomes explicit:

```rust
pub fn output_packet_size(
    frames: SampleFrames,
    format: StreamFormat,
) -> ByteCount {
    ByteCount(frames.0 * format.bytes_per_frame as u32)
}
```

This captures the correct E-MU abstraction directly in the type system.

### 10.2 Descriptor parsing

USB descriptors are packed externally supplied bytes.

Rust is well suited to converting:

```text
raw USB bytes
      |
      v
bounds validation
      |
      v
validated descriptor
      |
      v
typed device model
```

Avoid transmuting arbitrary packed structures from an unvalidated pointer. Parse explicitly from byte slices and reject malformed or inconsistent descriptors.

### 10.3 State-machine modeling

Instead of historical-style combinations of booleans:

```text
running
starting
stopping
terminating
needsReset
...
```

represent lifecycle explicitly:

```rust
pub enum DeviceState {
    Detached,
    Probing(ProbingState),
    Idle(IdleState),
    Configuring(ConfiguringState),
    Priming(PrimingState),
    Running(RunningState),
    Recovering(RecoveryState),
    Failed(DriverError),
}
```

Transitions should be expressed through narrow methods rather than arbitrary mutation.

This is valuable for exactly the failure modes expected in an audio USB driver:

- unplug while transfers are active;
- sample-rate change during streaming;
- delayed completions from a previous configuration generation;
- stream teardown while callbacks remain outstanding;
- error recovery and restart;
- Core Audio opening only one logical direction while the USB engine needs both.

### 10.4 Clock estimation and packet planning

The clock estimator is a particularly good Rust module because it requires no Apple API.

```rust
pub struct ClockEstimator {
    accumulated_frames: u64,
    accumulated_intervals: u64,
    filtered_rate_hz: f64,
    phase_error_frames: f64,
}
```

It can be unit-tested against recorded packet traces without installing a driver.

### 10.5 Portable testability

The same `emu-ca0189` crate could build on an ordinary Mac/Linux host for tests even if the dext uses a restricted freestanding configuration.

Example test data:

```text
tests/data/
    tracker_44100_10min.trace
    tracker_48000_10min.trace
    tracker_96000_10min.trace
    tracker_rate_switch.trace
    tracker_disconnect.trace
```

This allows deterministic regression testing with `cargo test` outside DriverKit.

---

## 11. Recommended Rust restrictions inside the dext

Initially assume the embedded driver core is freestanding:

```rust
#![no_std]
```

and configure panic behavior as abort rather than unwinding.

### Use freely

- `core`;
- fixed arrays;
- slices;
- enums;
- `Option` / `Result`;
- atomics;
- const generics;
- explicitly provided memory regions;
- custom fixed-capacity queues and rings;
- plain numerical algorithms.

### Avoid initially

- `std`;
- `Vec` in real-time paths;
- `String` in real-time paths;
- implicit heap allocation;
- Rust threads;
- async runtimes;
- filesystem/network APIs;
- panic unwinding;
- allocator-dependent collections until DriverKit compatibility is intentionally designed and tested.

This is not merely a workaround for DriverKit. It is a good real-time audio discipline.

Later, if a small allocator-backed control-plane subset is demonstrably safe and supported, it can be introduced outside the isochronous/audio hot path.

---

## 12. Keep the C++ adapter intentionally boring

The C++ layer should own:

- DriverKit subclassing;
- DriverKit object lifetimes;
- USBDriverKit calls;
- AudioDriverKit callbacks;
- creation and mapping of shared buffers;
- IIG/user-client plumbing;
- translation of Rust commands into DriverKit operations.

It should **not** contain another independent copy of the device state machine.

Conceptually:

```cpp
IOReturn TrackerPreDriver::Start(IOService* provider)
{
    // DriverKit setup and USB discovery.
    // Allocate all required fixed memory.

    core_ = emu_core_create(&configuration_);
    if (!core_) {
        return kIOReturnNoMemory;
    }

    return kIOReturnSuccess;
}
```

Completion path:

```cpp
void TrackerPreDriver::InputComplete(
    uint32_t actualBytes,
    uint64_t usbFrame)
{
    EmuAction action = emu_core_input_complete(
        core_, actualBytes, usbFrame);

    apply(action);
}
```

The Rust core returns decisions rather than reaching into DriverKit:

```rust
pub enum DriverAction {
    None,
    SubmitInput {
        requested_bytes: ByteCount,
    },
    SubmitOutput {
        requested_bytes: ByteCount,
    },
    SetAlternateInterface {
        interface: u8,
        alternate: u8,
    },
    SetClock {
        rate: SampleRate,
    },
    BeginRecovery,
}
```

This maintains a clear line:

```text
Apple/DriverKit-specific and unsafe world
                 C++
                  |
------------------+------------------
                  |
portable deterministic device logic
                 Rust
```

---

## 13. FFI rules

Use a **small C ABI**, not C++ ABI calls directly from Rust.

### C-visible interface example

```c
#ifdef __cplusplus
extern "C" {
#endif

typedef struct EmuCore EmuCore;

typedef struct {
    uint32_t kind;
    uint32_t value0;
    uint32_t value1;
    uint64_t value2;
} EmuAction;

EmuCore* emu_core_create(const EmuCoreConfig* config);
void emu_core_destroy(EmuCore* core);

EmuAction emu_core_input_complete(
    EmuCore* core,
    uint32_t actual_bytes,
    uint64_t usb_frame);

EmuAction emu_core_output_complete(
    EmuCore* core,
    uint32_t actual_bytes,
    uint64_t usb_frame);

#ifdef __cplusplus
}
#endif
```

Rust:

```rust
#[no_mangle]
pub extern "C" fn emu_core_input_complete(
    core: *mut EmuCore,
    actual_bytes: u32,
    usb_frame: u64,
) -> EmuAction {
    // Validate raw pointer once at the FFI edge.
    // Everything beyond this point should be safe Rust where possible.
}
```

### ABI guidelines

- Use `#[repr(C)]` or fixed-width primitive fields on all shared structs.
- Do not pass Rust enums directly across FFI unless represented explicitly.
- Do not expose Rust references or slices directly to C++.
- Do not allow exceptions or Rust panics across the boundary.
- Treat every pointer received from C++ as unsafe and validate once at the boundary.
- Keep ownership unambiguous: either C++ owns a resource or Rust owns it.
- Version the ABI if the control plane becomes public.

---

## 14. Rust build risk: the real unresolved question

FFI itself is not the major risk.

The major risk is **target/platform compatibility**.

As of this document's date, official Rust platform support includes normal Apple Silicon macOS (`aarch64-apple-darwin`) and many Apple targets, but it does not list a standard `aarch64-apple-driverkit` target.

DriverKit binaries are not ordinary macOS executables. They are linked against the DriverKit SDK/platform and carry DriverKit platform metadata.

Therefore the project must prove one of these paths:

### Path B1 — simplest experiment

Compile an extremely small `#![no_std]` Rust static library using the normal Apple Silicon Darwin target and attempt to link its object code into a DriverKit dext.

This might be accepted because a static archive is ultimately relocatable object code, but this must be treated strictly as an **experiment**, not an assumption.

If Xcode/`ld` rejects the archive due to platform metadata or ABI/platform mismatch, stop using this path.

### Path B2 — proper custom DriverKit Rust target

Create a minimal custom Rust target specification matching Apple's arm64 DriverKit LLVM target/platform and compile only `core` plus project code.

Likely requirements include:

- nightly/custom-target support as needed;
- DriverKit SDK selected through Xcode/xcrun;
- Apple arm64 ABI;
- freestanding/no `std`;
- panic abort;
- static library output;
- no Rust entry point;
- Xcode performs final dext link/sign/package.

The exact target JSON and linker flags should **not** be frozen in the main project until verified against the installed Xcode/SDK. Treat them as build-generated/toolchain-specific configuration.

### Path B3 — reject Rust in the dext

If the custom target requires fragile patches, unsupported compiler forks or platform hacks that cannot be reproduced cleanly, do not force the issue.

Keep Rust for:

- trace-analysis tools;
- descriptor/protocol experiments;
- offline tests;
- packet/clock-model prototypes;

and implement the production dext core in C++.

A preservation driver should not itself become dependent on an exotic unmaintainable toolchain.

---

# Part III — Swift / SwiftUI layer

## 15. Swift is recommended for the management application

Swift is a good choice for the **host/control/diagnostic application**, regardless of whether the dext core is C++ or Rust.

```text
EMUControl.app                 Swift / SwiftUI
      |
      | DriverKit user client / supported IPC
      v
TrackerPreDriver.dext          C++ boundary
      |
      +--> C++ core       [Approach A]
      |
      +--> Rust core      [Approach B]
```

The application can expose:

- connected device identity;
- firmware/device revision;
- active sample rate;
- selected USB alternate settings;
- input/output packet counts;
- USB errors;
- underruns/overruns;
- queue occupancy;
- estimated physical sample rate;
- clock drift in ppm;
- input/output latency estimates;
- stream generation/state;
- recovery events;
- diagnostic trace capture.

Example conceptual UI:

```text
E-MU Tracker Pre

Status
--------------------------------
Connected            Yes
USB                  High Speed
Clock                Internal
Sample rate          96.0 kHz

Streaming
--------------------------------
USB input            Active
Core Audio input     Closed
USB output           Active
Input packets        12,829,184
Output packets       12,829,191
USB errors           0
Underruns            0
Overruns             0

Clock
--------------------------------
Estimated rate       95,999.83 Hz
Deviation            -1.8 ppm
Feedback depth       67 %
```

The UI must not sit in the audio/USB timing path.

Driver statistics should be copied/snapshotted asynchronously through a deliberately narrow user-client interface.

---

# Part IV — Real-time and reliability guidelines

## 16. Hard real-time-path requirements

These rules apply to both C++ and Rust implementations.

### Do

- preallocate USB transfer descriptors;
- preallocate audio rings;
- use fixed-capacity feedback queues;
- use bounded algorithms;
- separate control-plane and streaming-plane state;
- identify transfer/configuration generations so stale completions can be discarded safely;
- maintain monotonically increasing counters for diagnostics;
- fail deterministically when invariants are broken;
- keep packet planning mathematically expressed in sample frames.

### Do not

- allocate memory in the completion path;
- sleep in real-time callbacks;
- perform synchronous UI/log/file I/O;
- hold locks across DriverKit API calls unless proven necessary and bounded;
- trust callback ordering beyond what the API guarantees;
- reuse a completion from a prior stream generation after a rate change/restart;
- silently hide protocol failures.

---

## 17. Clock/feedback engine guidance

The historical E-MU behavior gives the immediate packet planner:

```text
actual input packet bytes
        |
        / inputBytesPerFrame
        v
sample frames for this service interval
        |
        v
feedback queue
        |
        x outputBytesPerFrame
        v
next output packet byte request
```

The modern implementation should preserve this exact immediate-feedback behavior while also maintaining a longer-term estimator:

```text
input packet frame counts
        |
        +------------------> immediate output planner
        |
        +------------------> hardware clock estimator
                                  |
                                  +-- measured Hz
                                  +-- ppm drift
                                  +-- phase/timeline data
                                  +-- diagnostics
```

The estimator should **not** replace the known-working packet feedback algorithm until experimental evidence justifies doing so.

It is initially an additional observation/timestamp/recovery tool.

---

## 18. Error handling guidance

Every control operation that can be verified should be verified.

Sample-rate change:

```text
stop/quiet streams as required
       |
SET_CUR requested E-MU rate code
       |
check USB result
       |
GET_CUR rate code
       |
compare expected vs actual
       |
configure matching stream alternatives
       |
prime/restart
```

Never reproduce the original E-MU behavior in which a failed `SET_CUR` can be converted into apparent success.

Recoverable USB errors should transition through an explicit recovery state. Repeated failure should stop the stream and surface a diagnostic rather than loop invisibly forever.

---

# Part V — Minimal Rust-in-DriverKit feasibility project

## 19. Purpose

Before choosing Approach B, build a deliberately tiny project that proves only the language/toolchain boundary.

It should **not** contain Tracker Pre USB code or AudioDriverKit streaming.

The success criterion is simply:

> A signed Apple Silicon DriverKit dext loads, calls a Rust function linked into the dext, receives the correct deterministic result, and remains stable through repeated load/start/stop cycles.

Do this before writing the real driver in Rust.

---

## 20. Suggested project layout

```text
DriverKitRustProbe/
|
+-- ProbeHost.app/
|   +-- minimal Swift/SwiftUI host
|
+-- ProbeDriver.dext/
|   +-- ProbeDriver.iig        (if needed)
|   +-- ProbeDriver.hpp
|   +-- ProbeDriver.cpp
|   +-- RustBridge.h
|
+-- rust-core/
|   +-- Cargo.toml
|   +-- src/
|       +-- lib.rs
|
+-- scripts/
    +-- build-rust.sh
```

The project should remain intentionally disposable.

---

## 21. Minimal Rust library

`Cargo.toml` concept:

```toml
[package]
name = "driverkit-rust-probe"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[profile.release]
panic = "abort"
lto = false
codegen-units = 1
```

`src/lib.rs`:

```rust
#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // Probe-only policy. Production code should make panic paths impossible
    // in the streaming engine and choose an explicitly documented fatal policy.
    loop {
        core::hint::spin_loop();
    }
}

#[no_mangle]
pub extern "C" fn emu_rust_probe_add(a: u32, b: u32) -> u32 {
    a.wrapping_add(b).wrapping_add(0x454d_5500)
}
```

No allocator.
No `std`.
No panic path.
No thread-local storage.
No dependencies.
No USB.
No Core Audio.

The intentionally recognizable constant makes it obvious that the returned value really came through the Rust object code.

---

## 22. Minimal C bridge

`RustBridge.h`:

```c
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t emu_rust_probe_add(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif
```

In the dext start/probe path:

```cpp
uint32_t result = emu_rust_probe_add(7, 11);

constexpr uint32_t expected =
    static_cast<uint32_t>(7 + 11 + 0x454d5500u);

if (result != expected) {
    return kIOReturnError;
}
```

Expose the result through the most trivial supported diagnostic/user-client path or record it in a way that can be verified while debugging.

The point is to prove:

```text
DriverKit C++ -> C ABI -> Rust -> C ABI -> DriverKit C++
```

inside the real dext process.

---

## 23. Test sequence

### Test 0 — ordinary Rust host build

First verify the Rust function with normal host unit tests.

This does **not** prove DriverKit compatibility; it only verifies source/tooling.

### Test 1 — static archive inspection

Build `libdriverkit_rust_probe.a` and inspect:

- architecture is arm64;
- exported symbol exists;
- no unexpected dynamic dependencies;
- no required Rust runtime/`std` libraries appear;
- no unwinding symbols are unexpectedly required.

Useful inspection commands on the development Mac include:

```sh
nm -gU path/to/libdriverkit_rust_probe.a | grep emu_rust_probe_add
lipo -info path/to/libdriverkit_rust_probe.a
otool -l path/to/relevant-object.o
vtool -show-build path/to/relevant-object.o
xcrun --sdk driverkit --show-sdk-path
```

The exact output varies by Xcode/Rust version. Capture it in the repository under a `toolchain-notes/` directory so later toolchain upgrades can be compared rather than guessed about.

### Test 2 — link into empty/minimal DriverKit dext

Add the archive to the Xcode DriverKit extension target.

This is the first decisive result.

If the link fails due to platform metadata, do **not** paper over the error with arbitrary linker suppression flags. Move to the custom-target experiment.

### Test 3 — install/load/start

With normal DriverKit development signing/entitlements in place:

- install/activate the dext;
- start it;
- execute `emu_rust_probe_add()` from dext code;
- verify the returned constant;
- stop/unload/deactivate according to the supported development workflow.

### Test 4 — repetition

Repeat load/start/stop at least dozens of times.

No leak, crash, unresolved symbol, constructor/runtime issue or shutdown fault is acceptable.

### Test 5 — tiny mutable Rust state

Only after the pure function passes, test an opaque Rust object:

```c
typedef struct ProbeCore ProbeCore;

ProbeCore* probe_core_create(uint32_t initial);
uint32_t probe_core_increment(ProbeCore*);
void probe_core_destroy(ProbeCore*);
```

Use a caller-provided memory region if avoiding allocation is desired.

The point is to verify lifecycle and FFI ownership.

### Test 6 — atomics

Add a fixed `AtomicU64` counter and exercise it from repeated C++ callbacks.

This proves the subset of Rust/core facilities required by the future statistics and packet engine.

### Test 7 — only then touch USBDriverKit

Once the boundary is proven, the next experiment can become Tracker-specific:

```text
match 041e:3f0a
      |
read descriptor tree in C++/USBDriverKit
      |
pass immutable descriptor byte span to Rust
      |
Rust parses device model
      |
return compact validated model to C++
```

This is the first meaningful use of Rust for the real project and also tests the part of the design where Rust provides immediate safety benefits.

---

## 24. Rust feasibility pass/fail criteria

### PASS

Adopt Rust core architecture if all of the following are true:

- reproducible build from a clean checkout;
- no patched Rust compiler required;
- no private Apple framework dependency;
- static library links cleanly into the DriverKit target;
- no unexpected Rust runtime dependency;
- dext signs, installs and starts normally;
- C++ <-> Rust calls work reliably;
- teardown is clean;
- build remains understandable enough to document in a few scripts/configuration files;
- supported Rust subset is sufficient for fixed-memory state machine, descriptor parser, rings and clock estimator.

### FAIL / prefer C++ production core

Reject Rust inside the production dext if any of these become necessary:

- maintaining a private rustc fork;
- patching LLVM;
- suppressing platform-linker checks without understanding them;
- depending on unstable ABI tricks;
- relying on normal macOS runtime libraries unavailable to DriverKit;
- significant inability to debug crashes across the language boundary;
- fragile build steps that are tightly coupled to one Xcode minor release;
- inability to reproduce release builds on another developer Mac.

If the experiment fails, nothing is lost. The protocol and algorithm design remains directly applicable to C++, and Rust can still be used for offline tooling and tests.

---

# Part VI — Recommended decision sequence

## 25. Do not decide C++ vs Rust philosophically

Use an empirical gate.

```text
                 START
                   |
                   v
       minimal DriverKit C++ dext
                   |
                   v
       link no_std Rust probe
                   |
          +--------+--------+
          |                 |
       clean PASS          FAIL
          |                 |
          v                 v
 Rust core viable      production C++
          |                 |
          +--------+--------+
                   |
                   v
      Tracker descriptor probe
                   |
                   v
     capture isoch experiment
                   |
                   v
        real driver engine
```

This makes language choice a measured engineering decision rather than an ideological one.

---

## 26. Recommended choice today

### For shortest time to first audio

Use:

```text
C++ DriverKit driver
+ testable ordinary C++ CA0189 core
+ Swift/SwiftUI control app
```

This has the lowest external risk.

### For the preferred long-term preservation architecture

If the minimal probe succeeds cleanly, prefer:

```text
C++ DriverKit adapter
+ no_std Rust CA0189/stream/clock core
+ Swift/SwiftUI control app
```

This provides the best separation between:

- Apple-specific platform plumbing;
- unsafe external interfaces;
- reusable deterministic protocol logic;
- user-facing management/UI.

The proposed long-term architecture is therefore:

```text
                 SwiftUI control app
                         |
                  diagnostics / IPC
                         |
              +----------v-----------+
              | C++ DriverKit adapter|
              |                      |
              | AudioDriverKit       |
              | USBDriverKit         |
              | IIG / user client    |
              +----------+-----------+
                         |
                       C ABI
                         |
              +----------v-----------+
              |      emu-ca0189      |
              |        Rust          |
              |                      |
              | descriptor model     |
              | XU protocol model    |
              | state machine        |
              | feedback queue       |
              | packet planner       |
              | hardware clock       |
              | ring bookkeeping     |
              | diagnostic counters  |
              +----------------------+
```

---

# Part VII — Suggested repository organization if Rust passes

## 27. Proposed tree

```text
emu-tracker-pre/
|
+-- README.md
+-- docs/
|   +-- protocol-spec.md
|   +-- development-guidelines.md
|   +-- reverse-engineering/
|   +-- hardware-captures/
|
+-- apple/
|   +-- TrackerPre.xcodeproj
|   +-- Driver/
|   |   +-- TrackerPreDriver.cpp
|   |   +-- TrackerPreAudioDevice.cpp
|   |   +-- DriverKitUsbTransport.cpp
|   |   +-- RustBridge.h
|   +-- ControlApp/
|       +-- Swift / SwiftUI
|
+-- rust/
|   +-- emu-ca0189/
|   |   +-- Cargo.toml
|   |   +-- src/
|   |   +-- tests/
|   +-- emu-trace/
|       +-- optional host-side CLI/tooling
|
+-- captures/
|   +-- descriptors/
|   +-- packet-traces/
|   +-- latency/
|
+-- scripts/
    +-- build-rust-driverkit.sh
    +-- analyze-binary.sh
```

Do not place historical copyrighted binaries into a public repository unless distribution rights have been checked separately. Keep hashes and provenance metadata in documentation even when an artifact itself cannot be redistributed.

---

## 28. Core crate/module decomposition

If Rust is adopted:

```text
emu-ca0189
|
+-- types
|   +-- SampleFrames
|   +-- ByteCount
|   +-- UsbFrameNumber
|   +-- SampleRate
|
+-- descriptor
|   +-- parser
|   +-- validated topology
|   +-- extension units
|
+-- protocol
|   +-- clock rate codes
|   +-- request/response validation
|
+-- streaming
|   +-- feedback queue
|   +-- packet planner
|   +-- ring accounting
|
+-- clock
|   +-- immediate cadence
|   +-- long-term estimator
|   +-- timeline
|
+-- state
|   +-- device lifecycle
|   +-- stream generation
|   +-- recovery
|
+-- diagnostics
    +-- counters
    +-- snapshots
```

The crate should not know that the transport is DriverKit.

At most it should consume abstract facts such as:

```text
input packet completed
USB frame N
actual byte count X
control transfer result
alternate setting applied
```

and emit decisions/facts such as:

```text
request output packet of Y bytes
switch to alternate setting Z
clock change verified
stream generation invalid
enter recovery
```

---

# Part VIII — Development priorities

## 29. Recommended immediate milestones

### Milestone 0 — choose build architecture empirically

Build the minimal Rust-in-DriverKit probe described above.

Result:

- `PASS`: use Rust core;
- `FAIL`: use C++ core without further delay.

### Milestone 1 — Tracker Pre descriptor probe

On the real Apple Silicon Mac:

- match Creative VID `0x041e` / Tracker Pre PID `0x3f0a`;
- inspect complete descriptor topology;
- identify audio-control and streaming interfaces;
- identify E-MU extension units by extension code;
- record every alternate setting and endpoint;
- save raw and interpreted descriptor dumps as test fixtures.

### Milestone 2 — control-only clock experiment

Implement:

```text
GET_CUR current rate
SET_CUR requested rate
GET_CUR verification
```

No audio stream yet.

### Milestone 3 — input isochronous capture probe

At 44.1, 48, 88.2, 96, 176.4 and 192 kHz where supported:

record:

```text
USB service/frame number
requested bytes
actual bytes
sample frames
completion timing
error/status
```

Use these captures to validate the feedback and clock-estimator model.

### Milestone 4 — internally duplex USB engine

Run capture and playback together even when only playback is exposed to Core Audio, so hardware-clock feedback is available.

### Milestone 5 — AudioDriverKit streams

Only after USB transport is understood and independently testable, expose normal Core Audio input/output.

### Milestone 6 — diagnostics app

Add SwiftUI UI after the driver can already report structured diagnostics through a stable narrow interface.

---

## 30. Final guidance

There is no strong reason to force the entire project into one language.

The most appropriate language follows the boundary:

| Layer | Conservative choice | Hybrid preferred choice |
|---|---|---|
| AudioDriverKit classes | C++ | C++ |
| USBDriverKit calls | C++ | C++ |
| DriverKit/IIG glue | C++ | C++ |
| CA0189 protocol | C++ | **Rust** |
| descriptor parser | C++ | **Rust** |
| state machine | C++ | **Rust** |
| feedback/packet planner | C++ | **Rust** |
| clock estimator | C++ | **Rust** |
| fixed ring bookkeeping | C++ | **Rust** |
| control/diagnostic app | **Swift/SwiftUI** | **Swift/SwiftUI** |
| offline trace analysis | C++/Python/Rust | **Rust/Python** |

The engineering recommendation is therefore:

> **Use C++ at Apple's DriverKit boundary. Attempt Rust for the deterministic CA0189 core only after a minimal no_std static-library probe proves that the DriverKit build, link, runtime and teardown path is clean and reproducible. If it is not, use ordinary testable C++ for the core without compromising the architecture. Use Swift/SwiftUI for the management application in either case.**

This gives the project both a conservative path to first audio and a cleaner long-term architecture if Rust proves practical.
