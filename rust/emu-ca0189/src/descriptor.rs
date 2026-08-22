//! USB configuration descriptor parser.
//!
//! Descriptors are externally supplied bytes, so everything here is parsed
//! explicitly from slices with bounds checks. Nothing is transmuted from a
//! pointer, and inconsistent descriptors are rejected rather than tolerated
//! (guidelines section 10.2).
//!
//! The output is a fixed-size validated model. No allocation, so the same code
//! is usable on the dext's configuration path.

use crate::protocol::extension_code;

pub const MAX_ALT_SETTINGS: usize = 48;
pub const MAX_EXTENSION_UNITS: usize = 8;

// Standard descriptor types.
const DT_CONFIGURATION: u8 = 0x02;
const DT_INTERFACE: u8 = 0x04;
const DT_ENDPOINT: u8 = 0x05;
const DT_INTERFACE_ASSOCIATION: u8 = 0x0b;
const DT_CS_INTERFACE: u8 = 0x24;
const DT_CS_ENDPOINT: u8 = 0x25;

// Audio-control class-specific subtypes.
const AC_HEADER: u8 = 0x01;
const AC_INPUT_TERMINAL: u8 = 0x02;
const AC_OUTPUT_TERMINAL: u8 = 0x03;
const AC_FEATURE_UNIT: u8 = 0x06;
const AC_EXTENSION_UNIT: u8 = 0x08;

// Audio-streaming class-specific subtypes.
const AS_GENERAL: u8 = 0x01;
const AS_FORMAT_TYPE: u8 = 0x02;

// Interface subclasses. The class byte is vendor-specific (0xff) on every E-MU
// interface, but the subclass keeps USB-audio numbering. The 0404 USB also
// carries a MIDI-streaming interface, whose class-specific descriptors reuse
// the same subtype numbers for entirely different things, so which subclass we
// are inside has to be tracked rather than assumed.
const SUBCLASS_AUDIO_CONTROL: u8 = 0x01;
const SUBCLASS_AUDIO_STREAMING: u8 = 0x02;
const SUBCLASS_MIDI_STREAMING: u8 = 0x03;

// MIDI-streaming class-specific endpoint subtype.
const MS_GENERAL: u8 = 0x01;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParseError {
    TooShort,
    NotAConfiguration,
    /// A descriptor claimed a length that cannot be true.
    BadDescriptorLength,
    /// wTotalLength disagrees with the buffer actually supplied.
    LengthMismatch,
    TooManyAltSettings,
    TooManyExtensionUnits,
}

impl ParseError {
    pub fn code(self) -> u16 {
        match self {
            ParseError::TooShort => 1,
            ParseError::NotAConfiguration => 2,
            ParseError::BadDescriptorLength => 3,
            ParseError::LengthMismatch => 4,
            ParseError::TooManyAltSettings => 5,
            ParseError::TooManyExtensionUnits => 6,
        }
    }
}

/// One alternate setting of a streaming interface, flattened.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AltSetting {
    pub interface_number: u8,
    pub alternate_setting: u8,
    pub num_endpoints: u8,
    /// 0 when this alt setting carries no data endpoint (zero-bandwidth alt 0).
    pub data_endpoint: u8,
    pub data_endpoint_attributes: u8,
    /// Explicit feedback endpoint, or 0 when there is none.
    pub feedback_endpoint: u8,
    pub max_packet_size: u16,
    pub interval: u8,
    pub terminal_link: u8,
    pub channels: u8,
    /// Bytes per sample per channel.
    pub subframe_size: u8,
    pub bit_resolution: u8,
    pub sample_rate: u32,
}

impl AltSetting {
    /// Bytes per sample frame across all channels. Deriving this here keeps the
    /// bytes-versus-sample-frames distinction the guidelines warn about
    /// (section 6) in one place rather than at every call site.
    pub fn bytes_per_frame(&self) -> u32 {
        self.channels as u32 * self.subframe_size as u32
    }

    pub fn is_input(&self) -> bool {
        self.data_endpoint & 0x80 != 0
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ExtensionUnit {
    pub unit_id: u8,
    pub extension_code: u16,
    pub num_in_pins: u8,
    pub source_id: u8,
    pub channels: u8,
    /// bmControls, little-endian, up to 4 bytes of control size.
    pub controls: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Terminal {
    pub terminal_id: u8,
    pub terminal_type: u16,
    pub source_id: u8,
    pub channels: u8,
    pub is_input: u8,
}

pub const MAX_TERMINALS: usize = 12;

/// Validated topology. Fixed size, trivially copyable, no Apple or DriverKit
/// types anywhere.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceModel {
    pub configuration_value: u8,
    pub num_interfaces: u8,
    pub max_power_ma: u16,

    /// Interface carrying the audio-control descriptors and the extension units.
    pub control_interface: u8,
    /// Interrupt status endpoint on the control interface, 0 if absent.
    pub status_endpoint: u8,

    /// MIDI-streaming interface, 0xff when the device has none. The 0404 USB
    /// has one; the Tracker Pre has MIDI ports on the box but nothing in its
    /// descriptors.
    pub midi_interface: u8,
    /// Bulk endpoints of the MIDI interface, 0 when absent.
    pub midi_in_endpoint: u8,
    pub midi_out_endpoint: u8,
    /// Virtual cables multiplexed on each endpoint (embedded jack count from
    /// the class-specific endpoint descriptor).
    pub midi_in_cables: u8,
    pub midi_out_cables: u8,

    pub num_extension_units: u8,
    pub extension_units: [ExtensionUnit; MAX_EXTENSION_UNITS],

    pub num_terminals: u8,
    pub terminals: [Terminal; MAX_TERMINALS],

    pub num_alt_settings: u16,
    pub alt_settings: [AltSetting; MAX_ALT_SETTINGS],
}

impl Default for DeviceModel {
    fn default() -> Self {
        DeviceModel {
            configuration_value: 0,
            num_interfaces: 0,
            max_power_ma: 0,
            control_interface: 0,
            status_endpoint: 0,
            midi_interface: 0xff,
            midi_in_endpoint: 0,
            midi_out_endpoint: 0,
            midi_in_cables: 0,
            midi_out_cables: 0,
            num_extension_units: 0,
            extension_units: [ExtensionUnit::default(); MAX_EXTENSION_UNITS],
            num_terminals: 0,
            terminals: [Terminal::default(); MAX_TERMINALS],
            num_alt_settings: 0,
            alt_settings: [AltSetting::default(); MAX_ALT_SETTINGS],
        }
    }
}

impl DeviceModel {
    /// Unit ID of the extension unit with the given code, if the device has one.
    pub fn extension_unit(&self, code: u16) -> Option<&ExtensionUnit> {
        self.extension_units[..self.num_extension_units as usize]
            .iter()
            .find(|xu| xu.extension_code == code)
    }

    pub fn clock_rate_unit(&self) -> Option<&ExtensionUnit> {
        self.extension_unit(extension_code::CLOCK_RATE)
    }

    pub fn has_midi(&self) -> bool {
        self.midi_interface != 0xff && self.midi_in_endpoint != 0 && self.midi_out_endpoint != 0
    }

    pub fn alts(&self) -> &[AltSetting] {
        &self.alt_settings[..self.num_alt_settings as usize]
    }

    /// Distinct sample rates advertised across every streaming alt setting.
    pub fn sample_rates(&self, out: &mut [u32; 16]) -> usize {
        let mut n = 0;
        for alt in self.alts() {
            if alt.sample_rate == 0 {
                continue;
            }
            if out[..n].contains(&alt.sample_rate) {
                continue;
            }
            if n < out.len() {
                out[n] = alt.sample_rate;
                n += 1;
            }
        }
        out[..n].sort_unstable();
        n
    }
}

fn le16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

/// Walks the configuration descriptor chain and builds the validated model.
pub fn parse_configuration(bytes: &[u8]) -> Result<DeviceModel, ParseError> {
    if bytes.len() < 9 {
        return Err(ParseError::TooShort);
    }
    if bytes[1] != DT_CONFIGURATION {
        return Err(ParseError::NotAConfiguration);
    }

    let total_length = le16(&bytes[2..4]) as usize;
    if total_length > bytes.len() || total_length < 9 {
        return Err(ParseError::LengthMismatch);
    }
    let bytes = &bytes[..total_length];

    let mut model = DeviceModel {
        num_interfaces: bytes[4],
        configuration_value: bytes[5],
        // bMaxPower is in 2 mA units for full/high speed.
        max_power_ma: bytes[8] as u16 * 2,
        ..DeviceModel::default()
    };

    // Tracks the interface whose descriptors we are currently inside, so
    // class-specific descriptors can be attributed correctly.
    let mut current_is_control = false;
    let mut current_is_midi = false;
    let mut current_alt_index: Option<usize> = None;
    // Last endpoint seen on the MIDI interface, so the class-specific endpoint
    // descriptor that follows it can attribute its cable count.
    let mut current_midi_endpoint: u8 = 0;

    let mut offset = bytes[0] as usize;
    if offset == 0 {
        return Err(ParseError::BadDescriptorLength);
    }

    while offset + 2 <= bytes.len() {
        let length = bytes[offset] as usize;
        let dtype = bytes[offset + 1];

        // A zero or over-long length would let the walk loop forever or read
        // past the end; both mean the descriptor blob is malformed.
        if length < 2 || offset + length > bytes.len() {
            return Err(ParseError::BadDescriptorLength);
        }
        let d = &bytes[offset..offset + length];

        match dtype {
            DT_INTERFACE_ASSOCIATION => {}

            DT_INTERFACE => {
                if length < 9 {
                    return Err(ParseError::BadDescriptorLength);
                }
                let current_interface = d[2];
                let alternate_setting = d[3];
                let num_endpoints = d[4];
                let subclass = d[6];

                // Vendor-specific class (0xff) throughout, USB-audio subclass
                // numbering: 1 for control, 2 for streaming. Anything else --
                // MIDI streaming on the 0404 USB -- belongs to neither, and
                // its class-specific descriptors must be skipped rather than
                // read as audio.
                current_is_control = subclass == SUBCLASS_AUDIO_CONTROL;
                current_is_midi = subclass == SUBCLASS_MIDI_STREAMING;
                current_alt_index = None;

                if current_is_control {
                    model.control_interface = current_interface;
                } else if current_is_midi {
                    model.midi_interface = current_interface;
                } else if subclass == SUBCLASS_AUDIO_STREAMING {
                    let index = model.num_alt_settings as usize;
                    if index >= MAX_ALT_SETTINGS {
                        return Err(ParseError::TooManyAltSettings);
                    }
                    model.alt_settings[index] = AltSetting {
                        interface_number: current_interface,
                        alternate_setting,
                        num_endpoints,
                        ..AltSetting::default()
                    };
                    model.num_alt_settings += 1;
                    current_alt_index = Some(index);
                }
            }

            DT_ENDPOINT => {
                if length < 7 {
                    return Err(ParseError::BadDescriptorLength);
                }
                let address = d[2];
                let attributes = d[3];
                let max_packet = le16(&d[4..6]);
                let interval = d[6];

                if current_is_control {
                    model.status_endpoint = address;
                } else if current_is_midi {
                    // Bulk data endpoints only; the jack topology behind them
                    // is fixed on this hardware and not modelled.
                    if attributes & 0x03 == 0x02 {
                        if address & 0x80 != 0 {
                            model.midi_in_endpoint = address;
                        } else {
                            model.midi_out_endpoint = address;
                        }
                        current_midi_endpoint = address;
                    }
                } else if let Some(index) = current_alt_index {
                    let alt = &mut model.alt_settings[index];
                    // Bits 5:4 == 01 marks a feedback endpoint. The data
                    // endpoint is whichever one is not that.
                    let is_feedback = (attributes & 0x0f) == 0x01 && (attributes & 0x30) == 0x10;
                    if is_feedback {
                        alt.feedback_endpoint = address;
                    } else if alt.data_endpoint == 0 {
                        alt.data_endpoint = address;
                        alt.data_endpoint_attributes = attributes;
                        alt.max_packet_size = max_packet;
                        alt.interval = interval;
                    }
                }
            }

            DT_CS_INTERFACE => {
                if length < 3 {
                    return Err(ParseError::BadDescriptorLength);
                }
                let subtype = d[2];

                if current_is_control {
                    match subtype {
                        AC_HEADER => {}

                        AC_INPUT_TERMINAL | AC_OUTPUT_TERMINAL => {
                            let is_input = subtype == AC_INPUT_TERMINAL;
                            let needed = if is_input { 9 } else { 9 };
                            if length < needed {
                                return Err(ParseError::BadDescriptorLength);
                            }
                            let index = model.num_terminals as usize;
                            if index < MAX_TERMINALS {
                                model.terminals[index] = Terminal {
                                    terminal_id: d[3],
                                    terminal_type: le16(&d[4..6]),
                                    source_id: if is_input { 0 } else { d[7] },
                                    channels: if is_input && length >= 9 { d[7] } else { 0 },
                                    is_input: is_input as u8,
                                };
                                model.num_terminals += 1;
                            }
                        }

                        AC_FEATURE_UNIT => {}

                        AC_EXTENSION_UNIT => {
                            // bLength, bDescriptorType, bDescriptorSubtype,
                            // bUnitID, wExtensionCode(2), bNrInPins,
                            // baSourceID[], bNrChannels, wChannelConfig(2),
                            // iChannelNames, bControlSize, bmControls[], iExtension
                            if length < 13 {
                                return Err(ParseError::BadDescriptorLength);
                            }
                            let num_in_pins = d[6] as usize;
                            // Everything after the source-ID array is variable
                            // in position, so index relative to it rather than
                            // assuming a fixed layout.
                            let after_pins = 7 + num_in_pins;
                            if after_pins + 4 > length {
                                return Err(ParseError::BadDescriptorLength);
                            }
                            let channels = d[after_pins];
                            let control_size = d[after_pins + 4] as usize;
                            let controls_at = after_pins + 5;
                            if controls_at + control_size > length {
                                return Err(ParseError::BadDescriptorLength);
                            }

                            let mut controls: u32 = 0;
                            for i in 0..control_size.min(4) {
                                controls |= (d[controls_at + i] as u32) << (8 * i);
                            }

                            let index = model.num_extension_units as usize;
                            if index >= MAX_EXTENSION_UNITS {
                                return Err(ParseError::TooManyExtensionUnits);
                            }
                            model.extension_units[index] = ExtensionUnit {
                                unit_id: d[3],
                                extension_code: le16(&d[4..6]),
                                num_in_pins: num_in_pins as u8,
                                source_id: if num_in_pins > 0 { d[7] } else { 0 },
                                channels,
                                controls,
                            };
                            model.num_extension_units += 1;
                        }

                        _ => {}
                    }
                } else if let Some(index) = current_alt_index {
                    let alt = &mut model.alt_settings[index];
                    match subtype {
                        AS_GENERAL => {
                            if length >= 4 {
                                alt.terminal_link = d[3];
                            }
                        }
                        AS_FORMAT_TYPE => {
                            // bFormatType, bNrChannels, bSubframeSize,
                            // bBitResolution, bSamFreqType, tSamFreq[]
                            if length < 11 {
                                return Err(ParseError::BadDescriptorLength);
                            }
                            alt.channels = d[4];
                            alt.subframe_size = d[5];
                            alt.bit_resolution = d[6];
                            let freq_type = d[7];
                            // Discrete rate list; the Tracker Pre always
                            // advertises exactly one rate per alt setting.
                            if freq_type >= 1 && length >= 11 {
                                alt.sample_rate = u32::from_le_bytes([d[8], d[9], d[10], 0]);
                            }
                        }
                        _ => {}
                    }
                }
            }

            DT_CS_ENDPOINT => {
                // MS_GENERAL carries bNrEmbMIDIJack: how many virtual cables
                // the endpoint it follows multiplexes.
                if current_is_midi && length >= 4 && d[2] == MS_GENERAL {
                    if current_midi_endpoint & 0x80 != 0 {
                        model.midi_in_cables = d[3];
                    } else if current_midi_endpoint != 0 {
                        model.midi_out_cables = d[3];
                    }
                }
            }

            _ => {}
        }

        offset += length;
    }

    Ok(model)
}
