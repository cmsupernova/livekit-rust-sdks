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

use std::fmt::Debug;

use crate::{
    imp::peer_connection_factory as imp_pcf, peer_connection::PeerConnection,
    rtp_parameters::RtpCapabilities, MediaType, RtcError,
};

#[derive(Debug, Clone)]
pub struct IceServer {
    pub urls: Vec<String>,
    pub username: String,
    pub password: String,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum ContinualGatheringPolicy {
    GatherOnce,
    GatherContinually,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum IceTransportsType {
    Relay,
    NoHost,
    All,
}

#[derive(Debug, Clone)]
pub struct RtcConfiguration {
    pub ice_servers: Vec<IceServer>,
    pub continual_gathering_policy: ContinualGatheringPolicy,
    pub ice_transport_type: IceTransportsType,
}

impl Default for RtcConfiguration {
    fn default() -> Self {
        Self {
            ice_servers: vec![],
            continual_gathering_policy: ContinualGatheringPolicy::GatherContinually,
            ice_transport_type: IceTransportsType::All,
        }
    }
}

#[derive(Clone, Default)]
pub struct PeerConnectionFactory {
    pub(crate) handle: imp_pcf::PeerConnectionFactory,
}

impl Debug for PeerConnectionFactory {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.debug_struct("PeerConnectionFactory").finish()
    }
}

impl PeerConnectionFactory {
    pub fn create_peer_connection(
        &self,
        config: RtcConfiguration,
    ) -> Result<PeerConnection, RtcError> {
        self.handle.create_peer_connection(config)
    }

    pub fn get_rtp_sender_capabilities(&self, media_type: MediaType) -> RtpCapabilities {
        self.handle.get_rtp_sender_capabilities(media_type)
    }

    pub fn get_rtp_receiver_capabilities(&self, media_type: MediaType) -> RtpCapabilities {
        self.handle.get_rtp_receiver_capabilities(media_type)
    }
}

#[cfg(not(target_arch = "wasm32"))]
impl PeerConnectionFactory {
    /// Creates a dedicated native factory whose ICE allocator can use loopback adapters.
    /// This allows loopback in addition to the otherwise permitted interfaces.
    pub fn with_loopback_ice() -> Self {
        Self { handle: imp_pcf::PeerConnectionFactory::with_loopback_ice() }
    }

    /// Creates one native peer connection restricted to numeric loopback ICE-TCP.
    /// UDP, DNS resolution and configured STUN/TURN servers are disabled.
    pub fn with_loopback_only_ice() -> Self {
        Self { handle: imp_pcf::PeerConnectionFactory::with_loopback_only_ice() }
    }

    /// Creates one native peer connection that starts with only loopback ICE-TCP.
    /// Its owner can add or withdraw numeric direct host routes without replacing
    /// the connection. Only configured STUN/TURN hostnames can resolve while
    /// direct ICE is allowed.
    pub fn with_consent_fenced_ice() -> Self {
        Self { handle: imp_pcf::PeerConnectionFactory::with_consent_fenced_ice() }
    }

    /// Creates a native peer connection with optional TCP gathering and ICE renomination policy.
    pub fn create_peer_connection_with_ice_options(
        &self,
        config: RtcConfiguration,
        ice: native::RtcIceOptions,
    ) -> Result<PeerConnection, RtcError> {
        self.handle.create_peer_connection_with_ice_options(config, ice)
    }
}

pub mod native {
    use super::PeerConnectionFactory;

    #[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
    pub enum TcpCandidatePolicy {
        #[default]
        Default,
        Enabled,
        Disabled,
    }

    /// Opt-in ICE policy for native peer connections.
    #[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
    pub struct RtcIceOptions {
        pub tcp_candidate_policy: TcpCandidatePolicy,
        pub enable_ice_renomination: bool,
    }
    use crate::{
        audio_source::native::NativeAudioSource, audio_track::RtcAudioTrack,
        video_source::native::NativeVideoSource, video_track::RtcVideoTrack,
    };

    pub trait PeerConnectionFactoryExt {
        fn create_video_track(&self, label: &str, source: NativeVideoSource) -> RtcVideoTrack;
        fn create_audio_track(&self, label: &str, source: NativeAudioSource) -> RtcAudioTrack;
    }

    impl PeerConnectionFactoryExt for PeerConnectionFactory {
        fn create_video_track(&self, label: &str, source: NativeVideoSource) -> RtcVideoTrack {
            self.handle.create_video_track(label, source)
        }

        fn create_audio_track(&self, label: &str, source: NativeAudioSource) -> RtcAudioTrack {
            self.handle.create_audio_track(label, source)
        }
    }
}
