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
        /// Number of calls that actually returned encoded output. Distinct
        /// from submitted `frames` when output-delay pipelining is enabled.
        pub wait_frames: u64,
        pub frames: u64,
        /// Bitstream-wait distribution over the drained window. Percentiles
        /// are reported at the containing histogram bucket edge, clamped to
        /// the true maximum, so they are upper bounds rather than estimates.
        pub wait_p50_us: u64,
        pub wait_p95_us: u64,
        pub wait_max_us: u64,
        /// Longest stretch with no encoded output at all: the freeze a viewer
        /// actually sees, which per-frame timings cannot express.
        pub output_gap_max_us: u64,
        /// Submit-to-output residency. `latency_frames` is its own counter
        /// because frames in flight at the drain boundary have no latency yet.
        pub latency_us: u64,
        pub latency_max_us: u64,
        pub latency_frames: u64,
        /// Extra NVENC output surfaces currently configured, so a measurement
        /// window records which arm of an A/B produced it.
        pub output_delay: u64,
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
        /// Sets `nExtraOutputDelay` for NVENC encoders created from now on.
        /// Takes effect on the next encoder creation, never mid-stream.
        fn nvenc_set_output_delay(delay: u32);
        /// Selects the NVENC effort profile (0 quality, 1 single-pass, 2 fast)
        /// for encoders created from now on. Takes effect on the next encoder
        /// creation, never mid-stream.
        fn nvenc_set_screen_profile(profile: u32);
        fn new_log_sink(fnc: fn(String, LoggingSeverity)) -> UniquePtr<LogSink>;
    }
}

impl_thread_safety!(ffi::LogSink, Send + Sync);
