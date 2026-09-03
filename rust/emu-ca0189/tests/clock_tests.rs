//! Validates the feedback and clock model against packet traces captured from
//! the real device.
//!
//! Guidelines Milestone 3: "Use these captures to validate the feedback and
//! clock-estimator model." These run with `cargo test` and need no hardware.

use emu_ca0189::clock::{
    feedback_true_q16, frames_in_packet, output_packet_bytes, ClockEstimator, FeedbackQueue,
};
use emu_ca0189::types::{ByteCount, SampleFrames};

const BYTES_PER_FRAME: u32 = 6; // 2 channels x 3 bytes, every alt setting

/// The device runs a phase-alignment ramp after a stream starts. Observed
/// durations range from roughly 830 ms to 1630 ms, so the window allows for the
/// longer case. Statistics taken across the ramp read hundreds of ppm fast.
const SETTLE_MS: f64 = 2000.0;

struct Trace {
    nominal_hz: u32,
    b_interval: u8,
    /// Actual bytes received per service interval, in order.
    packets: Vec<u32>,
}

impl Trace {
    fn load(nominal_hz: u32) -> Trace {
        Trace::load_named(&format!("tracker-pre-{nominal_hz}"), nominal_hz)
    }

    fn load_named(name: &str, nominal_hz: u32) -> Trace {
        let path = format!(
            "{}/../../captures/packet-traces/{}.csv",
            env!("CARGO_MANIFEST_DIR"),
            name
        );
        let text = std::fs::read_to_string(&path)
            .unwrap_or_else(|e| panic!("cannot read {path}: {e}"));

        let mut b_interval = 0u8;
        let mut packets = Vec::new();

        for line in text.lines() {
            if let Some(rest) = line.strip_prefix("# nominal_rate_hz=") {
                for field in rest.split_whitespace() {
                    if let Some(v) = field.strip_prefix("bInterval=") {
                        b_interval = v.parse().expect("bInterval");
                    }
                }
                continue;
            }
            if line.starts_with('#') || line.starts_with("usb_frame") {
                continue;
            }
            let mut cols = line.split(',');
            let actual = cols.nth(2).expect("actual_bytes column");
            packets.push(actual.parse().expect("actual_bytes"));
        }

        assert!(b_interval > 0, "trace header must record bInterval");
        assert!(!packets.is_empty(), "trace must contain packets");
        Trace { nominal_hz, b_interval, packets }
    }

    fn interval_ms(&self) -> f64 {
        // Period is 2^(bInterval-1) microframes of 125 us.
        (1u32 << (self.b_interval - 1)) as f64 * 0.125
    }

    fn settle_count(&self) -> usize {
        (SETTLE_MS / self.interval_ms()) as usize
    }
}

const ALL_RATES: [u32; 6] = [44100, 48000, 88200, 96000, 176400, 192000];

// --- packet accounting ----------------------------------------------------

#[test]
fn binterval_4_packets_are_whole_sample_frames() {
    for hz in [44100, 48000, 88200, 96000] {
        let trace = Trace::load(hz);
        assert_eq!(trace.b_interval, 4, "{hz} Hz should use a 1 ms service interval");

        for (i, &bytes) in trace.packets.iter().enumerate() {
            if bytes == 0 {
                continue;
            }
            assert_eq!(
                bytes % BYTES_PER_FRAME,
                0,
                "{hz} Hz packet {i}: {bytes} bytes is not a whole number of sample frames"
            );
        }
    }
}

#[test]
fn binterval_3_packets_carry_exactly_four_extra_bytes() {
    // Only 176.4 and 192 kHz exist at bInterval 3, and on those every capture
    // packet is 4 bytes longer than a whole number of sample frames. The bytes
    // are not a header -- the payload starts with aligned 24-bit stereo -- and
    // what they are is still unexplained. A driver that copies `actual` bytes
    // straight into the audio ring would inject 4 bytes of garbage per packet.
    for hz in [176400, 192000] {
        let trace = Trace::load(hz);
        assert_eq!(trace.b_interval, 3);

        for (i, &bytes) in trace.packets.iter().enumerate() {
            if bytes == 0 {
                continue;
            }
            assert_eq!(
                bytes % BYTES_PER_FRAME,
                4,
                "{hz} Hz packet {i}: expected a constant 4-byte excess, got {} from {bytes}",
                bytes % BYTES_PER_FRAME
            );
        }
    }
}

#[test]
fn flooring_per_packet_matters_on_real_data() {
    // Summing bytes and dividing once lets the 4-byte excess accumulate into
    // sample frames that were never sent. On this trace that is the difference
    // between a correct reading and one thousands of ppm fast.
    let trace = Trace::load(176400);

    let total_bytes: u64 = trace.packets.iter().map(|&b| b as u64).sum();
    let naive_frames = total_bytes / BYTES_PER_FRAME as u64;
    let correct_frames: u64 = trace
        .packets
        .iter()
        .map(|&b| frames_in_packet(ByteCount(b), BYTES_PER_FRAME).0 as u64)
        .sum();

    assert!(
        naive_frames > correct_frames,
        "the naive sum should overcount; got naive={naive_frames} correct={correct_frames}"
    );

    let phantom = naive_frames - correct_frames;
    let error_ppm = phantom as f64 / correct_frames as f64 * 1e6;
    assert!(
        error_ppm > 5000.0,
        "the naive error should be large enough to be obvious, was {error_ppm:.0} ppm"
    );
}

// --- clock estimation -----------------------------------------------------

#[test]
fn clock_estimator_converges_to_nominal_on_every_trace() {
    for hz in ALL_RATES {
        let trace = Trace::load(hz);
        let mut estimator = ClockEstimator::new(hz, trace.b_interval);

        for &bytes in trace.packets.iter().skip(trace.settle_count()) {
            estimator.observe(frames_in_packet(ByteCount(bytes), BYTES_PER_FRAME));
        }

        assert!(estimator.intervals() > 1000, "{hz} Hz: too few settled intervals");

        let ppm = estimator.drift_ppm();
        assert!(
            ppm.abs() < 25.0,
            "{hz} Hz: measured {:.2} Hz, {ppm:+.1} ppm off nominal",
            estimator.measured_hz()
        );
    }
}

#[test]
fn service_interval_follows_binterval() {
    // 1 ms at bInterval 4, 0.5 ms at bInterval 3. Getting this wrong reads the
    // high rates back as exactly half their true value.
    assert_eq!(ClockEstimator::new(48000, 4).interval_ns(), 1_000_000);
    assert_eq!(ClockEstimator::new(176400, 3).interval_ns(), 500_000);
}

#[test]
fn measuring_across_the_startup_ramp_is_visibly_wrong() {
    // Guards the reason the settling window exists: including the ramp reports
    // the device meaningfully fast, so anyone who removes the skip sees a
    // failure rather than a quietly biased number.
    let trace = Trace::load_named("tracker-pre-48000-startup-ramp", 48000);

    let mut unsettled = ClockEstimator::new(48000, trace.b_interval);
    for &bytes in trace.packets.iter().take(trace.settle_count()) {
        unsettled.observe(frames_in_packet(ByteCount(bytes), BYTES_PER_FRAME));
    }

    let mut settled = ClockEstimator::new(48000, trace.b_interval);
    for &bytes in trace.packets.iter().skip(trace.settle_count()) {
        settled.observe(frames_in_packet(ByteCount(bytes), BYTES_PER_FRAME));
    }

    assert!(
        unsettled.drift_ppm() > settled.drift_ppm() + 50.0,
        "ramp window {:+.1} ppm should read clearly faster than settled {:+.1} ppm",
        unsettled.drift_ppm(),
        settled.drift_ppm()
    );
}

#[test]
fn phase_alignment_ramp_corrects_every_tenth_interval() {
    // At 48 kHz the settled packet size is a constant 48 sample frames. During
    // the startup ramp the device inserts one extra frame on a strict
    // ten-interval cadence until phase is recovered.
    //
    // The ramp appears on roughly one stream start in four, so it gets its own
    // fixture rather than being asserted against the ordinary traces -- most of
    // which contain no ramp at all.
    let trace = Trace::load_named("tracker-pre-48000-startup-ramp", 48000);

    let corrections: Vec<usize> = trace.packets[..trace.settle_count()]
        .iter()
        .enumerate()
        .filter(|(_, &b)| b > 0 && frames_in_packet(ByteCount(b), BYTES_PER_FRAME).0 > 48)
        .map(|(i, _)| i)
        .collect();

    assert!(
        corrections.len() > 20,
        "expected a visible ramp, found {} corrections",
        corrections.len()
    );

    let spacings: Vec<usize> = corrections.windows(2).map(|w| w[1] - w[0]).collect();
    let tens = spacings.iter().filter(|&&s| s == 10).count();
    assert!(
        tens * 10 >= spacings.len() * 9,
        "expected a ten-interval cadence; {tens} of {} spacings were 10",
        spacings.len()
    );
}

#[test]
fn settled_integer_rates_hold_exactly_nominal() {
    // 48 kHz family rates deliver a whole number of frames per interval, so
    // once settled every packet is identical. Any variation there is a real
    // event, not clock distribution.
    for (hz, frames) in [(48000u32, 48u32), (96000, 96), (192000, 96)] {
        let trace = Trace::load(hz);
        let settled: Vec<u32> = trace
            .packets
            .iter()
            .skip(trace.settle_count())
            .map(|&b| frames_in_packet(ByteCount(b), BYTES_PER_FRAME).0)
            .collect();

        let off = settled.iter().filter(|&&f| f != frames).count();
        assert_eq!(
            off, 0,
            "{hz} Hz: {off} of {} settled packets were not {frames} frames",
            settled.len()
        );
    }
}

#[test]
fn fractional_rates_distribute_two_packet_sizes() {
    // 44.1 kHz family rates cannot deliver a whole number of frames per
    // interval, so the device alternates between two sizes in the ratio that
    // averages to nominal.
    for (hz, low, want_high_fraction) in
        [(44100u32, 44u32, 0.10f64), (88200, 88, 0.20), (176400, 88, 0.20)]
    {
        let trace = Trace::load(hz);
        let settled: Vec<u32> = trace
            .packets
            .iter()
            .skip(trace.settle_count())
            .map(|&b| frames_in_packet(ByteCount(b), BYTES_PER_FRAME).0)
            .collect();

        let high = settled.iter().filter(|&&f| f == low + 1).count();
        let recognised = settled.iter().filter(|&&f| f == low || f == low + 1).count();
        assert_eq!(recognised, settled.len(), "{hz} Hz: unexpected packet sizes");

        let fraction = high as f64 / settled.len() as f64;
        assert!(
            (fraction - want_high_fraction).abs() < 0.01,
            "{hz} Hz: {:.1}% were {} frames, expected about {:.0}%",
            fraction * 100.0,
            low + 1,
            want_high_fraction * 100.0
        );
    }
}

// --- feedback queue -------------------------------------------------------

#[test]
fn feedback_queue_is_fifo_and_bounded() {
    let mut queue: FeedbackQueue<4> = FeedbackQueue::new();
    assert!(queue.is_empty());
    assert_eq!(queue.capacity(), 4);

    for frames in [44u32, 45, 44, 44] {
        queue.push(SampleFrames(frames));
    }
    assert_eq!(queue.len(), 4);
    assert_eq!(queue.depth_percent(), 100);

    // Full: further pushes are counted, not silently dropped.
    queue.push(SampleFrames(45));
    assert_eq!(queue.overflows, 1);
    assert_eq!(queue.len(), 4);

    assert_eq!(queue.pop(), Some(SampleFrames(44)));
    assert_eq!(queue.pop(), Some(SampleFrames(45)));
    assert_eq!(queue.pop(), Some(SampleFrames(44)));
    assert_eq!(queue.pop(), Some(SampleFrames(44)));
    assert_eq!(queue.pop(), None);
    assert!(queue.is_empty());
}

#[test]
fn feedback_queue_wraps_without_losing_order() {
    let mut queue: FeedbackQueue<3> = FeedbackQueue::new();
    for round in 0..50u32 {
        queue.push(SampleFrames(round));
        assert_eq!(queue.pop(), Some(SampleFrames(round)));
    }
    assert_eq!(queue.overflows, 0);
    assert!(queue.is_empty());
}

#[test]
fn capture_frame_counts_drive_playback_requests() {
    // The immediate planner from guidelines section 17: sample frames observed
    // on capture become the next playback request, converted through the
    // *output* direction's bytes per frame.
    let trace = Trace::load(44100);
    let mut queue: FeedbackQueue<64> = FeedbackQueue::new();

    let mut requested_bytes = 0u64;
    let mut planned_frames = 0u64;

    for &bytes in trace.packets.iter().skip(trace.settle_count()).take(1000) {
        queue.push(frames_in_packet(ByteCount(bytes), BYTES_PER_FRAME));

        if let Some(frames) = queue.pop() {
            let out = output_packet_bytes(frames, BYTES_PER_FRAME);
            assert_eq!(out.0 % BYTES_PER_FRAME, 0, "output request must be whole frames");
            requested_bytes += out.0 as u64;
            planned_frames += frames.0 as u64;
        }
    }

    assert_eq!(queue.overflows, 0);
    // Playback follows capture exactly: that is the property the whole
    // mechanism exists to provide.
    assert_eq!(requested_bytes, planned_frames * BYTES_PER_FRAME as u64);

    let mean = planned_frames as f64 / 1000.0;
    assert!(
        (mean - 44.1).abs() < 0.05,
        "planner averaged {mean:.3} frames per interval, expected about 44.1"
    );
}

// --- unit-level ------------------------------------------------------------

#[test]
fn frames_in_packet_floors_and_survives_nonsense() {
    assert_eq!(frames_in_packet(ByteCount(264), 6), SampleFrames(44));
    assert_eq!(frames_in_packet(ByteCount(532), 6), SampleFrames(88)); // 4 byte excess
    assert_eq!(frames_in_packet(ByteCount(0), 6), SampleFrames(0));
    assert_eq!(frames_in_packet(ByteCount(5), 6), SampleFrames(0));
    // A zero format must not divide by zero.
    assert_eq!(frames_in_packet(ByteCount(264), 0), SampleFrames(0));
}

#[test]
fn output_packet_bytes_uses_the_output_format() {
    assert_eq!(output_packet_bytes(SampleFrames(45), 6), ByteCount(270));
    // Capture and playback share 6 bytes per frame on this device, which is
    // exactly why a direction mix-up would go unnoticed here. An 8-byte output
    // format must produce a different answer for the same frame count.
    assert_eq!(output_packet_bytes(SampleFrames(45), 8), ByteCount(360));
}

// --- fractional nominal ----------------------------------------------------

#[test]
fn nominal_rate_averages_exactly_on_fractional_rates() {
    use emu_ca0189::clock::NominalRate;

    // 176.4 kHz over a 0.5 ms interval is 88.2 frames. Truncating to 88 is
    // 2268 ppm slow every time the fallback is used.
    let mut nominal = NominalRate::new(176_400, 500_000);
    assert_eq!(nominal.floor(), 88);

    let n = 10_000;
    let total: u64 = (0..n).map(|_| nominal.next().0 as u64).sum();
    let mean = total as f64 / n as f64;

    assert!(
        (mean - 88.2).abs() < 0.001,
        "expected 88.2 frames per interval on average, got {mean}"
    );
}

#[test]
fn nominal_rate_only_ever_yields_floor_or_ceiling() {
    use emu_ca0189::clock::NominalRate;

    let mut nominal = NominalRate::new(176_400, 500_000);
    for _ in 0..1000 {
        let f = nominal.next().0;
        assert!(f == 88 || f == 89, "unexpected packet size {f}");
    }
}

#[test]
fn nominal_rate_is_exact_on_integer_rates() {
    use emu_ca0189::clock::NominalRate;

    // The 48 kHz family divides evenly, so every interval must be identical --
    // no dithering where none is called for.
    for (rate, interval_ns, expect) in
        [(48_000u32, 1_000_000u64, 48u32), (96_000, 1_000_000, 96), (192_000, 500_000, 96)]
    {
        let mut nominal = NominalRate::new(rate, interval_ns);
        for _ in 0..500 {
            assert_eq!(nominal.next().0, expect, "{rate} Hz should be constant");
        }
    }
}

// --- timestamp filter -------------------------------------------------------

/// 2 ms in nanoseconds: the engine's request cadence, the filter's natural step.
const STEP: u64 = 2_000_000;

/// Deterministic stand-in for completion jitter: a fixed pattern of scheduling
/// noise, mostly small with occasional millisecond-scale spikes, matching what
/// the kext documented (0.01 ms USB jitter, rare 1-3 ms callback pulses).
fn jitter(n: u64) -> i64 {
    match n % 17 {
        0 => 1_400_000, // a late callback
        5 => 300_000,
        11 => -80_000,
        _ => ((n * 7919) % 20_000) as i64 - 10_000,
    }
}

#[test]
fn timestamp_filter_tracks_a_clean_clock_exactly() {
    use emu_ca0189::clock::TimestampFilter;

    let start = 1_000_000_000u64;
    let mut f = TimestampFilter::new(start, STEP);
    for n in 1..=5000u64 {
        let out = f.filter(start + n * STEP);
        let err = out as i64 - (start + n * STEP) as i64;
        assert!(err.abs() < 1_000, "step {n}: filtered {err} ns from truth");
    }
    assert_eq!(f.resets(), 0);
}

#[test]
fn timestamp_filter_suppresses_scheduling_jitter() {
    use emu_ca0189::clock::TimestampFilter;

    let start = 1_000_000_000u64;
    let mut f = TimestampFilter::new(start, STEP);
    let mut worst = 0i64;
    for n in 1..=5000u64 {
        let raw = (start + n * STEP) as i64 + jitter(n);
        let out = f.filter(raw as u64);
        let err = out as i64 - (start + n * STEP) as i64;
        if n > 500 {
            worst = worst.max(err.abs());
        }
    }
    // Raw observations are up to 1.4 ms off the true clock; the filtered
    // timeline must stay an order of magnitude closer.
    assert!(worst < 140_000, "worst filtered error {worst} ns");
    assert_eq!(f.resets(), 0);
}

#[test]
fn timestamp_filter_converges_to_an_offset_rate() {
    use emu_ca0189::clock::TimestampFilter;

    // A device clock 100 ppm fast. The kext measured ~30 ppm on real hardware;
    // 100 ppm is comfortably beyond anything a working crystal produces.
    let start = 1_000_000_000u64;
    let true_step = STEP + STEP / 10_000;
    let mut f = TimestampFilter::new(start, STEP);
    let mut last_in = 0i64;
    let mut last_out = 0i64;
    let (mut sum_err, mut count) = (0i64, 0i64);
    for n in 1..=20_000u64 {
        let raw = start + n * true_step;
        let out = f.filter(raw) as i64;
        if n > 10_000 {
            sum_err += out - raw as i64;
            count += 1;
        }
        last_in = raw as i64;
        last_out = out;
    }
    // Converged: no growing lag, and the filtered clock sits on the true one.
    assert!((last_out - last_in).abs() < 30_000, "lag {} ns", last_out - last_in);
    assert!((sum_err / count).abs() < 30_000, "mean error {} ns", sum_err / count);
    assert_eq!(f.resets(), 0);
}

#[test]
fn timestamp_filter_snaps_on_discontinuity() {
    use emu_ca0189::clock::TimestampFilter;

    let start = 1_000_000_000u64;
    let mut f = TimestampFilter::new(start, STEP);
    for n in 1..=100u64 {
        f.filter(start + n * STEP);
    }
    // A 200 ms hole, as a resync after a stall would produce: snap, not slew.
    let jumped = start + 100 * STEP + 200_000_000;
    let out = f.filter(jumped);
    assert_eq!(out, jumped);
    assert_eq!(f.resets(), 1);
    // And the filter keeps tracking cleanly from the new position.
    for n in 1..=100u64 {
        let raw = jumped + n * STEP;
        let err = f.filter(raw) as i64 - raw as i64;
        assert!(err.abs() < 1_000, "post-reset step {n}: {err} ns off");
    }
}

#[test]
fn timestamp_filter_output_is_monotonic() {
    use emu_ca0189::clock::TimestampFilter;

    // Even fed hostile input -- stuck timestamps, bursts of catch-up -- the
    // filtered timeline must never run backwards; Core Audio treats a
    // regressing anchor as a fault.
    let start = 1_000_000_000u64;
    let mut f = TimestampFilter::new(start, STEP);
    let mut prev = 0u64;
    for n in 1..=2000u64 {
        let raw = if n % 13 < 3 {
            start + (n - n % 13) * STEP // a stalled observer: repeated values
        } else {
            start + n * STEP
        };
        let out = f.filter(raw);
        assert!(out >= prev, "step {n}: {out} < {prev}");
        prev = out;
    }
}

#[test]
fn timestamp_filter_snaps_on_a_stall_sized_gap() {
    use emu_ca0189::clock::TimestampFilter;

    // A 20 ms hole -- the size a stall-plus-resync actually leaves, and the
    // range an earlier 50 ms threshold slewed through, which oscillated for
    // seconds and crackled until stream restart. Must snap, first observation.
    let start = 1_000_000_000u64;
    let mut f = TimestampFilter::new(start, STEP);
    for n in 1..=200u64 {
        f.filter(start + n * STEP);
    }
    let jumped = start + 200 * STEP + 20_000_000;
    assert_eq!(f.filter(jumped), jumped);
    assert_eq!(f.resets(), 1);
    for n in 1..=200u64 {
        let raw = jumped + n * STEP;
        let err = f.filter(raw) as i64 - raw as i64;
        assert!(err.abs() < 1_000, "post-snap step {n}: {err} ns off");
    }
}

#[test]
fn timestamp_filter_rebase_crosses_a_known_gap_without_resetting() {
    use emu_ca0189::clock::TimestampFilter;

    // A device 300 ppm fast, long enough for the filter to have learned it.
    const STEP: u64 = 48_000;
    let true_step = STEP as f64 * (1.0 - 300e-6);
    let start = 1_000_000u64;
    let mut f = TimestampFilter::new(start, STEP);
    let mut t = start as f64;
    let mut n = 0u64;
    let mut last_out = 0u64;
    while n < 4000 {
        n += 1;
        t += true_step;
        last_out = f.filter(t.round() as u64);
    }
    assert_eq!(f.resets(), 0);
    // Learned: output tracks the fast clock, not the nominal one.
    let nominal_here = start as f64 + n as f64 * STEP as f64;
    assert!((nominal_here - last_out as f64) > 100.0 * STEP as f64 * 300e-6);

    // A 30-step hole in the observations (a stall and a rebuilt schedule),
    // announced to the filter: the next observation lands exactly where the
    // caller said it would.
    t += 30.0 * true_step;
    let expected_next = t.round() as u64;
    f.rebase(expected_next);
    let out = f.filter(expected_next);
    assert_eq!(f.resets(), 0, "a rebase must not count as a reset");
    assert_eq!(out, expected_next);

    // And the learned rate is intact: a further run stays within a few ticks
    // of the raw clock, which a filter re-seeded with the nominal step would
    // take hundreds of observations to manage.
    for _ in 0..50 {
        t += true_step;
        let raw = t.round() as u64;
        let out = f.filter(raw);
        let err = (out as f64 - raw as f64).abs();
        assert!(err < 4.0, "drifted {err} ticks from raw after rebase");
    }
    assert_eq!(f.resets(), 0);
}

/// Every rate and service interval the device publishes, with the raw feedback
/// word read from the hardware at each. Corrected, all ten must land on the
/// nominal frames-per-interval they claim to describe.
///
/// The 48 kHz family has no fraction and must come back untouched; the 44.1
/// family is the whole point, being 53.1 ppm low as sent.
#[test]
fn corrects_the_feedback_scaling_at_every_published_rate() {
    // (rate, interval in microframes, raw word, nominal frames per interval)
    let rows: &[(u32, u32, u32, f64)] = &[
        (44_100, 8, 0x002c_1900, 44.1),
        (44_100, 4, 0x0016_0c80, 22.05),
        (48_000, 8, 0x0030_0000, 48.0),
        (48_000, 4, 0x0018_0000, 24.0),
        (88_200, 8, 0x0058_3200, 88.2),
        (88_200, 4, 0x002c_1900, 44.1),
        (96_000, 8, 0x0060_0000, 96.0),
        (96_000, 4, 0x0030_0000, 48.0),
        (176_400, 4, 0x0058_3200, 88.2),
        (192_000, 4, 0x0060_0000, 96.0),
    ];

    for &(rate, microframes, raw, nominal) in rows {
        let corrected = feedback_true_q16(raw) as f64 / 65536.0;
        let error_ppm = (corrected - nominal) / nominal * 1e6;
        assert!(
            error_ppm.abs() < 1.0,
            "{rate} Hz on {microframes} microframes: {raw:#010x} corrected to \
             {corrected} frames, wanted {nominal} ({error_ppm:.1} ppm off)"
        );

        // And the uncorrected value is wrong by the documented amount, so a
        // regression that stopped correcting could not pass quietly.
        let raw_ppm = (raw as f64 / 65536.0 - nominal) / nominal * 1e6;
        if raw & 0xffff == 0 {
            assert_eq!(feedback_true_q16(raw), raw, "integer rates must pass through");
        } else {
            assert!(
                (raw_ppm + 53.1).abs() < 0.5,
                "{rate} Hz: raw word should read -53.1 ppm, read {raw_ppm:.1}"
            );
        }
    }
}

/// The device's deviation from nominal has to survive the correction --
/// otherwise the planner would follow a constant and not a servo.
///
/// It also fixes the size of that deviation, which the raw word misstates. The
/// hardware's excursion at 192 kHz is one raw step of 0x1000, which reads as
/// 1/16 of a frame and is really 4096/64000 = 0.064 frames. Anything quoting
/// 0.0625 is quoting the firmware's arithmetic rather than the device.
#[test]
fn the_correction_preserves_the_devices_own_deviation() {
    let nominal = 0x0060_0000u32; // 96.0000 at 192 kHz, no fraction
    let low = nominal - 0x1000; // reads as 95.9375; means 95.9600

    let corrected = feedback_true_q16(low) as f64 / 65536.0;
    assert!(
        (corrected - 95.96).abs() < 0.001,
        "one raw step should be 0.064 frames, got {corrected}"
    );
    assert!(corrected < 96.0, "an excursion below nominal must stay below");
    assert_eq!(feedback_true_q16(nominal), nominal, "nominal must not move");
}

/// A fraction the model cannot have produced is passed through rather than
/// scaled into something larger and wronger.
#[test]
fn refuses_to_correct_what_the_model_cannot_explain() {
    let impossible = 0x0060_0000 | 0xfa00; // fraction at the scale itself
    assert_eq!(feedback_true_q16(impossible), impossible);
}
