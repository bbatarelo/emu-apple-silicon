# E-MU driver development

Use the **two-part audio driver by default for development and hardware testing**. It keeps the HAL plug-in loaded while you rebuild and replace the USB engine as the logged-in user. Keep plain `make` and `make install` as the one-part release workflow for README users.

- Before deploying, run `./scripts/engine-service.sh status` to see whether the development service is registered. If it is, work from its registered checkout and use `make engine-status` to inspect the running build and devices.
- For an engine-only change, stop streams on the E-MU units and run `make reload`. This rebuilds the engine and checks the Core Audio and HAL host PIDs across the reload. Leave the split driver installed between iterations.
- For the first split deployment, or after changing the resident HAL or its shared ABI, use `make MODE=two-part` and `make MODE=two-part install`. The install requires admin credentials and restarts system audio. Plain `make install` switches the machine back to one-part.
- Test the requested idle interface from a Mac login session with `EMU_DEVICE=3F0A make test-device` (Tracker Pre) or `EMU_DEVICE=3F04 make test-device` (0404 USB). Use a unique serial substring for two units of the same model. The tests open their own stream and preserve the system sound selections.

See the [development guide](docs/SPLIT-DRIVER.md) for installation, rollback and analog checks, and the [hardware verification report](docs/issue-investigations/SPLIT-DRIVER-VERIFICATION.md) for the tested revision and known limits.
