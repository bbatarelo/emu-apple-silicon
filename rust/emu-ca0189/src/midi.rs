//! USB-MIDI 1.0 event packets.
//!
//! On the wire every MIDI message travels as a 4-byte event packet: a header
//! byte carrying the virtual cable number and a Code Index Number, then three
//! MIDI bytes, zero-padded. The 0404 USB's MIDI interface is a plain USB-MIDI
//! 1.0 implementation of this, one virtual cable in each direction.
//!
//! The encoder is a byte-at-a-time state machine rather than a
//! message-at-a-time function because its input is an arbitrary MIDI byte
//! stream: CoreMIDI hands the driver packets that may use running status, and
//! a SysEx transfer can span any number of calls. The decoder is stateless --
//! each event packet is self-describing.

/// How many MIDI bytes each Code Index Number carries. CINs 0x0 and 0x1 are
/// reserved for future use ("miscellaneous" and "cable events"); nothing emits
/// them and the decoder skips them.
const CIN_BYTES: [u8; 16] = [0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1];

/// SysEx start-or-continue: three payload bytes, more to follow.
const CIN_SYSEX_CONTINUE: u8 = 0x4;
/// One-byte message: SysEx ending with F7 alone, or Tune Request.
const CIN_ONE_BYTE: u8 = 0x5;
const CIN_SYSEX_END_2: u8 = 0x6;
const CIN_SYSEX_END_3: u8 = 0x7;
/// Two- and three-byte system common messages.
const CIN_SYSCOM_2: u8 = 0x2;
const CIN_SYSCOM_3: u8 = 0x3;
/// Single-byte real-time message.
const CIN_REALTIME: u8 = 0xf;

/// Number of data bytes a status byte demands, or None for status bytes that
/// stand alone or are undefined (F4, F5).
fn data_bytes_for(status: u8) -> Option<u8> {
    match status {
        0x80..=0xbf | 0xe0..=0xef => Some(2),
        0xc0..=0xdf => Some(1),
        0xf1 | 0xf3 => Some(1),
        0xf2 => Some(2),
        _ => None,
    }
}

/// MIDI byte stream to USB-MIDI event packets, one virtual cable.
#[derive(Clone, Copy, Debug)]
pub struct PacketEncoder {
    cable: u8,
    /// Active status byte, kept across messages so running status works.
    /// 0 when no status is active.
    status: u8,
    data: [u8; 2],
    have: u8,
    in_sysex: bool,
    /// SysEx payload waiting to fill a packet, F0 and F7 included.
    sysex: [u8; 3],
    sysex_have: u8,
}

impl PacketEncoder {
    pub fn new(cable: u8) -> PacketEncoder {
        PacketEncoder {
            cable: cable & 0x0f,
            status: 0,
            data: [0; 2],
            have: 0,
            in_sysex: false,
            sysex: [0; 3],
            sysex_have: 0,
        }
    }

    fn packet(&self, cin: u8, bytes: [u8; 3]) -> [u8; 4] {
        [(self.cable << 4) | cin, bytes[0], bytes[1], bytes[2]]
    }

    /// Feeds one byte of the MIDI stream. Returns the event packet it
    /// completes, if it completes one.
    pub fn feed(&mut self, byte: u8) -> Option<[u8; 4]> {
        // Real-time messages may be interleaved anywhere, even mid-SysEx, and
        // must not disturb whatever else is in flight.
        if byte >= 0xf8 {
            return Some(self.packet(CIN_REALTIME, [byte, 0, 0]));
        }

        if byte == 0xf0 {
            // A second F0 while one transfer is open abandons the first; there
            // is no way to terminate it validly without its F7.
            self.in_sysex = true;
            self.sysex = [0xf0, 0, 0];
            self.sysex_have = 1;
            self.status = 0;
            return None;
        }

        if self.in_sysex {
            if byte == 0xf7 {
                self.in_sysex = false;
                let (cin, bytes) = match self.sysex_have {
                    0 => (CIN_ONE_BYTE, [0xf7, 0, 0]),
                    1 => (CIN_SYSEX_END_2, [self.sysex[0], 0xf7, 0]),
                    _ => (CIN_SYSEX_END_3, [self.sysex[0], self.sysex[1], 0xf7]),
                };
                self.sysex_have = 0;
                return Some(self.packet(cin, bytes));
            }
            if byte < 0x80 {
                self.sysex[self.sysex_have as usize] = byte;
                self.sysex_have += 1;
                if self.sysex_have == 3 {
                    let bytes = self.sysex;
                    self.sysex_have = 0;
                    return Some(self.packet(CIN_SYSEX_CONTINUE, bytes));
                }
                return None;
            }
            // Any other status byte ends the transfer without termination.
            // The unterminated remainder cannot be represented, so it drops.
            self.in_sysex = false;
            self.sysex_have = 0;
            // Fall through to normal status handling.
        }

        if byte >= 0x80 {
            match data_bytes_for(byte) {
                Some(_) => {
                    self.status = byte;
                    self.have = 0;
                }
                None => {
                    self.status = 0;
                    if byte == 0xf6 {
                        return Some(self.packet(CIN_ONE_BYTE, [0xf6, 0, 0]));
                    }
                    // F4, F5, stray F7: undefined or unpaired, dropped.
                }
            }
            return None;
        }

        // Data byte. Meaningless without an active status (e.g. after a
        // dropped unterminated SysEx); those bytes fall on the floor.
        let status = self.status;
        if status == 0 {
            return None;
        }
        let needed = data_bytes_for(status).unwrap_or(0);
        self.data[self.have as usize] = byte;
        self.have += 1;
        if self.have < needed {
            return None;
        }
        self.have = 0;

        let packet = match (status, needed) {
            (0xf1 | 0xf3, _) => self.packet(CIN_SYSCOM_2, [status, self.data[0], 0]),
            (0xf2, _) => self.packet(CIN_SYSCOM_3, [status, self.data[0], self.data[1]]),
            (_, 1) => self.packet(status >> 4, [status, self.data[0], 0]),
            (_, _) => self.packet(status >> 4, [status, self.data[0], self.data[1]]),
        };
        // System common messages do not establish running status; channel
        // messages keep theirs so the next data byte continues the run.
        if status >= 0xf0 {
            self.status = 0;
        }
        Some(packet)
    }
}

/// Virtual cable number an event packet arrived on.
pub fn packet_cable(packet: [u8; 4]) -> u8 {
    packet[0] >> 4
}

/// The MIDI bytes inside one event packet. Empty for the reserved CINs.
pub fn decode_packet(packet: [u8; 4]) -> ([u8; 3], u8) {
    let n = CIN_BYTES[(packet[0] & 0x0f) as usize];
    ([packet[1], packet[2], packet[3]], n)
}
