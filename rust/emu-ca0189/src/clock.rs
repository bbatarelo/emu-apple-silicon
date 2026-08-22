//! Packet accounting, immediate feedback, and hardware clock estimation.
//!
//! Guidelines section 17: the immediate packet planner and the long-term
//! estimator are separate things. The planner reproduces the known-working E-MU
//! behaviour — sample frames observed on the capture stream drive the next
//! playback request — while the estimator is an additional observation tool and
//! must not replace the planner until evidence justifies it.
//!
//! No allocation, no floating point in the planner, and nothing here knows what
//! a USB transfer is.

use crate::types::{ByteCount, SampleFrames};

/// How many sample frames a capture packet of `bytes` contains.
///
/// Floors deliberately. A partial sample frame cannot exist, and on this device
/// it is not hypothetical: at `bInterval = 3` every capture packet carries
/// exactly 4 bytes beyond a whole number of frames. Summing raw byte counts and
/// dividing once at the end lets those remainders accumulate into frames that
/// were never sent — which measured as +7553 ppm before it was fixed.
pub fn frames_in_packet(bytes: ByteCount, bytes_per_frame: u32) -> SampleFrames {
    if bytes_per_frame == 0 {
        return SampleFrames(0);
    }
    SampleFrames(bytes.0 / bytes_per_frame)
}

/// Bytes to request for an output packet carrying `frames`.
///
/// Uses the *output* direction's `bytes_per_frame`. The two directions happen
/// to agree on the Tracker Pre (both 6), which is exactly the coincidence that
/// makes a bytes-versus-frames mix-up survive testing on this device and fail
/// on another.
pub fn output_packet_bytes(frames: SampleFrames, output_bytes_per_frame: u32) -> ByteCount {
    ByteCount(frames.0 * output_bytes_per_frame)
}

/// Fixed-capacity queue carrying sample-frame counts from the capture stream to
/// the playback planner.
///
/// Entries are **sample frames per service interval**, never bytes. Conflating
/// the two is the historical E-MU/Wouter ambiguity the guidelines call out.
pub struct FeedbackQueue<const N: usize> {
    entries: [SampleFrames; N],
    head: usize,
    len: usize,
    /// Counts pushes that were dropped because the queue was full. A real
    /// driver surfaces this as a diagnostic rather than silently discarding.
    pub overflows: u32,
}

impl<const N: usize> Default for FeedbackQueue<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> FeedbackQueue<N> {
    pub const fn new() -> Self {
        FeedbackQueue {
            entries: [SampleFrames(0); N],
            head: 0,
            len: 0,
            overflows: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn capacity(&self) -> usize {
        N
    }

    /// Records the sample-frame count observed on one capture service interval.
    pub fn push(&mut self, frames: SampleFrames) {
        if self.len == N {
            self.overflows += 1;
            return;
        }
        let tail = (self.head + self.len) % N;
        self.entries[tail] = frames;
        self.len += 1;
    }

    /// Takes the next count for the playback planner.
    pub fn pop(&mut self) -> Option<SampleFrames> {
        if self.len == 0 {
            return None;
        }
        let value = self.entries[self.head];
        self.head = (self.head + 1) % N;
        self.len -= 1;
        Some(value)
    }

    /// Queue occupancy as a percentage, for diagnostics.
    pub fn depth_percent(&self) -> u32 {
        if N == 0 {
            return 0;
        }
        (self.len * 100 / N) as u32
    }
}

/// Long-term measurement of the device's clock against the host's.
///
/// Every USB service interval is a known duration derived from the host
/// controller's own clock, so sample frames per interval measures the device
/// directly. Accumulation is in integers; only the reported rate is floating
/// point, and nothing here runs on the completion path's critical section.
#[derive(Clone, Copy, Debug)]
pub struct ClockEstimator {
    accumulated_frames: u64,
    accumulated_intervals: u64,
    nominal_rate_hz: u32,
    /// Duration of one service interval in nanoseconds. Derived from bInterval:
    /// 125_000 << (bInterval - 1).
    interval_ns: u64,
}

impl ClockEstimator {
    /// `b_interval` is the endpoint's descriptor value; for high speed the
    /// period is 2^(bInterval-1) microframes of 125 µs each.
    pub fn new(nominal_rate_hz: u32, b_interval: u8) -> ClockEstimator {
        let shift = b_interval.saturating_sub(1).min(15);
        ClockEstimator {
            accumulated_frames: 0,
            accumulated_intervals: 0,
            nominal_rate_hz,
            interval_ns: 125_000u64 << shift,
        }
    }

    pub fn interval_ns(&self) -> u64 {
        self.interval_ns
    }

    /// Records one completed service interval.
    pub fn observe(&mut self, frames: SampleFrames) {
        self.accumulated_frames += frames.0 as u64;
        self.accumulated_intervals += 1;
    }

    pub fn intervals(&self) -> u64 {
        self.accumulated_intervals
    }

    pub fn frames(&self) -> u64 {
        self.accumulated_frames
    }

    pub fn frames_per_interval(&self) -> f64 {
        if self.accumulated_intervals == 0 {
            return 0.0;
        }
        self.accumulated_frames as f64 / self.accumulated_intervals as f64
    }

    /// Measured device sample rate in Hz, or 0 before anything is observed.
    pub fn measured_hz(&self) -> f64 {
        if self.accumulated_intervals == 0 {
            return 0.0;
        }
        let seconds = (self.accumulated_intervals as f64) * (self.interval_ns as f64) / 1e9;
        self.accumulated_frames as f64 / seconds
    }

    /// Deviation from nominal in parts per million.
    pub fn drift_ppm(&self) -> f64 {
        if self.nominal_rate_hz == 0 || self.accumulated_intervals == 0 {
            return 0.0;
        }
        let nominal = self.nominal_rate_hz as f64;
        (self.measured_hz() - nominal) / nominal * 1e6
    }

    pub fn reset(&mut self) {
        self.accumulated_frames = 0;
        self.accumulated_intervals = 0;
    }
}

/// Nominal packet size for a rate that does not divide evenly into service
/// intervals.
///
/// At 176.4 kHz with a 0.5 ms interval the device averages 88.2 sample frames.
/// A fallback that truncates to 88 is 2268 ppm slow every time it is used, and
/// with starvation during priming that measured as -36 ppm over a whole run.
/// Distributing the remainder keeps the long-run average exact, which is the
/// same thing the device itself does when it alternates packet sizes.
#[derive(Clone, Copy, Debug)]
pub struct NominalRate {
    /// Sample frames per interval, scaled by `DEN`.
    numerator: u64,
    accumulator: u64,
}

impl NominalRate {
    /// Fixed denominator. Nanoseconds per second, so `sample_rate * interval_ns`
    /// is an exact numerator with no rounding anywhere.
    const DEN: u64 = 1_000_000_000;

    pub fn new(sample_rate: u32, interval_ns: u64) -> NominalRate {
        NominalRate {
            numerator: sample_rate as u64 * interval_ns,
            accumulator: 0,
        }
    }

    /// Sample frames for the next interval, alternating between the floor and
    /// ceiling so the running average matches the true rate.
    pub fn next(&mut self) -> SampleFrames {
        self.accumulator += self.numerator;
        let frames = self.accumulator / Self::DEN;
        self.accumulator -= frames * Self::DEN;
        SampleFrames(frames as u32)
    }

    /// Whole frames per interval, ignoring the remainder.
    pub fn floor(&self) -> u32 {
        (self.numerator / Self::DEN) as u32
    }
}
