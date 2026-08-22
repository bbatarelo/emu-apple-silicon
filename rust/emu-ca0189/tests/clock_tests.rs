//! Validates the feedback and clock model against packet traces captured from
//! the real device.
//!
//! Guidelines Milestone 3: "Use these captures to validate the feedback and
//! clock-estimator model." These run with `cargo test` and need no hardware.

use emu_ca0189::clock::{frames_in_packet, output_packet_bytes, ClockEstimator, FeedbackQueue};
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
