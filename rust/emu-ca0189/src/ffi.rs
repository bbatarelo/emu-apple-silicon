//! C ABI surface.
//!
//! The rules from guidelines section 13 apply: small C ABI, `#[repr(C)]` on
//! everything shared, raw pointers validated once here at the edge, and no
//! panics allowed to cross the boundary.

use crate::descriptor::{parse_configuration, DeviceModel};
use crate::protocol::{selector, ControlSetup, RateCode};

/// Parses a configuration descriptor into `out`.
///
/// Returns 0 on success, or a positive `ParseError` code. Returns -1 if the
/// caller passed a null pointer.
///
/// # Safety
/// `bytes` must point to `len` readable bytes and `out` to a writable
/// `DeviceModel`.
#[no_mangle]
pub unsafe extern "C" fn emu_parse_config_descriptor(
    bytes: *const u8,
    len: u32,
    out: *mut DeviceModel,
) -> i32 {
    if bytes.is_null() || out.is_null() {
        return -1;
    }
    let slice = core::slice::from_raw_parts(bytes, len as usize);

    match parse_configuration(slice) {
        Ok(model) => {
            out.write(model);
            0
        }
        Err(e) => e.code() as i32,
    }
}

/// Builds the setup packet that reads the active clock rate code.
///
/// # Safety
/// `out` must point to a writable `ControlSetup`.
#[no_mangle]
pub unsafe extern "C" fn emu_setup_get_clock_rate(
    unit_id: u8,
    interface: u8,
    out: *mut ControlSetup,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    out.write(ControlSetup::get_xu(unit_id, interface, selector::CLOCK_RATE, 1));
    0
}

/// Builds the setup packet that sets the clock rate code.
///
/// # Safety
/// `out` must point to a writable `ControlSetup`.
#[no_mangle]
pub unsafe extern "C" fn emu_setup_set_clock_rate(
    unit_id: u8,
    interface: u8,
    out: *mut ControlSetup,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    out.write(ControlSetup::set_xu(unit_id, interface, selector::CLOCK_RATE, 1));
    0
}

/// Builds the setup packet that reads the supported-rate bitmap.
///
/// # Safety
/// `out` must point to a writable `ControlSetup`.
#[no_mangle]
pub unsafe extern "C" fn emu_setup_get_clock_rate_support(
    unit_id: u8,
    interface: u8,
    length: u16,
    out: *mut ControlSetup,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    out.write(ControlSetup::get_xu(
        unit_id,
        interface,
        selector::CLOCK_RATE_SUPPORT,
        length,
    ));
    0
}

/// Frequency in Hz for a device rate code, or 0 if the code is not one the
/// device defines.
#[no_mangle]
pub extern "C" fn emu_rate_code_to_hz(code: u8) -> u32 {
    match RateCode::from_code(code) {
        Some(r) => r.hz(),
        None => 0,
    }
}

/// Device rate code for a frequency, or 0xff if the rate is not supported.
#[no_mangle]
pub extern "C" fn emu_hz_to_rate_code(hz: u32) -> u8 {
    match RateCode::from_hz(hz) {
        Some(r) => r as u8,
        None => 0xff,
    }
}

/// Size of `DeviceModel`, so C can assert its own view of the struct agrees.
/// A silent layout mismatch here would corrupt every field the tool prints.
#[no_mangle]
pub extern "C" fn emu_device_model_size() -> u32 {
    core::mem::size_of::<DeviceModel>() as u32
}

// ---------------------------------------------------------------------------
// Streaming: feedback queue and packet planning.
//
// Exposed so the userspace duplex engine drives playback through exactly the
// same code the dext will use, rather than a C reimplementation that could
// drift away from what the tests cover.
// ---------------------------------------------------------------------------

use crate::clock::{frames_in_packet, output_packet_bytes, FeedbackQueue, NominalRate};
use crate::types::{ByteCount, SampleFrames};

/// Depth of the feedback queue. 128 service intervals is far deeper than the
/// engine should ever need; overflow means capture and playback have decoupled.
pub const EMU_FEEDBACK_CAPACITY: usize = 128;

/// Opaque to C. Caller provides the storage, so nothing here allocates.
#[repr(C)]
pub struct EmuFeedback {
    magic: u32,
    queue: FeedbackQueue<EMU_FEEDBACK_CAPACITY>,
    /// Fallback packet size, exact on average even for fractional rates.
    nominal: NominalRate,
    /// Times a playback packet had to fall back to the nominal size because no
    /// capture measurement was available.
    starved: u32,
}

const EMU_FEEDBACK_MAGIC: u32 = 0x464e_4551; // "FBEQ"

#[no_mangle]
pub extern "C" fn emu_feedback_size() -> u32 {
    core::mem::size_of::<EmuFeedback>() as u32
}

#[no_mangle]
pub extern "C" fn emu_feedback_align() -> u32 {
    core::mem::align_of::<EmuFeedback>() as u32
}

/// Initializes caller-provided storage in place.
///
/// # Safety
/// `storage` must point to at least `emu_feedback_size()` writable, suitably
/// aligned bytes.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_init(storage: *mut u8) -> *mut EmuFeedback {
    if storage.is_null() || (storage as usize) % core::mem::align_of::<EmuFeedback>() != 0 {
        return core::ptr::null_mut();
    }
    let p = storage as *mut EmuFeedback;
    p.write(EmuFeedback {
        magic: EMU_FEEDBACK_MAGIC,
        queue: FeedbackQueue::new(),
        nominal: NominalRate::new(0, 0),
        starved: 0,
    });
    p
}

unsafe fn feedback<'a>(fb: *mut EmuFeedback) -> Option<&'a mut EmuFeedback> {
    let fb = fb.as_mut()?;
    if fb.magic != EMU_FEEDBACK_MAGIC {
        return None;
    }
    Some(fb)
}

/// Sets the fallback packet size from the stream format, so starvation falls
/// back to a rate-accurate value rather than a truncated one.
///
/// # Safety
/// `fb` must come from `emu_feedback_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_set_nominal(
    fb: *mut EmuFeedback,
    sample_rate: u32,
    interval_ns: u64,
) {
    if let Some(fb) = feedback(fb) {
        fb.nominal = NominalRate::new(sample_rate, interval_ns);
    }
}

/// Records the sample frames observed on one capture service interval.
///
/// # Safety
/// `fb` must come from `emu_feedback_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_push(fb: *mut EmuFeedback, frames: u32) {
    if let Some(fb) = feedback(fb) {
        fb.queue.push(SampleFrames(frames));
    }
}

/// Sample frames for the next playback packet.
///
/// Falls back to the configured nominal rate when no capture measurement is
/// queued, which happens while the streams are priming, and counts that as
/// starvation so it cannot hide in steady state. `fallback` is used only if no
/// nominal has been set.
///
/// # Safety
/// `fb` must come from `emu_feedback_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_next(fb: *mut EmuFeedback, fallback: u32) -> u32 {
    match feedback(fb) {
        Some(fb) => match fb.queue.pop() {
            Some(frames) => frames.0,
            None => {
                fb.starved += 1;
                let n = fb.nominal.next().0;
                if n > 0 { n } else { fallback }
            }
        },
        None => fallback,
    }
}

/// # Safety
/// `fb` must come from `emu_feedback_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_depth(fb: *mut EmuFeedback) -> u32 {
    feedback(fb).map_or(0, |fb| fb.queue.len() as u32)
}

/// # Safety
/// `fb` must come from `emu_feedback_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_overflows(fb: *mut EmuFeedback) -> u32 {
    feedback(fb).map_or(0, |fb| fb.queue.overflows)
}

/// # Safety
/// `fb` must come from `emu_feedback_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_feedback_starved(fb: *mut EmuFeedback) -> u32 {
    feedback(fb).map_or(0, |fb| fb.starved)
}

// ---------------------------------------------------------------------------
// Timestamp filtering: smooths the completion timestamps that anchor Core
// Audio's timeline. Same caller-provided-storage pattern as EmuFeedback.
// ---------------------------------------------------------------------------

use crate::clock::TimestampFilter;

/// Opaque to C. Caller provides the storage, so nothing here allocates.
#[repr(C)]
pub struct EmuTsFilter {
    magic: u32,
    filter: TimestampFilter,
}

const EMU_TS_FILTER_MAGIC: u32 = 0x5453_464c; // "TSFL"

#[no_mangle]
pub extern "C" fn emu_ts_filter_size() -> u32 {
    core::mem::size_of::<EmuTsFilter>() as u32
}

#[no_mangle]
pub extern "C" fn emu_ts_filter_align() -> u32 {
    core::mem::align_of::<EmuTsFilter>() as u32
}

/// Initializes caller-provided storage in place. `start` is the timestamp the
/// stream is expected to begin at; `nominal_step` the expected spacing of
/// observations, in the same unit.
///
/// # Safety
/// `storage` must point to at least `emu_ts_filter_size()` writable, suitably
/// aligned bytes.
#[no_mangle]
pub unsafe extern "C" fn emu_ts_filter_init(
    storage: *mut u8,
    start: u64,
    nominal_step: u64,
) -> *mut EmuTsFilter {
    if storage.is_null() || (storage as usize) % core::mem::align_of::<EmuTsFilter>() != 0 {
        return core::ptr::null_mut();
    }
    let p = storage as *mut EmuTsFilter;
    p.write(EmuTsFilter {
        magic: EMU_TS_FILTER_MAGIC,
        filter: TimestampFilter::new(start, nominal_step),
    });
    p
}

/// Feeds one raw timestamp, returns the filtered one. Returns `raw` unchanged
/// if `f` is not a valid filter.
///
/// # Safety
/// `f` must come from `emu_ts_filter_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_ts_filter_apply(f: *mut EmuTsFilter, raw: u64) -> u64 {
    match f.as_mut() {
        Some(f) if f.magic == EMU_TS_FILTER_MAGIC => f.filter.filter(raw),
        _ => raw,
    }
}

/// Moves the filter's prediction to `expected_next` -- the raw timestamp the
/// next observation is expected to carry -- keeping the rate it has learned.
/// For a discontinuity the caller knows about in advance; see
/// `TimestampFilter::rebase`.
///
/// # Safety
/// `f` must come from `emu_ts_filter_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_ts_filter_rebase(f: *mut EmuTsFilter, expected_next: u64) {
    if let Some(f) = f.as_mut() {
        if f.magic == EMU_TS_FILTER_MAGIC {
            f.filter.rebase(expected_next);
        }
    }
}

/// How often the filter snapped to a discontinuity instead of slewing.
///
/// # Safety
/// `f` must come from `emu_ts_filter_init`.
#[no_mangle]
pub unsafe extern "C" fn emu_ts_filter_resets(f: *mut EmuTsFilter) -> u32 {
    match f.as_mut() {
        Some(f) if f.magic == EMU_TS_FILTER_MAGIC => f.filter.resets(),
        _ => 0,
    }
}

/// Whole sample frames carried by a packet of `bytes`.
#[no_mangle]
pub extern "C" fn emu_frames_in_packet(bytes: u32, bytes_per_frame: u32) -> u32 {
    frames_in_packet(ByteCount(bytes), bytes_per_frame).0
}

/// Bytes to request for an output packet carrying `frames`.
#[no_mangle]
pub extern "C" fn emu_output_packet_bytes(frames: u32, output_bytes_per_frame: u32) -> u32 {
    output_packet_bytes(SampleFrames(frames), output_bytes_per_frame).0
}
