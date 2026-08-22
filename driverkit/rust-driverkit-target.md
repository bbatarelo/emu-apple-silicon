# Rust on the DriverKit target

How the freestanding Rust core is built for `arm64-apple-driverkit`, why the
obvious approach does not work, and what to check when a toolchain upgrade
breaks it.

Background: `EMU_Tracker_Pre_Development_Guidelines.md` section 14.

## Path B1 (use the normal Apple Silicon target) — rejected

Guidelines section 14 proposed first trying a plain `aarch64-apple-darwin`
`no_std` staticlib and linking it into the dext, on the theory that a static
archive is "ultimately relocatable object code".

It is not. Mach-O objects carry an `LC_BUILD_VERSION` load command naming the
platform, and `ld` enforces it:

```
ld: building for 'driverKit', but linking in object file
    (libdriverkit_rust_probe.a[2](...rcgu.o)) built for 'macOS'
```

Section 23 (Test 2) says explicitly not to paper this over with linker
suppression flags, so Path B1 is closed. The failure is clean and unambiguous,
which is the good case.

Two secondary problems would have bitten later anyway. Built for
`aarch64-apple-darwin`, the archive contained **368 members** and referenced
`_sysctlbyname`, `_rust_eh_personality`, `___stack_chk_fail`,
`___stack_chk_guard` and `___assert_rtn` — libSystem and Rust runtime
dependencies that DriverKit does not provide.

## Path B2 (custom target specification) — adopted

`rust-spike/targets/aarch64-apple-driverkit.json` is **generated**, not
committed by hand, by `scripts/gen-rust-target.sh`. Guidelines section 14 asks
for exactly this: the spec depends on the installed Xcode SDK version and on
rustc's own schema, so it is derived on every build from

1. `rustc --print target-spec-json --target aarch64-apple-darwin`, and
2. `xcrun --sdk driverkit --show-sdk-version`.

The build needs nightly, because a custom target requires `-Z build-std` to
compile `core` and `compiler_builtins` for it. Nightly is an official release
channel, not a patched compiler, so this stays inside the section 24 PASS
criteria.

### The one change that matters

```json
"llvm-target": "arm64-apple-driverkit25.5",
"os": "driverkit"
```

That produces `platform DRIVERKIT, minos 25.5` in `LC_BUILD_VERSION`, which is
what makes the link legal.

Setting `os` to `driverkit` has a second, larger benefit: `compiler_builtins`
stops compiling its macOS-specific paths. The archive drops from 368 members to
**3**, and every libSystem and Rust-runtime dependency listed above disappears.

### Why `is-like-darwin` is false

This looks wrong at first glance and is worth understanding before anyone
"fixes" it.

With `is_like_darwin` set, rustc routes the triple through
`add_version_to_llvm_target`, which calls `deployment_target()`. That function
matches on a hardcoded list of Apple OSes and hits `unreachable!()` for anything
else:

```
internal error: entered unreachable code:
tried to get deployment target for non-Apple platform: Other("driverkit")
```

That is an ICE, not a diagnostic. Turning the flag off makes rustc pass
`llvm-target` through verbatim, which is what we want — the version is already
in the triple.

Nothing is lost by disabling it. Mach-O output comes from
`"binary-format": "mach-o"`, and the leading-underscore symbol prefix comes from
the `m:o` component of the data layout. Both are still set. The generated
archive is confirmed to be a valid arm64 Mach-O `darwin`-format archive.

rustc then enforces two consistency rules that force further changes:

- `is_like_darwin` **iff** `vendor == "apple"` → `vendor` becomes `"unknown"`
  (it must also be non-empty). This only affects `cfg(target_vendor)`.
- `linker_flavor == "darwin"` **iff** `is_like_darwin` → `"gnu-cc"`.
  Irrelevant in practice: rustc only produces a staticlib here and never links.
  Xcode owns the final link.

### Building

```bash
make rust
```

or directly:

```bash
cd rust-spike
RUST_TARGET_PATH=$PWD/targets RUSTFLAGS="-Zunstable-options" \
  cargo +nightly build --release \
  --target aarch64-apple-driverkit \
  -Z build-std=core,compiler_builtins
```

### A cargo caching trap

Cargo caches target-spec probe results in `target/.rustc_info.json`. If the spec
changes — or previously ICEd — cargo keeps replaying the stale result and the
build fails with an error that no longer reflects the file on disk.
`gen-rust-target.sh` deletes that file on every run. If a spec change appears to
have no effect, this is why.

## Expected external symbols

The DriverKit archive should reference only:

```
_memcpy  _memset  _memcmp  _bzero
```

LLVM emits these for ordinary struct and slice moves, and the DriverKit SDK
provides them. `scripts/build-rust.sh` hard-fails on anything else, because a
new unresolved symbol means `std`, libSystem or a Rust runtime dependency has
crept in — a section 24 FAIL condition.

## arm64 only

The dext target sets `ARCHS = arm64`. The Rust archive is arm64-only, so a
universal build fails to link:

```
ld: symbol(s) not found for architecture x86_64
```

This matches the project's stated scope (Apple Silicon revival). Supporting
Intel would mean generating an `x86_64-apple-driverkit` spec the same way and
`lipo`-ing the archives together. Nothing in the design prevents it; it just is
not a goal.

## When a toolchain upgrade breaks this

The custom target depends on rustc internals that carry no stability promise.
`toolchain-notes/` holds dated fingerprints so the change can be diffed rather
than guessed at. Refresh with `make toolchain-notes`.

Check in this order:

1. Did the DriverKit SDK version change? The spec regenerates automatically;
   confirm `llvm-target` matches `xcrun --sdk driverkit --show-sdk-version`.
2. Did rustc's target-spec schema change? Run the `rustc --print
   target-spec-json` command above and diff against the notes.
3. Did new consistency assertions appear? They surface as
   `error loading target specification: ...`, which names the rule directly.
4. Did new external symbols appear? `make rust` fails loudly and lists them.

## Reproducibility caveat

`rust-toolchain.toml` currently tracks `nightly` rather than a pinned date, so a
clean checkout gets whatever nightly is current. The verified-good version is
recorded in `toolchain-notes/`. Pinning to an exact nightly would make builds
byte-reproducible across machines, at the cost of that build eventually
disappearing from static.rust-lang.org. That trade-off is worth making
deliberately before the project depends on it.
