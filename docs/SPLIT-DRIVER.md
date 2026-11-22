# Developing with the two-part E-MU driver

Use the split build on a development Mac when iterating on USB, clocking, recovery or audio transport. The resident HAL publishes the devices to Core Audio, and a separate process named `emu-engine` owns the audio USB interfaces. The CoreMIDI plug-in handles the 0404 MIDI interface in either build.

## Install once

Use the [standard build prerequisites](../README.md#build-and-install-the-e-mu-driver-on-macos). From the checkout where you intend to edit the engine, check whether the development service is registered.

```sh
./scripts/engine-service.sh status
```

If the service is already registered, look up the executable path with the following command. Run subsequent builds and reloads from the checkout containing that executable.

```sh
/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' /Library/LaunchDaemons/net.batarelo.EMUTrackerPre.Engine.plist
```

For the first split install, stop audio on the Mac and run the following commands.

```sh
make MODE=two-part
make MODE=two-part install
make engine-status
```

The install prompts for admin credentials, registers the engine under the installing user, replaces the HAL and MIDI bundles, and restarts the system audio and MIDI daemons. It saves the default input, output and sound-effects selections and attempts to restore them. The engine runs from `build/two-part/emu-engine` inside this checkout. Keep the checkout in place while the service is registered. `make clean` guards the active engine's build directory.

## Edit, reload and test

After an engine-only change, stop streams on the E-MU devices and run the following in your normal Mac login session.

```sh
make reload
EMU_DEVICE=3F0A make test-device
```

`make reload` rebuilds the engine, requests a clean shutdown and waits for launchd to start the replacement and the HAL to reconnect. It reports the old and new engine PIDs along with the Core Audio and HAL host PIDs. Routine reloads run under your account without sudo or restarting Core Audio. Use the split install command again when changing the installed HAL, its remote-engine client, or the shared ABI.

The example targets the Tracker Pre. Use `3F04` for the 0404 USB. To distinguish two units of the same model, run `make test-build`, list the UIDs with `build/bin/hal-stream --list`, and pass the full UID in quotes. `test-device` opens its own silent stream. It runs 20 ordinary restarts, 20 starts with an injected fault, then exercises recovery and reopens the stream. Leave the system defaults as they are and keep other audio clients off the unit under test.

For the Tracker Pre stereo analog checks, patch left output to input 1 and right output to input 2. Set levels below clipping and turn hardware direct monitoring off before running the test.

```sh
EMU_DEVICE=3F0A build/bin/hal-loopback all
```

The analog suite checks channel routing, raw round-trip delay and waveform continuity. The fault-recovery tests work without cables. The test guide covers hardware-free process tests and sanitizers. The README describes how to check the 0404's MIDI endpoints and run a physical DIN loopback. [Test guide](../tests/README.md) · [MIDI diagnostics](../README.md#optional-diagnostics-and-troubleshooting)

## Return to the release driver

Run `make install` when you want to switch the machine back to the one-part release driver. It removes the development service, installs the release bundles and restarts system audio. To remove both audio and MIDI drivers entirely, run `make uninstall`. Both operations require admin credentials.

## Qualification and follow-up

The qualified build was `73ad8f6` at 48 kHz and 512 frames. Both devices passed restart and fault-recovery checks. Tracker Pre passed all 45 analog checks and measured 5.796 ms raw round trip. The 0404 published both MIDI endpoints. Its final post-recovery snapshot showed 2,528 input-underrun frames. Physical 0404 analog and DIN loopback and the split driver's contribution to software-monitoring latency remain open. [Qualification report and raw logs](issue-investigations/SPLIT-DRIVER-VERIFICATION.md)

For transport design, see the [architecture](ARCHITECTURE.md#optional-two-part-driver). The [latency investigation](SPLIT-DRIVER-PLAN.md#latency-opportunity-after-parity) and [cross-project candidates](SPLIT-DRIVER-PLAN.md#contributions-between-repositories) describe what can be reused between E-MU, the generic driver template and Fast Track Ultra.
