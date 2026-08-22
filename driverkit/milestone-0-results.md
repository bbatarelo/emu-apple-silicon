# Milestone 0 — Rust-in-DriverKit feasibility

Empirical result of the gate defined in
`EMU_Tracker_Pre_Development_Guidelines.md` Part V, run on 2026-08-20.

**Verdict: conditional PASS.** Every test that can be run without Apple's
DriverKit entitlements passes cleanly. Tests 3 and 4 require a loadable dext and
are blocked; see [driverkit-entitlement-request.md](driverkit-entitlement-request.md).

Environment: macOS 26.5.2 (25F84), arm64, Xcode 26.5 (17F42),
DriverKit SDK 25.5, rustc 1.100.0-nightly (f7d782a3b 2026-08-19).
Full fingerprint in `toolchain-notes/2026-08-20-toolchain.txt`.

## Test results

| Test | Description | Result |
|---|---|---|
| 0 | Rust logic + C ABI on host toolchain | **PASS** — 12/12 checks (`make test`) |
| 1 | Static archive inspection | **PASS** — arm64, `platform DRIVERKIT`, 8 symbols exported, no runtime deps |
| 2 | Link into DriverKit dext | **PASS** via Path B2; **FAIL** via Path B1 (see below) |
| 3 | Install / load / start | **BLOCKED** — no DriverKit entitlement |
| 4 | Repeated load/start/stop | **BLOCKED** — depends on Test 3 |
| 5 | Mutable Rust state over FFI | **PASS** at source and link level; runtime unverified |
| 6 | Atomics | **PASS** at source and link level; runtime unverified |
| 7 | USBDriverKit descriptor probe | Not started — Milestone 1 |

Tests 5 and 6 were implemented up front rather than staged, since they cost
nothing extra to link and prove. Their *runtime* behaviour inside a dext is
verified by `TrackerPreDriver::Start()`, which runs all three stages and fails
`Start` outright if any result is wrong — but that code cannot execute until
Test 3 unblocks.

## Test 2 in detail — the decisive result

**Path B1 (plain `aarch64-apple-darwin` archive): rejected.**

```
ld: building for 'driverKit', but linking in object file
    (libdriverkit_rust_probe.a[2](...rcgu.o)) built for 'macOS'
```

Guidelines section 23 forbids suppressing this, so the path is closed. The
archive also carried 368 members and pulled in `_sysctlbyname`,
`_rust_eh_personality`, `___stack_chk_fail`, `___stack_chk_guard` and
`___assert_rtn`.

**Path B2 (custom target specification): accepted.** With `os = "driverkit"` and
`llvm-target = "arm64-apple-driverkit25.5"`, the archive drops to **3 members**,
every libSystem and Rust-runtime dependency disappears, and it links. Mechanics
in [rust-driverkit-target.md](rust-driverkit-target.md).

Evidence from the built dext:

```
platform DRIVERKIT, minos 25.5, arm64

__TEXT,__text  _emu_rust_probe_add   _probe_core_align   _probe_core_destroy
               _probe_core_increment _probe_core_init    _probe_core_size
               _probe_counter_bump   _probe_counter_read
__DATA,__bss   ...driverkit_rust_probe13PROBE_COUNTER
```

Disassembly confirms the marker constant is materialized in real arm64 code
rather than folded at the C++ call site:

```
_emu_rust_probe_add:
    mov  w8, #0x5500
    movk w8, #0x454d, lsl #16
```

Undefined symbols in the dext are exclusively DriverKit C++ ABI
(`IOService`, `OSMetaClassBase`, …) plus `_memcpy`/`_memset`/`_memcmp`/`_bzero`,
which the SDK provides. No libc, no `std`, no Rust runtime.

## Against the section 24 criteria

| PASS criterion | Status |
|---|---|
| Reproducible build from clean checkout | Yes — `make build` verified from `rm -rf build rust-spike/target` |
| No patched Rust compiler | Yes — stock nightly from rustup |
| No private Apple framework dependency | Yes |
| Static library links cleanly into DriverKit target | Yes |
| No unexpected Rust runtime dependency | Yes — only SDK-provided mem functions |
| dext signs, installs and starts normally | **Unverified** — entitlement blocked |
| C++ ↔ Rust calls work reliably | Link-verified; runtime unverified |
| Teardown is clean | **Unverified** — entitlement blocked |
| Build documentable in a few scripts | Yes — `scripts/`, ~6 small files |
| Rust subset sufficient for the real core | Yes — fixed memory, atomics, no allocator |

No FAIL criterion is currently met. Nothing required a compiler fork, an LLVM
patch, suppressed linker checks, or unstable ABI tricks.

The two honest caveats:

- **Nightly + `-Z build-std`.** Custom targets are unstable by definition. This
  is not a patched compiler, but it is not a stability guarantee either, and
  rustc's target-spec schema can change under us. Mitigated by generating the
  spec from rustc's live output and by keeping dated fingerprints in
  `toolchain-notes/`.
- **Runtime behaviour is genuinely unproven.** Linking is not loading. A dext
  that links can still fail at load, at `Start`, or during teardown. The
  verdict stays *conditional* until Tests 3 and 4 actually run.

## Recommendation

Proceed on the assumption that the Rust core is viable, but do not commit
irreversibly until Tests 3 and 4 pass.

Concretely: keep the Rust core free of Apple types, as the guidelines already
require. That boundary is what makes the decision cheap to reverse — if the dext
turns out to misbehave at runtime, the same protocol logic ports to C++ without
redesign, exactly as section 24 anticipates.

The next action is not technical. **File the entitlement request**; it gates
everything and Apple's review takes weeks.

Milestone 1 (descriptor probe) needs no entitlement and can proceed in parallel
using userspace IOKit, though it does need the hardware connected.
