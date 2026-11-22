# Testing the E-MU driver

Run commands from the repository root on macOS with the same Xcode command-line
tools and Rust toolchain used to build the driver. Make builds each test's
prerequisites automatically; you do not need to compile individual C files.
Tests return a nonzero exit status on failure or unmet prerequisites.

## Before submitting a change: no hardware required

```sh
make test
```

This never installs a driver, opens USB hardware, or starts audio. `make
test-split` does start a process: a test engine, as a launchd service under a
throwaway name in your own user domain, removed again when the test ends. It
runs:

| Command | Coverage | Source |
| --- | --- | --- |
| `make test-rust` | Protocol, descriptors, MIDI packet handling and clock logic | `rust/emu-ca0189/` |
| `make test-analysis` | Synthetic signals exercising the loopback analyser | `tools/hal-loopback/` (`selftest`) |
| `make test-regression` | Production USB callbacks and startup retries with a fake USB interface | `tests/regression/` |
| `make test-usb-abort` | One regression: request ownership during cancellation, delayed completions, retries, and partial cleanup | `tests/regression/usb-abort-test.c` |
| `make test-hal-surface` | Everything the HAL plug-in publishes and asks of the engine, compared with a recorded copy; whose session a start or stop belongs to; one unit starting while another plays | `tests/regression/hal-surface-test.c`, `hal-surface.expected` |
| `make test-split` | The optional two-part driver across a real process boundary: sample placement, two units at once, engine crash, hang and reload, version mismatch, no engine | `tests/regression/split-engine-test.c`, `fake-transport.c`, `split-engine-test.sh` |

The USB regression reproduces [issue #7](https://github.com/bbatarelo/emu-apple-silicon/issues/7)
without relying on YouTube or timing luck. It includes the production engine
implementation so that it tests the real static callbacks, rather than a copy of
their logic. See the [investigation](../docs/issue-investigations/ISSUE-7-INVESTIGATION.md)
for the failure and fix. Passing this fake-interface test does not establish how
the macOS USB stack behaves during unplug or sleep.

`test-hal-surface` runs the production plug-in against a fake host, bus and
engine and diffs what a client would see against `hal-surface.expected`. A
change to a default, a property or a notification fails it; if the change is
meant, `make hal-surface-accept` records the new surface and the diff goes in
the same commit.

`test-split` needs a login session, because launchd starts the test engine.
From ssh or a CI runner without one it reports **NOT RUN** and fails;
`EMU_ALLOW_SKIP=1` lets the rest of the suite pass without it. Its device is
pretend -- host clock, playback looped into capture at the same sample index
-- so it tests the boundary, not USB timing. For a sanitizer pass:

```sh
make test-regression test-split SANITIZE="-fsanitize=address,undefined -g" BUILD=build/asan
```

Use the [two-part development guide](../docs/SPLIT-DRIVER.md) for deployment and the [hardware verification report](../docs/issue-investigations/SPLIT-DRIVER-VERIFICATION.md) for recorded results.

## After installing a change: hardware integration

These commands exercise the **installed** driver. Use the two-part driver for
development. After an engine edit, run `make reload` from the registered
checkout. For a first split install or a change to the installed HAL or shared
ABI, run `make MODE=two-part` and `make MODE=two-part install`.
Use the README's plain `make install` when qualifying the one-part release.
Check the installed version with `EMU_DEVICE=3F0A build/bin/hal-check`
for a Tracker Pre, or substitute the target unit's UID. Stop other playback
and recording on that unit before starting the tests.

```sh
EMU_DEVICE=3F04 make test-integration   # 0404 USB
EMU_DEVICE=3F0A make test-integration   # Tracker Pre
```

Use a unique UID or serial substring instead when multiple units have the same
product ID. `EMU_DEVICE` is resolved to one full UID before anything runs
(`build/bin/hal-stream --uid`); with several units attached and no
`EMU_DEVICE`, or one that matches more than one, the tests stop and list the
UIDs rather than pick one. `EMU_DEVICE=<uid> make test-device` runs
`test-integration` and then `test-recovery` on that unit.

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

- **Recovery:** `EMU_DEVICE=3F04 make test-recovery`. The existing
  `scripts/test-recovery.sh` opens its own silent stream on that unit
  (`tests/integration/hal-stream.c`), injects transient and persistent
  transport failures, checks recovery/dead-device reporting, clears the fault,
  then reopens the stream and checks audio flows and the transport goes idle
  with no client left. Nothing needs to be playing and no default device is
  changed; anything else playing on that unit is interrupted. `EMU_RATE` and
  `EMU_BUFFER` run it at a given rate and buffer size and restore both.
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
