//! USB-MIDI 1.0 event packet codec tests.
//!
//! The encoder's input contract is "any MIDI byte stream", which is wider than
//! it sounds: CoreMIDI hands drivers packets that may use running status, and
//! SysEx transfers arrive in arbitrary slices. These tests pin the packets
//! byte-for-byte against the USB-MIDI 1.0 spec's own examples.

use emu_ca0189::midi::{decode_packet, packet_cable, PacketEncoder};

/// Feeds a byte stream and collects every packet it produces.
fn encode(cable: u8, stream: &[u8]) -> Vec<[u8; 4]> {
    let mut enc = PacketEncoder::new(cable);
    stream.iter().filter_map(|&b| enc.feed(b)).collect()
}

#[test]
fn note_on_and_off() {
    assert_eq!(
        encode(0, &[0x90, 0x3c, 0x40, 0x80, 0x3c, 0x00]),
        [[0x09, 0x90, 0x3c, 0x40], [0x08, 0x80, 0x3c, 0x00]]
    );
}

#[test]
fn the_cable_number_lands_in_the_high_nibble() {
    assert_eq!(encode(5, &[0x90, 0x3c, 0x40]), [[0x59, 0x90, 0x3c, 0x40]]);
    assert_eq!(packet_cable([0x59, 0x90, 0x3c, 0x40]), 5);
}

#[test]
fn running_status_reuses_the_last_status_byte() {
    // Three note-ons, the status byte sent once. Each packet must carry the
    // status explicitly -- USB-MIDI has no running status on the wire.
    assert_eq!(
        encode(0, &[0x90, 0x3c, 0x40, 0x3e, 0x40, 0x40, 0x40]),
        [
            [0x09, 0x90, 0x3c, 0x40],
            [0x09, 0x90, 0x3e, 0x40],
            [0x09, 0x90, 0x40, 0x40],
        ]
    );
}

#[test]
fn two_byte_channel_messages() {
    // Program change and channel pressure take one data byte, CIN C and D.
    assert_eq!(
        encode(0, &[0xc2, 0x14, 0xd3, 0x22]),
        [[0x0c, 0xc2, 0x14, 0x00], [0x0d, 0xd3, 0x22, 0x00]]
    );
}

#[test]
fn system_common_messages() {
    // MTC quarter frame (2 bytes), song position (3), song select (2), tune
    // request (1).
    assert_eq!(
        encode(0, &[0xf1, 0x15, 0xf2, 0x00, 0x40, 0xf3, 0x02, 0xf6]),
        [
            [0x02, 0xf1, 0x15, 0x00],
            [0x03, 0xf2, 0x00, 0x40],
            [0x02, 0xf3, 0x02, 0x00],
            [0x05, 0xf6, 0x00, 0x00],
        ]
    );
}

#[test]
fn system_common_does_not_establish_running_status() {
    // A data byte after a completed song select has no status to attach to.
    assert_eq!(encode(0, &[0xf3, 0x02, 0x03]), [[0x02, 0xf3, 0x02, 0x00]]);
}

#[test]
fn realtime_messages_pass_through_alone() {
    assert_eq!(
        encode(0, &[0xf8, 0xfa, 0xfc, 0xfe, 0xff]),
        [
            [0x0f, 0xf8, 0x00, 0x00],
            [0x0f, 0xfa, 0x00, 0x00],
            [0x0f, 0xfc, 0x00, 0x00],
            [0x0f, 0xfe, 0x00, 0x00],
            [0x0f, 0xff, 0x00, 0x00],
        ]
    );
}

#[test]
fn realtime_interleaved_mid_message_does_not_break_it() {
    // Clock arrives between a note-on's data bytes; the note must survive.
    assert_eq!(
        encode(0, &[0x90, 0x3c, 0xf8, 0x40]),
        [[0x0f, 0xf8, 0x00, 0x00], [0x09, 0x90, 0x3c, 0x40]]
    );
}

// SysEx lengths hit all three terminating CINs. The F0 and F7 both travel in
// the packet payload, which is the part of the encoding easiest to get wrong.

#[test]
fn sysex_whose_tail_is_f7_alone() {
    // F0 41 10 42 F7: one full packet, then the last byte with its F7.
    assert_eq!(
        encode(0, &[0xf0, 0x41, 0x10, 0x42, 0xf7]),
        [[0x04, 0xf0, 0x41, 0x10], [0x06, 0x42, 0xf7, 0x00]]
    );
}

#[test]
fn shortest_possible_sysex() {
    // F0 F7 -- degenerate but legal.
    assert_eq!(encode(0, &[0xf0, 0xf7]), [[0x06, 0xf0, 0xf7, 0x00]]);
}

#[test]
fn sysex_ending_on_a_packet_boundary() {
    // F0 41 10 F7: F0+2 bytes fill a packet exactly, F7 follows alone.
    assert_eq!(
        encode(0, &[0xf0, 0x41, 0x10, 0xf7]),
        [[0x04, 0xf0, 0x41, 0x10], [0x05, 0xf7, 0x00, 0x00]]
    );
}

#[test]
fn sysex_with_two_bytes_in_the_last_packet() {
    assert_eq!(
        encode(0, &[0xf0, 0x41, 0x10, 0x42, 0x12, 0xf7]),
        [[0x04, 0xf0, 0x41, 0x10], [0x07, 0x42, 0x12, 0xf7]]
    );
}

#[test]
fn realtime_inside_sysex_leaves_the_transfer_intact() {
    assert_eq!(
        encode(0, &[0xf0, 0x41, 0xf8, 0x10, 0x42, 0xf7]),
        [
            [0x0f, 0xf8, 0x00, 0x00],
            [0x04, 0xf0, 0x41, 0x10],
            [0x06, 0x42, 0xf7, 0x00]
        ]
    );
}

#[test]
fn a_status_byte_abandons_an_unterminated_sysex() {
    // The first three bytes filled a packet and were already sent -- nothing
    // can retract them. The dangling 42 has no valid encoding and drops; the
    // new message must still come through.
    assert_eq!(
        encode(0, &[0xf0, 0x41, 0x10, 0x42, 0x90, 0x3c, 0x40]),
        [[0x04, 0xf0, 0x41, 0x10], [0x09, 0x90, 0x3c, 0x40]]
    );
}

#[test]
fn stray_data_bytes_are_dropped() {
    assert_eq!(encode(0, &[0x01, 0x02, 0x03]), Vec::<[u8; 4]>::new());
    // ...including after an abandoned SysEx killed the running status.
    assert_eq!(encode(0, &[0xf0, 0x41, 0x90, 0x3c]), Vec::<[u8; 4]>::new());
}

#[test]
fn undefined_status_bytes_are_dropped() {
    assert_eq!(encode(0, &[0xf4, 0xf5, 0xf7]), Vec::<[u8; 4]>::new());
}

// ---------------------------------------------------------------- decoding

#[test]
fn decode_gives_back_the_bytes_the_cin_promises() {
    assert_eq!(decode_packet([0x09, 0x90, 0x3c, 0x40]), ([0x90, 0x3c, 0x40], 3));
    assert_eq!(decode_packet([0x0c, 0xc2, 0x14, 0x00]), ([0xc2, 0x14, 0x00], 2));
    assert_eq!(decode_packet([0x0f, 0xf8, 0x00, 0x00]), ([0xf8, 0x00, 0x00], 1));
}

#[test]
fn reserved_cins_decode_to_nothing() {
    assert_eq!(decode_packet([0x00, 0x90, 0x3c, 0x40]).1, 0);
    assert_eq!(decode_packet([0x01, 0x90, 0x3c, 0x40]).1, 0);
}

#[test]
fn every_encoded_packet_decodes_back_to_the_original_stream() {
    // A stream exercising every message class, without running status so the
    // byte streams should match exactly.
    let stream: &[u8] = &[
        0x90, 0x3c, 0x40, 0xb0, 0x07, 0x64, 0xc2, 0x14, 0xe0, 0x00, 0x40, 0xf1,
        0x15, 0xf6, 0xf8, 0xf0, 0x41, 0x10, 0x42, 0x12, 0x34, 0xf7, 0x80, 0x3c,
        0x00,
    ];
    let mut out = Vec::new();
    for packet in encode(0, stream) {
        let (bytes, n) = decode_packet(packet);
        out.extend_from_slice(&bytes[..n as usize]);
    }
    assert_eq!(out, stream);
}
