//! Semantic wrappers for the quantities that are easy to confuse.
//!
//! Guidelines section 10.1: sample frames, byte counts and USB frame numbers
//! must not be interchangeable integers. The Tracker Pre makes this trap
//! especially easy to fall into, because capture and playback happen to share
//! the same 6 bytes per frame — a mix-up survives testing on this device and
//! breaks on the next one.

/// A count of sample frames: one sample across all channels.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct SampleFrames(pub u32);

/// A count of bytes on the wire.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct ByteCount(pub u32);

/// A USB frame number as reported by the host controller.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct UsbFrameNumber(pub u64);

/// A sample rate in Hz. Distinct from the device's opaque rate *codes*, which
/// live in `protocol::RateCode`.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct SampleRate(pub u32);
