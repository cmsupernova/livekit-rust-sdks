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

use crate::impl_thread_safety;

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    /// Aggregated NVENC timing, drained on read. All zero when the NVENC
    /// path is not compiled in or has not encoded anything yet.
    #[derive(Debug)]
    pub struct NvencTiming {
        pub copy_us: u64,
        pub submit_us: u64,
        pub wait_us: u64,
        pub frames: u64,
    }

    #[derive(Debug)]
    #[repr(i32)]
    pub enum MediaType {
        Audio,
        Video,
        Data,
        Unsupported,
    }

    #[derive(Debug)]
    #[repr(i32)]
    pub enum Priority {
        VeryLow,
        Low,
        Medium,
        High,
    }

    #[derive(Debug)]
    #[repr(i32)]
    pub enum RtpTransceiverDirection {
        SendRecv,
        SendOnly,
        RecvOnly,
        Inactive,
        Stopped,
    }

    #[derive(Debug)]
    #[repr(i32)]
    pub enum LoggingSeverity {
        Verbose,
        Info,
        Warning,
        Error,
        None,
    }

    unsafe extern "C++" {
        include!("livekit/webrtc.h");

        type LogSink;

        fn create_random_uuid() -> String;
        /// Read and reset the NVENC timing counters. Splits the single
        /// `encode_ms_per_frame` stat into host->device copy, submit and
        /// bitstream wait so the dominant cost is identifiable.
        fn nvenc_timing_take() -> NvencTiming;
        fn new_log_sink(fnc: fn(String, LoggingSeverity)) -> UniquePtr<LogSink>;
    }
}

impl_thread_safety!(ffi::LogSink, Send + Sync);
