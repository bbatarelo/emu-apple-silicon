# Architecture

```
   application
        |
   Core Audio HAL              Float32, fixed buffers, host clock
        |
   driver/plugin.c             AudioServerPlugIn: device, streams, controls
        |
   driver/ring.h               lock-free SPSC rings, both directions
        |
   driver/usb_engine.c         its own thread and run loop
        |
   shared/usb_util.c           IOKit interface and pipe helpers
   rust/emu-ca0189             descriptors, protocol, feedback, clock
        |
   low-latency isochronous
        |
   E-MU Tracker Pre
```

One process. The plug-in loads into `com.apple.audio.Core-Audio-Driver-Service.helper`
and opens USB directly, so there is no helper daemon and no IPC.

## Layout

| | |
|---|---|
| `driver/` | The HAL plug-in. This is the product. |
| `shared/` | Device identity and IOKit helpers, used by the driver and the tools |
| `rust/emu-ca0189/` | Descriptor parser, protocol model, feedback queue, clock estimator |
| `tools/` | `emu-probe` (USB diagnostics), `hal-check` (device inspector), `hal-record` (capture verifier) |
| `captures/` | Descriptor and packet-trace fixtures, replayed by the tests |
| `driverkit/` | An unfinished DriverKit version, parked |

## The Rust core

`rust/emu-ca0189` holds everything that is about the *device* rather than about
macOS: parsing descriptors, the CA0189 request encoding, rate codes, the feedback
queue and the clock estimator.

It knows nothing about IOKit, Core Audio or DriverKit. That is deliberate — it
compiles `no_std` for a DriverKit dext and with `std` for userspace, and it is
tested against captured traces with no hardware attached.

```bash
make test        # 35 tests, replaying real captures
```

Its C interface is `rust/emu-ca0189/include/emu_ca0189.h`. The boundary is a
small C ABI: `#[repr(C)]` on everything shared, raw pointers validated once at
the edge, no panics crossing over.

## The two clocks

This is the heart of it.

Core Audio delivers fixed 512-frame buffers paced by the host clock. USB moves
variable packets — 44 or 45 frames at 44.1 kHz — paced by the *device's* clock.
The two never agree exactly.

The rings absorb the difference; the timeline removes it.

**The rings** (`driver/ring.h`) are single-producer single-consumer, lock-free,
and count underruns and overruns rather than hiding them. Format conversion
happens during the copy, since that side already touches every sample.

Only the consumer may move the read index. Violating that produced audio that
looked like full-scale noise — see FINDINGS.

**The timeline** (`GetZeroTimeStamp`) anchors to frames the device has actually
consumed, not to `mach_absolute_time`. The engine publishes a matched pair —
frames consumed, and the host time that count was true — through a seqlock. Two
plain atomics would let a frame count from one completion pair with a timestamp
from the next, which is worse than no anchor at all.

## Packet sizing

Playback packet sizes come from the capture stream. Each capture service interval
reports how many sample frames the device produced; that count sizes the next
playback packet, through the feedback queue in the Rust core.

This is why **capture runs even when only playback is wanted** — it is how the
driver measures what the hardware is doing. It is also what E-MU's original
driver did.

There is an explicit feedback endpoint (`0x81`) on the playback interface which
this driver does not currently use.

## The USB engine

`driver/usb_engine.c` runs on its own thread with its own run loop. `StartIO`
creates it, `StopIO` joins it — outside the state lock, since holding a lock
across a thread join invites deadlock.

It uses **low-latency isochronous transfers**. The classic API delivers one
frame-list entry per USB frame, which silently halves the audio on the
`bInterval 3` endpoints used at 176.4 and 192 kHz. Buffers come from
`LowLatencyCreateBuffer`; ordinary allocations are rejected.

Startup order matters and is not negotiable: parse descriptors, set the clock
rate, **verify it by read-back**, then select alternate settings. Doing it in any
other order can wedge the device until it is physically replugged.

## Diagnostics

The plug-in runs inside a system daemon that exits when idle, so there is no
process to attach to, and log queries proved unreliable while developing it.

Instead the driver publishes counters through a declared custom property, and
`hal-check` enumerates whatever it finds rather than a list kept client-side.
Frames played and captured, ring depths, underruns, overruns, USB errors,
feedback starvation.

This is not scaffolding. Every misalignment bug in this project produced correct
byte counts and zero errors; the counters are what made it possible to say a path
was *clean* rather than merely plausible — and what made clear when the answer
had to come from listening instead.
