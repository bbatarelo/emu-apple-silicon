# Reference material provenance

What is in `resources/`, where it came from, and under what terms.

None of it is covered by this project's MIT licence, and no code from any of it
was copied into this project. It was read to understand the hardware's protocol;
the conclusions are recorded in [FINDINGS.md](FINDINGS.md) in this project's own
words. See [NOTICE](../NOTICE) at the repository root.

If you hold rights to any of this and would prefer it not be distributed here,
open an issue and it will be removed.

## 1. Wouter1 EMU-driver

- Path: `resources/EMU-driver-master-wouter1-repo/`
- Upstream: <https://github.com/Wouter1/EMU-driver>
- Snapshot date: 2021-12-10 (directory mtime)
- Nature: a KEXT that began as an independent implementation and was later
  updated against E-MU's released source.
- Support status: discontinued. Per its README, the author moved to Linux and
  ended support. **Apple Silicon was never supported** — the author had no M1
  machine (upstream issue #136).
- Coverage claimed: 44.1/48/88.2/96/176.4/192 kHz, capture and playback, MIDI.
  Tested by the author only on the 0404 USB; Tracker Pre support is
  user-reported.
- Relevance: the closest thing to a working reference for late-era macOS, and
  the most useful source for behaviour E-MU's own code leaves ambiguous. It is a
  KEXT, so none of it ports directly to DriverKit, but the protocol logic does.
- Untracked binaries in this tree: `EMUUSBAudio original.kext`,
  `EMUMIDIDriver orig.plugin`.

## 2. E-MU official source — macOS

- Path: `resources/zaudiodrivermac-code-r7-trunk-official-source/`
- Nature: E-MU's own released CA0188/CA0189 driver source, trunk r7.
- Relevance: authoritative for E-MU-specific extension-unit semantics and clock
  rate codes. Guidelines section 18 flags one behaviour to deliberately *not*
  reproduce — the original can turn a failed `SET_CUR` into apparent success.

## 3. E-MU official source — Windows

- Path: `resources/zaudiodriverwin-code-r5-trunk-official-source/`
- Nature: E-MU's Windows driver source, trunk r5.
- Relevance: useful cross-check on protocol intent. Where the macOS and Windows
  implementations disagree, that disagreement is itself informative about which
  behaviour is incidental.

## 4. Last official macOS driver (binary)

- File: `EMUU_MacAppDrv_US_1_50_07-last-official-driver.dmg`
- Version: 1.50.07
- Published: 2011-10-14
- SHA-256: `a1c39fac02c0e921fe7a290b211ccbafe926a2d89ce20ab4d93f94da228a4d19`
- Size: 12,798,216 bytes
- Nature: the final shipping macOS driver. Its retained C++ symbols map directly
  onto the released source, which makes it useful for confirming that a given
  source tree is really what shipped.
- Redistribution rights unverified. Included for reference; see NOTICE.

## Device identity

| | |
|---|---|
| Device | E-MU Tracker Pre USB |
| Vendor ID | `0x041e` (Creative Labs) |
| Product ID | `0x3f0a` |
| Chipset | CA0189 |

Descriptors are authoritative for interface, alternate-setting and endpoint
topology; only the match identity above is treated as fixed.
