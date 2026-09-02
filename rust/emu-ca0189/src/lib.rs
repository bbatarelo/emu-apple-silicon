//! CA0189 protocol core for the E-MU Tracker Pre.
//!
//! Deliberately knows nothing about DriverKit, IOKit or Core Audio. It consumes
//! abstract facts (descriptor bytes, completion results) and emits decisions and
//! validated models, so the same code serves the userspace probe today and the
//! dext later (guidelines section 28).
//!
//! The crate is `no_std` when built with `--no-default-features`, which is how
//! the dext will consume it. The default `std` feature exists purely so host
//! tests and the userspace probe link against an ordinary runtime; no code
//! outside the panic handler is conditional on it, and nothing here uses an
//! allocator or any facility DriverKit lacks.

#![cfg_attr(not(feature = "std"), no_std)]

pub mod clock;
pub mod descriptor;
pub mod ffi;
pub mod protocol;
pub mod types;

pub use descriptor::{parse_configuration, AltSetting, DeviceModel, ExtensionUnit, ParseError};
pub use clock::{frames_in_packet, output_packet_bytes, ClockEstimator, FeedbackQueue, TimestampFilter};
pub use protocol::{extension_code, selector, ControlSetup, RateCode};
pub use types::{ByteCount, SampleFrames, SampleRate, UsbFrameNumber};

/// Creative Labs, who owned E-MU. Shared across the CA0189 family.
pub const VENDOR_ID: u16 = 0x041e;

/// E-MU Tracker Pre. Descriptors are authoritative for everything else; only
/// the identity is fixed. Sibling devices are listed in `shared/device.h` on the
/// C side -- see docs/ADDING-A-DEVICE.md.
pub const PRODUCT_ID: u16 = 0x3f0a;

#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
