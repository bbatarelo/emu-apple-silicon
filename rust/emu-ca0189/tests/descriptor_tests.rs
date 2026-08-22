//! Parser tests against the descriptor the real Tracker Pre reported.
//!
//! Guidelines Milestone 1: "save raw and interpreted descriptor dumps as test
//! fixtures". These run with plain `cargo test` and need no hardware.

use emu_ca0189::descriptor::parse_configuration;
use emu_ca0189::protocol::{extension_code, ControlSetup, RateCode};
use emu_ca0189::ParseError;

/// Captured 2026-08-20 from Tracker Pre 041e:3f0a, bcdDevice 0x0100.
const CONFIG: &[u8] = include_bytes!("../../../captures/descriptors/tracker-pre-config.bin");

#[test]
fn parses_the_captured_configuration() {
    let m = parse_configuration(CONFIG).expect("fixture must parse");

    assert_eq!(CONFIG.len(), 1132);
    assert_eq!(m.configuration_value, 1);
    assert_eq!(m.num_interfaces, 3);
    assert_eq!(m.max_power_ma, 500);
}

#[test]
fn the_tracker_pre_has_no_midi_interface() {
    // It has MIDI ports on the box, but nothing in its descriptors. MIDI
    // support keys off the model, so it must not claim an interface here.
    let m = parse_configuration(CONFIG).unwrap();
    assert_eq!(m.midi_interface, 0xff);
    assert!(!m.has_midi());
}

#[test]
fn finds_the_clock_rate_extension_unit() {
    let m = parse_configuration(CONFIG).unwrap();

    let xu = m
        .clock_rate_unit()
        .expect("Tracker Pre must expose the clock rate extension unit");

    // These two values are what Milestone 2's control transfers depend on.
    assert_eq!(xu.unit_id, 12);
    assert_eq!(xu.extension_code, extension_code::CLOCK_RATE);

    // bmControls 0x07: enable-processing, rate-support and rate-select.
    assert_eq!(xu.controls, 0x07);
}

#[test]
fn control_interface_is_zero() {
    let m = parse_configuration(CONFIG).unwrap();
    assert_eq!(m.control_interface, 0);
    // Interrupt IN endpoint for asynchronous status change notifications.
    assert_eq!(m.status_endpoint, 0x83);
}

#[test]
fn advertises_all_six_sample_rates() {
    let m = parse_configuration(CONFIG).unwrap();

    let mut rates = [0u32; 16];
    let n = m.sample_rates(&mut rates);

    assert_eq!(
        &rates[..n],
        &[44100, 48000, 88200, 96000, 176400, 192000],
        "every rate in the device's own rate-code enum should appear"
    );
}

#[test]
fn playback_interface_has_an_explicit_feedback_endpoint() {
    let m = parse_configuration(CONFIG).unwrap();

    let playback: Vec<_> = m
        .alts()
        .iter()
        .filter(|a| a.interface_number == 1 && a.data_endpoint != 0)
        .collect();

    assert!(!playback.is_empty(), "interface 1 should carry playback alts");

    for alt in &playback {
        assert_eq!(alt.data_endpoint, 0x01, "playback data endpoint is OUT ep1");
        assert!(!alt.is_input());
        assert_eq!(
            alt.feedback_endpoint, 0x81,
            "each playback alt must pair with the feedback IN endpoint"
        );
    }
}

#[test]
fn capture_interface_has_no_feedback_endpoint() {
    let m = parse_configuration(CONFIG).unwrap();

    let capture: Vec<_> = m
        .alts()
        .iter()
        .filter(|a| a.interface_number == 2 && a.data_endpoint != 0)
        .collect();

    assert!(!capture.is_empty(), "interface 2 should carry capture alts");

    for alt in &capture {
        assert_eq!(alt.data_endpoint, 0x82, "capture data endpoint is IN ep2");
        assert!(alt.is_input());
        assert_eq!(alt.feedback_endpoint, 0, "capture is the clock, it needs no feedback");
    }
}

#[test]
fn every_streaming_alt_is_24_bit_stereo() {
    let m = parse_configuration(CONFIG).unwrap();

    for alt in m.alts().iter().filter(|a| a.data_endpoint != 0) {
        assert_eq!(alt.channels, 2, "alt {:?}", alt);
        assert_eq!(alt.subframe_size, 3, "alt {:?}", alt);
        assert_eq!(alt.bit_resolution, 24, "alt {:?}", alt);
        // The bytes-per-frame value the packet planner will depend on.
        assert_eq!(alt.bytes_per_frame(), 6);
    }
}

#[test]
fn zero_bandwidth_alt_zero_exists_on_both_streaming_interfaces() {
    let m = parse_configuration(CONFIG).unwrap();

    for interface in [1u8, 2u8] {
        let alt0 = m
            .alts()
            .iter()
            .find(|a| a.interface_number == interface && a.alternate_setting == 0)
            .unwrap_or_else(|| panic!("interface {interface} must have an alt 0"));

        assert_eq!(alt0.num_endpoints, 0, "alt 0 must be zero-bandwidth");
        assert_eq!(alt0.data_endpoint, 0);
    }
}

#[test]
fn max_packet_size_is_large_enough_for_the_advertised_rate() {
    let m = parse_configuration(CONFIG).unwrap();

    for alt in m.alts().iter().filter(|a| a.data_endpoint != 0) {
        // High speed: bInterval is 2^(n-1) microframes, 8 microframes per ms.
        let microframes_per_service = 1u32 << (alt.interval - 1);
        let services_per_second = 8000 / microframes_per_service;

        let frames_per_service = alt.sample_rate.div_ceil(services_per_second);
        let needed = frames_per_service * alt.bytes_per_frame();

        assert!(
            alt.max_packet_size as u32 >= needed,
            "iface {} alt {}: {} Hz needs >= {} bytes/packet but advertises {}",
            alt.interface_number,
            alt.alternate_setting,
            alt.sample_rate,
            needed,
            alt.max_packet_size
        );
    }
}

// --- protocol encoding ----------------------------------------------------

#[test]
fn clock_rate_request_matches_emu_driver_encoding() {
    // Cross-checked against EMUUSBAudioDevice::getExtensionUnitSetting:
    //   wValue = controlSelector << 8
    //   wIndex = (unitID << 8) | mInterfaceNum
    let get = ControlSetup::get_xu(12, 0, emu_ca0189::selector::CLOCK_RATE, 1);
    assert_eq!(get.bm_request_type, 0xa1);
    assert_eq!(get.b_request, 0x81); // GET_CUR
    assert_eq!(get.w_value, 0x0300);
    assert_eq!(get.w_index, 0x0c00);
    assert_eq!(get.w_length, 1);

    let set = ControlSetup::set_xu(12, 0, emu_ca0189::selector::CLOCK_RATE, 1);
    assert_eq!(set.bm_request_type, 0x21);
    assert_eq!(set.b_request, 0x01); // SET_CUR
    assert_eq!(set.w_value, 0x0300);
    assert_eq!(set.w_index, 0x0c00);
}

#[test]
fn rate_codes_round_trip() {
    for rate in RateCode::ALL {
        assert_eq!(RateCode::from_code(rate as u8), Some(rate));
        assert_eq!(RateCode::from_hz(rate.hz()), Some(rate));
    }
    assert_eq!(RateCode::from_code(6), None);
    assert_eq!(RateCode::from_hz(32000), None);
}

#[test]
fn descriptor_rates_all_map_to_device_rate_codes() {
    let m = parse_configuration(CONFIG).unwrap();

    for alt in m.alts().iter().filter(|a| a.sample_rate != 0) {
        assert!(
            RateCode::from_hz(alt.sample_rate).is_some(),
            "{} Hz appears in descriptors but has no device rate code",
            alt.sample_rate
        );
    }
}

// --- malformed input ------------------------------------------------------

#[test]
fn rejects_truncated_input() {
    assert_eq!(parse_configuration(&[]), Err(ParseError::TooShort));
    assert_eq!(parse_configuration(&CONFIG[..4]), Err(ParseError::TooShort));
}

#[test]
fn rejects_wrong_descriptor_type() {
    let mut bad = CONFIG.to_vec();
    bad[1] = 0x01;
    assert_eq!(parse_configuration(&bad), Err(ParseError::NotAConfiguration));
}

#[test]
fn rejects_total_length_beyond_the_buffer() {
    let mut bad = CONFIG.to_vec();
    bad[2] = 0xff;
    bad[3] = 0xff;
    assert_eq!(parse_configuration(&bad), Err(ParseError::LengthMismatch));
}

#[test]
fn rejects_zero_length_descriptor_rather_than_looping() {
    // A zero bLength mid-chain would make a naive walk spin forever.
    let mut bad = CONFIG.to_vec();
    bad[9] = 0;
    assert_eq!(parse_configuration(&bad), Err(ParseError::BadDescriptorLength));
}

#[test]
fn rejects_descriptor_claiming_to_run_past_the_end() {
    let mut bad = CONFIG.to_vec();
    let last = bad.len() - 7;
    bad[last] = 200;
    assert_eq!(parse_configuration(&bad), Err(ParseError::BadDescriptorLength));
}

#[test]
fn truncating_anywhere_never_panics() {
    // Every prefix must produce a clean Result, never an out-of-bounds panic.
    for n in 0..CONFIG.len() {
        let mut prefix = CONFIG[..n].to_vec();
        if prefix.len() >= 4 {
            let len = prefix.len() as u16;
            prefix[2..4].copy_from_slice(&len.to_le_bytes());
        }
        let _ = parse_configuration(&prefix);
    }
}

// ---------------------------------------------------------------- 0404 USB

/// Captured 2026-08-22 from 0404 USB 041e:3f04, bcdDevice 0x0100. The same
/// CA0189 protocol, but a larger device: four extension units instead of one,
/// four-channel alternate settings alongside the stereo ones, and a
/// MIDI-streaming interface the Tracker Pre does not have.
const CONFIG_0404: &[u8] = include_bytes!("../../../captures/descriptors/0404-usb-config.bin");

#[test]
fn parses_the_0404_configuration() {
    let m = parse_configuration(CONFIG_0404).expect("fixture must parse");

    assert_eq!(CONFIG_0404.len(), 1832);
    assert_eq!(m.configuration_value, 1);
    // Audio control, playback, capture, and MIDI.
    assert_eq!(m.num_interfaces, 4);
    assert_eq!(m.control_interface, 0);
    assert_eq!(m.status_endpoint, 0x83);
}

#[test]
fn the_0404_midi_interface_contributes_no_alt_settings() {
    // Its class-specific descriptors reuse the audio-streaming subtype numbers
    // for jacks and elements. Reading them as audio is what made this fixture
    // fail to parse at all, so the count is the regression guard.
    let m = parse_configuration(CONFIG_0404).unwrap();

    assert_eq!(m.num_alt_settings, 36);
    assert!(
        m.alts().iter().all(|a| a.interface_number == 1 || a.interface_number == 2),
        "only the two audio-streaming interfaces may produce alt settings"
    );
}

#[test]
fn the_0404_midi_interface_is_modelled() {
    // Interface 3: ordinary USB-MIDI 1.0 on bulk endpoints, one virtual cable
    // each way. These five values are everything the MIDI driver needs.
    let m = parse_configuration(CONFIG_0404).unwrap();

    assert!(m.has_midi());
    assert_eq!(m.midi_interface, 3);
    assert_eq!(m.midi_in_endpoint, 0x85);
    assert_eq!(m.midi_out_endpoint, 0x05);
    assert_eq!(m.midi_in_cables, 1);
    assert_eq!(m.midi_out_cables, 1);
}

#[test]
fn the_0404_clock_rate_unit_matches_the_tracker_pre() {
    let m = parse_configuration(CONFIG_0404).unwrap();
    let xu = m.clock_rate_unit().expect("must expose the clock rate unit");

    // Same unit ID and same bmControls, which is why the existing clock code
    // drives this device unchanged.
    assert_eq!(xu.unit_id, 12);
    assert_eq!(xu.extension_code, extension_code::CLOCK_RATE);
    assert_eq!(xu.controls, 0x07);
}

#[test]
fn the_0404_exposes_the_extra_extension_units() {
    let m = parse_configuration(CONFIG_0404).unwrap();

    // Clock source, digital I/O status and device options, none of which the
    // Tracker Pre has. Nothing drives them yet; this records that they exist.
    assert_eq!(m.num_extension_units, 4);
    assert!(m.extension_unit(extension_code::CLOCK_SOURCE).is_some());
    assert!(m.extension_unit(extension_code::DIGITAL_IO_STATUS).is_some());
    assert!(m.extension_unit(extension_code::DEVICE_OPTIONS).is_some());
}

#[test]
fn the_0404_advertises_all_six_sample_rates() {
    let m = parse_configuration(CONFIG_0404).unwrap();

    let mut rates = [0u32; 16];
    let n = m.sample_rates(&mut rates);
    assert_eq!(&rates[..n], &[44100, 48000, 88200, 96000, 176400, 192000]);
}

#[test]
fn every_0404_rate_is_available_in_stereo_both_ways() {
    // The driver's ring is stereo, so it selects alt settings by rate *and*
    // channel count. That only works if a stereo alt exists at every rate in
    // both directions.
    let m = parse_configuration(CONFIG_0404).unwrap();

    for rate in [44100, 48000, 88200, 96000, 176400, 192000] {
        for input in [true, false] {
            assert!(
                m.alts().iter().any(|a| {
                    a.sample_rate == rate && a.channels == 2 && a.is_input() == input
                }),
                "no stereo {} alt setting at {rate} Hz",
                if input { "capture" } else { "playback" }
            );
        }
    }
}

#[test]
fn the_0404_offers_four_channel_alt_settings_too() {
    // Not yet usable -- the ring and the published format are stereo -- but
    // their existence is why rate alone no longer identifies an alt setting.
    let m = parse_configuration(CONFIG_0404).unwrap();

    let quad = m.alts().iter().filter(|a| a.channels == 4).count();
    assert!(quad > 0, "the 0404 advertises four-channel alt settings");
}

#[test]
fn truncating_the_0404_anywhere_never_panics() {
    for n in 0..CONFIG_0404.len() {
        let mut prefix = CONFIG_0404[..n].to_vec();
        if prefix.len() >= 4 {
            let len = prefix.len() as u16;
            prefix[2..4].copy_from_slice(&len.to_le_bytes());
        }
        let _ = parse_configuration(&prefix);
    }
}
