//! CA0189 protocol constants and request encoding.
//!
//! Values are taken from E-MU's released driver source
//! (`EMUUSBDeviceDefines.h`, `USBAudioObject.h`) and confirmed against the
//! descriptors the Tracker Pre actually reports.

/// E-MU extension unit codes (`wExtensionCode` in an EXTENSION_UNIT descriptor).
pub mod extension_code {
    pub const CLOCK_RATE: u16 = 0xe301;
    pub const CLOCK_SOURCE: u16 = 0xe302;
    pub const DIGITAL_IO_STATUS: u16 = 0xe303;
    pub const DEVICE_OPTIONS: u16 = 0xe304;
    pub const DIRECT_MONITORING: u16 = 0xe305;
    pub const METERING: u16 = 0xe306;
}

/// Control selectors within an extension unit. Note that these overlap by
/// design: the meaning of selector 0x02 depends on which unit it is sent to.
pub mod selector {
    pub const ENABLE_PROCESSING: u8 = 0x01;
    /// On the clock rate unit: bitmap of supported rates.
    pub const CLOCK_RATE_SUPPORT: u8 = 0x02;
    /// On the clock rate unit: the active rate code.
    pub const CLOCK_RATE: u8 = 0x03;
    /// On the clock source unit.
    pub const CLOCK_SOURCE: u8 = 0x02;
}

/// USB audio class request codes.
pub mod request {
    pub const SET_CUR: u8 = 0x01;
    pub const GET_CUR: u8 = 0x81;
}

/// `bmRequestType` for a class request addressed to an interface.
pub mod request_type {
    pub const CLASS_INTERFACE_IN: u8 = 0xa1;
    pub const CLASS_INTERFACE_OUT: u8 = 0x21;
}

/// The device's own sample-rate encoding (`eSampleRate` in E-MU's source).
///
/// These are opaque codes, not frequencies. Confusing the two is exactly the
/// kind of mistake the type discipline in the guidelines exists to prevent.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RateCode {
    Hz44100 = 0,
    Hz48000 = 1,
    Hz88200 = 2,
    Hz96000 = 3,
    Hz176400 = 4,
    Hz192000 = 5,
}

impl RateCode {
    pub const ALL: [RateCode; 6] = [
        RateCode::Hz44100,
        RateCode::Hz48000,
        RateCode::Hz88200,
        RateCode::Hz96000,
        RateCode::Hz176400,
        RateCode::Hz192000,
    ];

    pub fn from_code(code: u8) -> Option<RateCode> {
        match code {
            0 => Some(RateCode::Hz44100),
            1 => Some(RateCode::Hz48000),
            2 => Some(RateCode::Hz88200),
            3 => Some(RateCode::Hz96000),
            4 => Some(RateCode::Hz176400),
            5 => Some(RateCode::Hz192000),
            _ => None,
        }
    }

    pub fn from_hz(hz: u32) -> Option<RateCode> {
        match hz {
            44100 => Some(RateCode::Hz44100),
            48000 => Some(RateCode::Hz48000),
            88200 => Some(RateCode::Hz88200),
            96000 => Some(RateCode::Hz96000),
            176400 => Some(RateCode::Hz176400),
            192000 => Some(RateCode::Hz192000),
            _ => None,
        }
    }

    pub fn hz(self) -> u32 {
        match self {
            RateCode::Hz44100 => 44100,
            RateCode::Hz48000 => 48000,
            RateCode::Hz88200 => 88200,
            RateCode::Hz96000 => 96000,
            RateCode::Hz176400 => 176400,
            RateCode::Hz192000 => 192000,
        }
    }
}

/// A control transfer setup packet, built rather than hand-assembled at each
/// call site so the encoding lives in exactly one place.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ControlSetup {
    pub bm_request_type: u8,
    pub b_request: u8,
    pub w_value: u16,
    pub w_index: u16,
    pub w_length: u16,
}

impl ControlSetup {
    /// Read an extension unit control.
    ///
    /// Matches `EMUUSBAudioDevice::getExtensionUnitSetting`:
    /// `wValue = selector << 8`, `wIndex = (unit_id << 8) | interface`.
    pub fn get_xu(unit_id: u8, interface: u8, selector: u8, length: u16) -> ControlSetup {
        ControlSetup {
            bm_request_type: request_type::CLASS_INTERFACE_IN,
            b_request: request::GET_CUR,
            w_value: (selector as u16) << 8,
            w_index: ((unit_id as u16) << 8) | interface as u16,
            w_length: length,
        }
    }

    /// Write an extension unit control.
    pub fn set_xu(unit_id: u8, interface: u8, selector: u8, length: u16) -> ControlSetup {
        ControlSetup {
            bm_request_type: request_type::CLASS_INTERFACE_OUT,
            b_request: request::SET_CUR,
            w_value: (selector as u16) << 8,
            w_index: ((unit_id as u16) << 8) | interface as u16,
            w_length: length,
        }
    }
}
