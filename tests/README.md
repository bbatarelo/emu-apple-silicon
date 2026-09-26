# Testing the E-MU driver

Run commands from the repository root on macOS with the same Xcode command-line
tools and Rust toolchain used to build the driver. Make builds each test's
prerequisites automatically; you do not need to compile individual C files.
Tests return a nonzero exit status on failure or unmet prerequisites.

## Before submitting a change: no hardware required

```sh
make test
```

This never installs a driver, opens USB hardware, or starts audio. It runs:

| Command | Coverage | Source |
| --- | --- | --- |
| `make test-rust` | Protocol, descriptors, MIDI packet handling and clock logic | `rust/emu-ca0189/` |
| `make test-analysis` | Synthetic signals exercising the loopback analyser | `tools/hal-loopback/` (`selftest`) |
| `make test-regression` | Production USB callbacks and startup retries with a fake USB interface | `tests/regression/` |
| `make test-usb-abort` | One regression: request ownership during cancellation, delayed completions, retries, and partial cleanup | `tests/regression/usb-abort-test.c` |

The USB regression reproduces [issue #7](https://github.com/bbatarelo/emu-apple-silicon/issues/7)
without relying on YouTube or timing luck. It includes the production engine
implementation so that it tests the real static callbacks, rather than a copy of
their logic. See the [investigation](../docs/issue-investigations/ISSUE-7-INVESTIGATION.md)
for the failure and fix. Passing this fake-interface test does not establish how
the macOS USB stack behaves during unplug or sleep.

## After installing a change: hardware integration

These commands test the **installed** driver, not the driver bundle in `build/`.
Build and install the intended revision first (`make all`, then `make install`).
Check its version with `build/bin/hal-check`. Stop other playback and recording
on the selected unit before running:

```sh
EMU_DEVICE=3F04 make test-integration   # 0404 USB
EMU_DEVICE=3F0A make test-integration   # Tracker Pre
```

Use a unique UID or serial substring instead when multiple units have the same
product ID. Without `EMU_DEVICE`, the first matching E-MU is selected.

`test-integration` runs the following two tests **sequentially**, also under
`make -j`. It takes roughly a minute per unit. No loopback cables are needed.

| Command | What it does |
| --- | --- |
| `make test-hal-restart` | Starts/stops silent output 20 times; checks that Core Audio frames bind to USB requests |
| `make test-hal-startup` | Repeats 20 starts, injecting a one-shot stale-schedule fault after a partial submission each time; checks recovery and fault consumption |

Both use `tests/integration/hal-restart-test.c`. They preserve the default output,
sample rate and volume, but deliberately start streams and, in the second test,
exercise failure recovery. They measure binding and counters, **not audible
quality**. An older installed driver may reject the startup fault command.

If a test fails, retain its output, `hal-check` diagnostics and the installed
version. Stop competing audio clients before retrying. To disarm a pending
startup fault: `EMU_DEVICE=3F04 build/bin/hal-check fault none`.

## Tests with additional setup

These are explicit commands, excluded from `test-integration` because they need
different starting conditions. There is no unattended command for every physical
and listening check.

- **Recovery:** play audio to the selected unit, then run
  `EMU_DEVICE=3F04 make test-recovery`. The existing
  `scripts/test-recovery.sh` injects transient and persistent transport failures,
  checks recovery/dead-device reporting and clears the fault. Playback is
  interrupted; restart it afterward and confirm sound returns.
- **Analog loopback:** connect and set levels as described in the
  [README diagnostics](../README.md#optional-diagnostics-and-troubleshooting),
  then use `make loopback` or the documented `hal-loopback` commands. Unlike the
  silent tests, this sends tones and assesses recorded audio.
- **Manual lifecycle/soak:** concurrent devices, seeking, rate changes,
  unplug/replug and sleep/wake need their own hardware trial. Record the units,
  rate, input mode, driver version and symptoms.

## Layout and adding tests

- `regression/`: deterministic C regressions with no hardware dependency.
- `integration/`: executables using an installed driver and real devices.
- Rust tests stay beside their crate, and analyser self-tests stay with the
  analyser. Existing recovery scripts remain in `scripts/`; this guide indexes
  them rather than duplicating them.

Name new tests for the behaviour they protect, not just an issue number. Link
an issue and its investigation in the source or this guide. Register a named Make
target with all source/header dependencies and automatic build prerequisites.
Add hardware-free regressions to `test-regression` (and thus `make test`). Add
hardware tests to `test-integration` only if they fit its idle-device, silent,
no-cable setup; keep them sequential. Otherwise provide a separate target and
document setup, side effects, expected results and cleanup here.

`make test-build` builds the C test executables without running anything. Test
binaries live in `build/bin/`, which `make clean` removes. The existing direct
binary commands and individual Make targets remain available for debugging.
