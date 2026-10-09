// Copyright 2025 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use std::{
    error::Error,
    fmt::{Display, Formatter},
};

// cxx doesn't support custom Exception type, so we serialize RtcError inside the cxx::Exception
// "what" string

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    #[derive(Debug)]
    #[repr(i32)]
    pub enum RtcErrorType {
        None,
        UnsupportedOperation,
        UnsupportedParameter,
        InvalidParameter,
        InvalidRange,
        SyntaxError,
        InvalidState,
        InvalidModification,
        NetworkError,
        ResourceExhausted,
        InternalError,
        OperationErrorWithData,
    }

    #[derive(Debug)]
    #[repr(i32)]
    pub enum RtcErrorDetailType {
        None,
        DataChannelFailure,
        DtlsFailure,
        FingerprintFailure,
        SctpFailure,
        SdpSyntaxError,
        HardwareEncoderNotAvailable,
        HardwareEncoderError,
    }

    #[derive(Debug)]
    pub struct RtcError {
        pub error_type: RtcErrorType,
        pub message: String,
        pub error_detail: RtcErrorDetailType,
        // cxx doesn't support the Option trait
        pub has_sctp_cause_code: bool,
        pub sctp_cause_code: u16,
    }
}

impl ffi::RtcError {
    /// Decode only complete, known headers. The UTF-8 message is not hex encoded.
    /// Backports upstream #1098/#1466 without reporting malformed errors as success.
    pub fn parse(value: &str) -> Option<Self> {
        let header = value.get(..22)?;
        if !header.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return None;
        }
        let error_type = match u32::from_str_radix(header.get(..8)?, 16).ok()? {
            0 => ffi::RtcErrorType::None,
            1 => ffi::RtcErrorType::UnsupportedOperation,
            2 => ffi::RtcErrorType::UnsupportedParameter,
            3 => ffi::RtcErrorType::InvalidParameter,
            4 => ffi::RtcErrorType::InvalidRange,
            5 => ffi::RtcErrorType::SyntaxError,
            6 => ffi::RtcErrorType::InvalidState,
            7 => ffi::RtcErrorType::InvalidModification,
            8 => ffi::RtcErrorType::NetworkError,
            9 => ffi::RtcErrorType::ResourceExhausted,
            10 => ffi::RtcErrorType::InternalError,
            11 => ffi::RtcErrorType::OperationErrorWithData,
            _ => return None,
        };
        let error_detail = match u32::from_str_radix(header.get(8..16)?, 16).ok()? {
            0 => ffi::RtcErrorDetailType::None,
            1 => ffi::RtcErrorDetailType::DataChannelFailure,
            2 => ffi::RtcErrorDetailType::DtlsFailure,
            3 => ffi::RtcErrorDetailType::FingerprintFailure,
            4 => ffi::RtcErrorDetailType::SctpFailure,
            5 => ffi::RtcErrorDetailType::SdpSyntaxError,
            6 => ffi::RtcErrorDetailType::HardwareEncoderNotAvailable,
            7 => ffi::RtcErrorDetailType::HardwareEncoderError,
            _ => return None,
        };
        let has_sctp_cause_code = match u8::from_str_radix(header.get(16..18)?, 16).ok()? {
            0 => false,
            1 => true,
            _ => return None,
        };
        Some(Self {
            error_type,
            error_detail,
            has_sctp_cause_code,
            sctp_cause_code: u16::from_str_radix(header.get(18..22)?, 16).ok()?,
            message: value.get(22..)?.into(),
        })
    }

    /// # Safety
    /// Kept unsafe for source compatibility; arbitrary strings are now accepted.
    pub unsafe fn from(value: &str) -> Self {
        Self::parse(value).unwrap_or_else(|| Self {
            error_type: ffi::RtcErrorType::InternalError,
            error_detail: ffi::RtcErrorDetailType::None,
            sctp_cause_code: 0,
            has_sctp_cause_code: false,
            message: value.into(),
        })
    }

    pub fn ok(&self) -> bool {
        self.error_type == ffi::RtcErrorType::None
    }
}

impl Error for ffi::RtcError {}

impl Display for ffi::RtcError {
    fn fmt(&self, f: &mut Formatter) -> std::fmt::Result {
        write!(f, "RtcError occurred {:?}: {}", self.error_type, self.message)
    }
}

#[cfg(test)]
mod tests {
    use crate::rtc_error::ffi::{RtcError, RtcErrorDetailType, RtcErrorType};

    #[test]
    fn malformed_headers_remain_errors_without_panicking() {
        let valid = "0000000a00000001010018message";
        for len in 0..22 {
            let input = &valid[..len];
            assert!(RtcError::parse(input).is_none());
            let error = unsafe { RtcError::from(input) };
            assert!(!error.ok());
            assert_eq!(error.message, input);
        }
        for input in [
            "not a serialized RTC error",
            "ffffffff00000000000000unknown type",
            "0000000affffffff000000unknown detail",
            "0000000a00000000020000bad flag",
            "+000000a00000000000000signed header",
        ] {
            assert!(RtcError::parse(input).is_none());
            let error = unsafe { RtcError::from(input) };
            assert_eq!(error.error_type, RtcErrorType::InternalError);
            assert!(!error.ok());
            assert_eq!(error.message, input);
        }
    }

    #[test]
    fn unicode_header_is_rejected_but_unicode_message_is_preserved() {
        for offset in 0..22 {
            for symbol in ["é", "界", "🦀"] {
                let input = format!("{}{}{}", "0".repeat(offset), symbol, "0".repeat(24));
                assert!(RtcError::parse(&input).is_none());
                assert!(!unsafe { RtcError::from(&input) }.ok());
            }
        }
        let error = RtcError::parse("0000000a00000000000000é界🦀").unwrap();
        assert_eq!(error.message, "é界🦀");
        assert!(!error.ok());
    }

    #[test]
    fn every_known_error_type_and_detail_round_trips() {
        for error_type in 0..=11 {
            for detail in 0..=7 {
                let input = format!("{error_type:08x}{detail:08x}01ffffmessage");
                let error = RtcError::parse(&input).unwrap();
                assert_eq!(error.ok(), error_type == 0);
                assert!(error.has_sctp_cause_code);
                assert_eq!(error.sctp_cause_code, u16::MAX);
                assert_eq!(error.message, "message");
            }
        }
    }

    #[cxx::bridge(namespace = "livekit_ffi")]
    pub mod ffi {
        unsafe extern "C++" {
            include!("livekit/rtc_error.h");

            fn serialize_deserialize() -> String;
            fn throw_error() -> Result<()>;
        }
    }

    #[test]
    fn serialize_deserialize() {
        let str = ffi::serialize_deserialize();
        let error = unsafe { RtcError::from(&str) };

        assert_eq!(error.error_type, RtcErrorType::InternalError);
        assert_eq!(error.error_detail, RtcErrorDetailType::DataChannelFailure);
        assert!(error.has_sctp_cause_code);
        assert_eq!(error.sctp_cause_code, 24);
        assert_eq!(error.message, "this is not a test, I repeat, this is not a test");
    }

    #[test]
    fn throw_error() {
        let exc: cxx::Exception = ffi::throw_error().err().unwrap();
        let error = unsafe { RtcError::from(exc.what()) };

        assert_eq!(error.error_type, RtcErrorType::InvalidModification);
        assert_eq!(error.error_detail, RtcErrorDetailType::None);
        assert!(!error.has_sctp_cause_code);
        assert_eq!(error.sctp_cause_code, 0);
        assert_eq!(error.message, "exception is thrown!");
    }
}
