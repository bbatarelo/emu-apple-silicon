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

/// Critically damped smoothing for the stream of transfer-completion
/// timestamps that anchors Core Audio's timeline.
///
/// A port of the mass-spring-damper filter from the original EMUUSBAudio kext
/// (`LowPassFilter.cpp`, Wouter Pasman), which was tuned against this hardware
/// family: the raw USB timestamps are good to ~0.01 ms, but the thread that
/// observes them takes occasional 1–3 ms scheduling hits, and an anchor that
/// follows those hits drags Core Audio's clock model around audibly. The filter
/// is a position `x` advancing by a velocity `dx` once per observation, with
/// the observed error pulling on a weak, critically damped spring: `x` barely
/// moves for a late observation, while a genuine rate difference steadily
/// adjusts `dx`.
///
/// Observations must arrive at a uniform cadence — one per USB request — since
/// the filter models time per observation, not time per frame.
///
/// Differences from the kext: mass and damping are scaled for a ~2 ms cadence
/// rather than a buffer-wrap cadence, the arithmetic is f64 rather than the
/// kext's integer math (this runs on the engine thread, not the realtime audio
/// thread), and recovery from gross discontinuities (a resync after a scheduling
/// overrun, an unplug) is a counted hard reset rather than the kext's
/// good-wraps gate.
#[derive(Clone, Copy, Debug)]
pub struct TimestampFilter {
    /// Filtered position: the smoothed timestamp last returned.
    x: f64,
    /// Filtered velocity: time per observation.
    dx: f64,
    /// Previous error, for the damping term.
    u: f64,
    /// Nominal time per observation. Bounds `dx` and sets the reset threshold.
    step: f64,
    resets: u32,
}

/// Spring constant, mass, and critical damping (`2·sqrt(K·M)`, precomputed
/// because no_std has no sqrt). The kext used K=1, M=1000, DA=63 at its slower
/// cadence; the larger mass keeps a comparable real-time constant — roughly 126
/// observations, ~0.25 s at the engine's 2 ms request cadence.
const FILTER_K: f64 = 1.0;
const FILTER_M: f64 = 4000.0;
const FILTER_DA: f64 = 126.49;

/// Observations further than this many steps from prediction are a
/// discontinuity, not jitter: snap instead of slewing through garbage.
///
/// Deliberately tight. With hardware completion timestamps the observation
/// jitter is microseconds, so anything milliseconds off prediction means the
/// bus schedule genuinely moved and snapping is correct. An earlier value of
/// 25 steps left a blind spot exactly where stall-recovery gaps land
/// (~16-50 ms): the filter slewed instead, and in the slew regime the clamp
/// zeroes the damping term's input, turning the critically damped spring into
/// a nearly undamped one -- the published anchor oscillated for many seconds
/// and dragged Core Audio's write phase across the engine's fill cursor,
/// heard as minutes of crackle until the stream was restarted. The original
/// kext drew the same line at 10 ms ("USB hick ... timer re-syncing").
const FILTER_RESET_STEPS: f64 = 3.0;

impl TimestampFilter {
    /// `start`: the timestamp the stream is expected to begin at (for the
    /// engine, the scheduled bus time of the first packet). `nominal_step`:
    /// expected time between observations, in the same unit as the timestamps.
    pub fn new(start: u64, nominal_step: u64) -> TimestampFilter {
        let step = nominal_step as f64;
        TimestampFilter {
            x: start as f64,
            dx: step,
            u: 0.0,
            step,
            resets: 0,
        }
    }

    /// Feeds one raw observation, returns the filtered timestamp for it.
    pub fn filter(&mut self, raw: u64) -> u64 {
        let xnext = self.x + self.dx;
        let error = raw as f64 - xnext;

        if error.abs() > FILTER_RESET_STEPS * self.step {
            self.resets += 1;
            self.x = raw as f64;
            self.dx = self.step;
            self.u = 0.0;
            return raw;
        }

        // A single very late observation may only pull with bounded force, so
        // it cannot fling the velocity; a sustained offset still converges.
        let u = error.clamp(-self.step, self.step);
        let force = FILTER_K * u + FILTER_DA * (u - self.u);
        self.dx += force / FILTER_M;
        // The device cannot halve or double its clock; excursions beyond this
        // are filter pathology, not measurement.
        self.dx = self.dx.clamp(0.5 * self.step, 2.0 * self.step);

        self.x = xnext;
        self.u = u;
        (self.x + 0.5) as u64
    }

    /// Moves the prediction to a known discontinuity without forgetting the
    /// rate: after this, an observation of exactly `expected_next` is a zero
    /// error, and `dx` is whatever the stream had taught the filter so far.
    ///
    /// For the engine this is a rebuilt bus schedule after a stall. The gap is
    /// known from the bus frame numbers, so there is nothing for the filter to
    /// discover -- feeding it the first post-gap observation cold would either
    /// snap (a counted reset for an event that was not a surprise) or, worse,
    /// slew through it. `resets` stays what it was: it counts the unplanned.
    pub fn rebase(&mut self, expected_next: u64) {
        self.x = expected_next as f64 - self.dx;
        self.u = 0.0;
    }

    /// How often the filter had to snap to a discontinuity.
    pub fn resets(&self) -> u32 {
        self.resets
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
