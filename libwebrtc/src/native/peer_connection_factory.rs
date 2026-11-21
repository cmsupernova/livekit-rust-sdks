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

use std::sync::Arc;

use cxx::{SharedPtr, UniquePtr};
use lazy_static::lazy_static;
use parking_lot::Mutex;
use webrtc_sys::{peer_connection_factory as sys_pcf, rtc_error as sys_err, webrtc as sys_rtc};

use crate::{
    audio_source::native::NativeAudioSource,
    audio_track::RtcAudioTrack,
    imp::{audio_track as imp_at, peer_connection as imp_pc, video_track as imp_vt},
    peer_connection::PeerConnection,
    peer_connection_factory::{
        native::{RtcIceOptions, TcpCandidatePolicy},
        RtcConfiguration,
    },
    rtp_parameters::RtpCapabilities,
    video_source::native::NativeVideoSource,
    video_track::RtcVideoTrack,
    MediaType, RtcError,
};

lazy_static! {
    static ref LOG_SINK: Mutex<Option<UniquePtr<sys_rtc::ffi::LogSink>>> = Default::default();
}

#[derive(Clone)]
pub struct PeerConnectionFactory {
    pub(crate) sys_handle: SharedPtr<sys_pcf::ffi::PeerConnectionFactory>,
}

impl Default for PeerConnectionFactory {
    fn default() -> Self {
        Self::new(sys_pcf::ffi::create_peer_connection_factory)
    }
}

impl PeerConnectionFactory {
    pub fn with_loopback_ice() -> Self {
        Self::new(sys_pcf::ffi::create_peer_connection_factory_with_loopback_ice)
    }

    pub fn with_loopback_only_ice() -> Self {
        Self::new(sys_pcf::ffi::create_peer_connection_factory_with_loopback_only_ice)
    }

    pub fn with_consent_fenced_ice() -> Self {
        Self::new(sys_pcf::ffi::create_peer_connection_factory_with_consent_fenced_ice)
    }

    fn new(create: fn() -> SharedPtr<sys_pcf::ffi::PeerConnectionFactory>) -> Self {
        let mut log_sink = LOG_SINK.lock();
        if log_sink.is_none() {
            *log_sink = Some(sys_rtc::ffi::new_log_sink(|msg, _| {
                let msg = msg.strip_suffix("\r\n").or(msg.strip_suffix('\n')).unwrap_or(&msg);

                log::debug!(target: "libwebrtc", "{}", msg);
            }));
        }

        Self { sys_handle: create() }
    }
    pub fn create_peer_connection(
        &self,
        config: RtcConfiguration,
    ) -> Result<PeerConnection, RtcError> {
        let observer = Arc::new(imp_pc::PeerObserver::default());
        let res = self.sys_handle.create_peer_connection(
            config.into(),
            Box::new(sys_pcf::PeerConnectionObserverWrapper::new(observer.clone())),
        );

        Self::finish_create(observer, res)
    }

    pub fn create_peer_connection_with_ice_options(
        &self,
        config: RtcConfiguration,
        ice: RtcIceOptions,
    ) -> Result<PeerConnection, RtcError> {
        use sys_pcf::ffi::TcpCandidatePolicy as SysTcpPolicy;
        let policy = match ice.tcp_candidate_policy {
            TcpCandidatePolicy::Default => SysTcpPolicy::Default,
            TcpCandidatePolicy::Enabled => SysTcpPolicy::Enabled,
            TcpCandidatePolicy::Disabled => SysTcpPolicy::Disabled,
        };
        let observer = Arc::new(imp_pc::PeerObserver::default());
        let res = self.sys_handle.create_peer_connection_with_ice_options(
            config.into(),
            policy,
            ice.enable_ice_renomination,
            Box::new(sys_pcf::PeerConnectionObserverWrapper::new(observer.clone())),
        );
        Self::finish_create(observer, res)
    }

    fn finish_create(
        observer: Arc<imp_pc::PeerObserver>,
        res: Result<SharedPtr<webrtc_sys::peer_connection::ffi::PeerConnection>, cxx::Exception>,
    ) -> Result<PeerConnection, RtcError> {
        match res {
            Ok(sys_handle) => Ok(PeerConnection {
                handle: imp_pc::PeerConnection::configure(sys_handle, observer),
            }),
            Err(e) => Err(unsafe { sys_err::ffi::RtcError::from(e.what()).into() }),
        }
    }

    pub fn create_video_track(&self, label: &str, source: NativeVideoSource) -> RtcVideoTrack {
        RtcVideoTrack {
            handle: imp_vt::RtcVideoTrack {
                sys_handle: self
                    .sys_handle
                    .create_video_track(label.to_string(), source.handle.sys_handle()),
            },
        }
    }

    pub fn create_audio_track(&self, label: &str, source: NativeAudioSource) -> RtcAudioTrack {
        RtcAudioTrack {
            handle: imp_at::RtcAudioTrack {
                sys_handle: self
                    .sys_handle
                    .create_audio_track(label.to_string(), source.handle.sys_handle()),
            },
        }
    }

    pub fn get_rtp_sender_capabilities(&self, media_type: MediaType) -> RtpCapabilities {
        self.sys_handle.rtp_sender_capabilities(media_type.into()).into()
    }

    pub fn get_rtp_receiver_capabilities(&self, media_type: MediaType) -> RtpCapabilities {
        self.sys_handle.rtp_receiver_capabilities(media_type.into()).into()
    }
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;
    use crate::{
        data_channel::DataChannelInit,
        peer_connection::{IceGatheringState, OfferOptions},
        peer_connection_factory::{IceServer, PeerConnectionFactory as Factory},
    };

    async fn ice_offer(pc: &PeerConnection) -> crate::session_description::SessionDescription {
        let channel = pc.create_data_channel("test", DataChannelInit::default()).unwrap();
        let offer = pc.create_offer(OfferOptions::default()).await.unwrap();
        drop(channel);
        offer
    }

    #[tokio::test]
    async fn factory_ice_renomination_is_opt_in() {
        let factory = Factory::default();
        let normal = factory.create_peer_connection(RtcConfiguration::default()).unwrap();
        assert!(!ice_offer(&normal).await.to_string().contains("renomination"));

        let opted_in = factory
            .create_peer_connection_with_ice_options(
                RtcConfiguration::default(),
                RtcIceOptions {
                    tcp_candidate_policy: TcpCandidatePolicy::Disabled,
                    enable_ice_renomination: true,
                },
            )
            .unwrap();
        assert!(ice_offer(&opted_in).await.to_string().contains("renomination"));
        opted_in.set_configuration(RtcConfiguration::default()).unwrap();
        assert!(ice_offer(&opted_in).await.to_string().contains("renomination"));
        normal.close();
        opted_in.close();
    }

    #[tokio::test]
    async fn factory_ice_loopback_only_rejects_servers_and_gathers_only_local_tcp() {
        let factory = Factory::with_loopback_only_ice();
        let with_server = RtcConfiguration {
            ice_servers: vec![IceServer {
                urls: vec!["stun:127.0.0.1:9".into()],
                username: String::new(),
                password: String::new(),
            }],
            ..RtcConfiguration::default()
        };
        assert!(factory.create_peer_connection(with_server.clone()).is_err());

        let pc = factory
            .create_peer_connection_with_ice_options(
                RtcConfiguration {
                    continual_gathering_policy:
                        crate::peer_connection_factory::ContinualGatheringPolicy::GatherOnce,
                    ..RtcConfiguration::default()
                },
                RtcIceOptions {
                    tcp_candidate_policy: TcpCandidatePolicy::Enabled,
                    enable_ice_renomination: true,
                },
            )
            .unwrap();
        assert!(pc.set_configuration(with_server).is_err());

        let (candidate_tx, mut candidate_rx) = tokio::sync::mpsc::unbounded_channel();
        pc.on_ice_candidate(Some(Box::new(move |candidate| {
            let _ = candidate_tx.send(candidate);
        })));
        let (end_tx, mut end_rx) = tokio::sync::mpsc::unbounded_channel();
        pc.on_ice_gathering_state_change(Some(Box::new(move |state| {
            if state == IceGatheringState::Complete {
                let _ = end_tx.send(());
            }
        })));
        let _channel = pc.create_data_channel("test", DataChannelInit::default()).unwrap();
        let offer = pc.create_offer(OfferOptions::default()).await.unwrap();
        pc.set_local_description(offer).await.unwrap();
        tokio::time::timeout(Duration::from_secs(10), end_rx.recv())
            .await
            .expect("loopback gathering must finish")
            .unwrap();
        let mut passive_ipv4 = None;
        while let Ok(candidate) = candidate_rx.try_recv() {
            let sdp = candidate.to_string();
            let fields: Vec<_> = sdp.split_ascii_whitespace().collect();
            assert!(fields[2].eq_ignore_ascii_case("tcp"));
            assert!(matches!(fields[4], "127.0.0.1" | "::1"));
            assert_eq!(fields[7], "host");
            if fields[4] == "127.0.0.1"
                && fields.windows(2).any(|values| values == ["tcptype", "passive"])
            {
                passive_ipv4 = fields[5].parse::<u16>().ok().filter(|port| *port > 9);
            }
        }
        let port = passive_ipv4.expect("loopback gathering produced no IPv4 TCP listener");
        assert!(factory.create_peer_connection(RtcConfiguration::default()).is_err());
        let other = std::net::TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
        assert_eq!(
            pc.dial_loopback_ice_listener(other.local_addr().unwrap().port()).unwrap_err().kind(),
            std::io::ErrorKind::PermissionDenied,
        );
        let connected = pc.dial_loopback_ice_listener(port).unwrap();
        assert_eq!(connected.peer_addr().unwrap().port(), port);
        let ordinary =
            Factory::default().create_peer_connection(RtcConfiguration::default()).unwrap();
        assert!(ordinary.dial_loopback_ice_listener(port).is_err());
        ordinary.close();
        pc.close();
        assert!(pc.dial_loopback_ice_listener(port).is_err());
    }

    #[tokio::test]
    async fn factory_ice_consent_changes_preserve_the_private_pc() {
        let factory = Factory::with_consent_fenced_ice();
        let servers = |host: &str| RtcConfiguration {
            ice_servers: vec![IceServer {
                urls: vec![format!("stun:{host}:3478"), format!("turn:{host}:3478")],
                username: "user".into(),
                password: "password".into(),
            }],
            ..RtcConfiguration::default()
        };
        assert!(factory.create_peer_connection(servers("localhost")).is_err());
        let pc = factory.create_peer_connection(servers("192.0.2.9")).unwrap();
        let named_turn = RtcConfiguration {
            ice_servers: vec![IceServer {
                urls: vec!["turns:relay.example:443?transport=tcp".into()],
                username: "user".into(),
                password: "password".into(),
            }],
            ..RtcConfiguration::default()
        };
        assert!(pc.set_configuration(named_turn).is_ok());
        assert!(pc.set_configuration(servers("127.0.0.1")).is_err());
        assert!(pc.set_configuration(servers("192.0.2.10")).is_ok());
        let configured_stun = RtcConfiguration {
            ice_servers: vec![IceServer {
                urls: vec!["stun:stun.example:3478".into()],
                username: String::new(),
                password: String::new(),
            }],
            ..RtcConfiguration::default()
        };
        assert!(pc.set_configuration(configured_stun).is_ok());
        assert!(pc.set_direct_ice_allowed(true));
        assert!(pc.set_direct_ice_allowed(false));
        assert!(pc.set_direct_ice_allowed(true));
        assert!(pc.set_direct_ice_allowed(false));
        assert!(!ice_offer(&pc).await.to_string().is_empty());
        pc.close();
        assert!(!pc.set_direct_ice_allowed(true));
        let strict = Factory::with_loopback_only_ice()
            .create_peer_connection(RtcConfiguration::default())
            .unwrap();
        assert!(!strict.set_direct_ice_allowed(true));
        strict.close();
    }

    #[tokio::test]
    #[ignore = "requires a non-loopback host interface on the test computer"]
    async fn factory_ice_same_pc_renominates_host_candidates_and_returns_to_loopback() {
        use crate::{ice_candidate::IceCandidate, peer_connection::AnswerOptions, stats::RtcStats};
        use tokio::sync::mpsc;
        async fn candidate(
            rx: &mut mpsc::UnboundedReceiver<IceCandidate>,
            protocol: &str,
        ) -> IceCandidate {
            tokio::time::timeout(Duration::from_secs(10), async {
                while let Some(candidate) = rx.recv().await {
                    let sdp = candidate.to_string();
                    let Some(kind) = sdp.split_ascii_whitespace().nth(2) else { continue };
                    if kind.eq_ignore_ascii_case(protocol) {
                        return candidate;
                    }
                }
                panic!("peer stopped reporting candidates");
            })
            .await
            .expect("expected transport candidate")
        }
        async fn selected(pc: &PeerConnection, protocol: &str) {
            tokio::time::timeout(Duration::from_secs(15), async {
                loop {
                    let stats = pc.get_stats().await.unwrap();
                    let pair = stats.iter().find_map(|row| match row {
                        RtcStats::Transport(value) if !value.transport.selected_candidate_pair_id.is_empty() => Some(&value.transport.selected_candidate_pair_id),
                        _ => None,
                    });
                    let local = stats.iter().find_map(|row| match row {
                        RtcStats::CandidatePair(value) if Some(&value.rtc.id) == pair => Some(&value.candidate_pair.local_candidate_id),
                        _ => None,
                    });
                    if stats.iter().any(|row| matches!(row, RtcStats::LocalCandidate(value) if Some(&value.rtc.id) == local && value.local_candidate.protocol == protocol)) {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            }).await.unwrap_or_else(|_| panic!("expected selected {protocol} route"));
        }
        async fn transmit(
            dc: &crate::data_channel::DataChannel,
            messages: &mut mpsc::UnboundedReceiver<Vec<u8>>,
            bytes: &[u8],
        ) {
            dc.send(bytes, true).expect("existing data channel accepts data");
            assert_eq!(
                tokio::time::timeout(Duration::from_secs(5), messages.recv())
                    .await
                    .expect("existing data channel delivers data")
                    .unwrap(),
                bytes
            );
        }

        let config = RtcConfiguration::default();
        let ice = RtcIceOptions {
            tcp_candidate_policy: TcpCandidatePolicy::Enabled,
            enable_ice_renomination: true,
        };
        let left = Factory::with_consent_fenced_ice()
            .create_peer_connection_with_ice_options(config.clone(), ice)
            .unwrap();
        let right = Factory::with_consent_fenced_ice()
            .create_peer_connection_with_ice_options(config, ice)
            .unwrap();
        let (left_tx, mut left_rx) = mpsc::unbounded_channel();
        let (right_tx, mut right_rx) = mpsc::unbounded_channel();
        left.on_ice_candidate(Some(Box::new(move |value| {
            let _ = left_tx.send(value);
        })));
        right.on_ice_candidate(Some(Box::new(move |value| {
            let _ = right_tx.send(value);
        })));
        let (data_tx, mut data_rx) = mpsc::unbounded_channel();
        right.on_data_channel(Some(Box::new(move |value| {
            let _ = data_tx.send(value);
        })));

        let channel = left.create_data_channel("consent-test", DataChannelInit::default()).unwrap();
        let offer = left.create_offer(OfferOptions::default()).await.unwrap();
        left.set_local_description(offer.clone()).await.unwrap();
        right.set_remote_description(offer).await.unwrap();
        let answer = right.create_answer(AnswerOptions::default()).await.unwrap();
        right.set_local_description(answer.clone()).await.unwrap();
        left.set_remote_description(answer).await.unwrap();
        left.add_ice_candidate(candidate(&mut right_rx, "tcp").await).await.unwrap();
        selected(&left, "tcp").await;
        let remote =
            tokio::time::timeout(Duration::from_secs(5), data_rx.recv()).await.unwrap().unwrap();
        let (messages_tx, mut messages_rx) = mpsc::unbounded_channel();
        remote.on_message(Some(Box::new(move |value| {
            let _ = messages_tx.send(value.data.to_vec());
        })));
        tokio::time::timeout(Duration::from_secs(5), async {
            while channel.state() != crate::data_channel::DataChannelState::Open {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .expect("initial data channel opens");
        transmit(&channel, &mut messages_rx, b"private").await;

        assert!(left.set_direct_ice_allowed(true));
        assert!(right.set_direct_ice_allowed(true));
        left.add_ice_candidate(candidate(&mut right_rx, "udp").await).await.unwrap();
        right.add_ice_candidate(candidate(&mut left_rx, "udp").await).await.unwrap();
        selected(&left, "udp").await;
        transmit(&channel, &mut messages_rx, b"direct").await;

        assert!(left.set_direct_ice_allowed(false));
        assert!(right.set_direct_ice_allowed(false));
        selected(&left, "tcp").await;
        transmit(&channel, &mut messages_rx, b"private again").await;
        left.close();
        right.close();
    }

    #[tokio::test]
    async fn test_peer_connection_factory() {
        let _ = env_logger::builder().is_test(true).try_init();

        let factory = PeerConnectionFactory::default();
        let source = NativeVideoSource::default();
        let _track = factory.create_video_track("test", source);
        drop(factory);
    }
}
