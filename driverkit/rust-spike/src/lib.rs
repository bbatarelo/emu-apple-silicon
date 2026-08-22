//! Minimal Rust-in-DriverKit feasibility probe.
//!
//! Purpose is narrow: prove that a freestanding Rust static library can be
//! linked into an Apple Silicon DriverKit dext and called across a C ABI.
//! See EMU_Tracker_Pre_Development_Guidelines.md, Part V.
//!
//! Deliberately contains no USB, no Core Audio and no Tracker Pre logic.
//! No std, no allocator, no threads, no dependencies.

#![no_std]

use core::panic::PanicInfo;
use core::sync::atomic::{AtomicU64, Ordering};

/// Probe-only policy. The production streaming engine must make panic paths
/// impossible rather than relying on a handler; the fatal policy there will be
/// chosen and documented explicitly.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

/// Recognizable marker so a returned value is provably Rust-originated and not
/// a C++ constant folded at the call site. "EMU\0" in the high bytes.
const EMU_MARKER: u32 = 0x454d_5500;

// ---------------------------------------------------------------------------
// Test 1-4: pure function, no state.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn emu_rust_probe_add(a: u32, b: u32) -> u32 {
    a.wrapping_add(b).wrapping_add(EMU_MARKER)
}

// ---------------------------------------------------------------------------
// Test 5: opaque object with mutable state.
//
// Memory is supplied by the caller so the probe stays allocation-free. C++
// owns the storage; Rust only ever borrows it for the lifetime of the handle.
// ---------------------------------------------------------------------------

/// Opaque to C++. Layout is deliberately simple and `repr(C)` so the size and
/// alignment contract below is stable.
#[repr(C)]
pub struct ProbeCore {
    magic: u32,
    counter: u32,
}

const PROBE_CORE_MAGIC: u32 = 0x5052_4243; // "PRBC"

/// Byte size C++ must reserve for a `ProbeCore`.
#[no_mangle]
pub extern "C" fn probe_core_size() -> u32 {
    core::mem::size_of::<ProbeCore>() as u32
}

/// Alignment C++ must satisfy for that storage.
#[no_mangle]
pub extern "C" fn probe_core_align() -> u32 {
    core::mem::align_of::<ProbeCore>() as u32
}

/// Initializes caller-provided storage in place.
///
/// Returns null if the pointer is null or insufficiently aligned. Every raw
/// pointer from C++ is validated once, here at the FFI edge; everything past
/// this point is safe Rust.
///
/// # Safety
/// `storage` must point to at least `probe_core_size()` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn probe_core_init(storage: *mut u8, initial: u32) -> *mut ProbeCore {
    if storage.is_null() || (storage as usize) % core::mem::align_of::<ProbeCore>() != 0 {
        return core::ptr::null_mut();
    }

    let core_ptr = storage as *mut ProbeCore;
    core_ptr.write(ProbeCore {
        magic: PROBE_CORE_MAGIC,
        counter: initial,
    });
    core_ptr
}

/// Increments and returns the new value, or `u32::MAX` if the handle is invalid.
///
/// # Safety
/// `core` must be a pointer returned by `probe_core_init` and not yet destroyed.
#[no_mangle]
pub unsafe extern "C" fn probe_core_increment(core: *mut ProbeCore) -> u32 {
    let Some(core) = core.as_mut() else {
        return u32::MAX;
    };
    if core.magic != PROBE_CORE_MAGIC {
        return u32::MAX;
    }
    core.counter = core.counter.wrapping_add(1);
    core.counter
}

/// Invalidates the handle. Does not free anything: C++ owns the storage.
///
/// # Safety
/// `core` must be a pointer returned by `probe_core_init`.
#[no_mangle]
pub unsafe extern "C" fn probe_core_destroy(core: *mut ProbeCore) {
    if let Some(core) = core.as_mut() {
        core.magic = 0;
    }
}

// ---------------------------------------------------------------------------
// Test 6: atomics.
//
// Proves the subset of `core` the future statistics and packet engine needs.
// ---------------------------------------------------------------------------

static PROBE_COUNTER: AtomicU64 = AtomicU64::new(0);

#[no_mangle]
pub extern "C" fn probe_counter_bump() -> u64 {
    PROBE_COUNTER.fetch_add(1, Ordering::Relaxed).wrapping_add(1)
}

#[no_mangle]
pub extern "C" fn probe_counter_read() -> u64 {
    PROBE_COUNTER.load(Ordering::Relaxed)
}
