/*
 * Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/peer_connection_factory.h"
#include "livekit/hybrid_ice.h"

#include <memory>
#include <utility>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/enable_media.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "livekit/audio_device.h"
#include "livekit/audio_track.h"
#include "livekit/peer_connection.h"
#include "livekit/rtc_error.h"
#include "livekit/rtp_parameters.h"
#include "livekit/video_decoder_factory.h"
#include "livekit/video_encoder_factory.h"
#include "livekit/webrtc.h"
#include "rtc_base/network_constants.h"
#include "rtc_base/thread.h"
#include "webrtc-sys/src/peer_connection.rs.h"
#include "webrtc-sys/src/peer_connection_factory.rs.h"

namespace livekit_ffi {

class PeerConnectionObserver;

PeerConnectionFactory::PeerConnectionFactory(
    std::shared_ptr<RtcRuntime> rtc_runtime, IceNetworkPolicy ice_network_policy)
    : rtc_runtime_(rtc_runtime),
      env_(webrtc::EnvironmentFactory().Create()),
      ice_network_policy_(ice_network_policy),
      loopback_listeners_(ice_network_policy == IceNetworkPolicy::LoopbackOnly ||
                                  ice_network_policy == IceNetworkPolicy::ConsentFenced
                              ? std::make_shared<LoopbackIceListeners>()
                              : nullptr),
      consent_ice_policy_(ice_network_policy == IceNetworkPolicy::ConsentFenced
                              ? std::make_shared<ConsentFencedIcePolicy>()
                              : nullptr) {
  RTC_LOG(LS_VERBOSE) << "PeerConnectionFactory::PeerConnectionFactory()";

  webrtc::PeerConnectionFactoryDependencies dependencies;
  dependencies.network_thread = rtc_runtime_->network_thread();
  dependencies.worker_thread = rtc_runtime_->worker_thread();
  dependencies.signaling_thread = rtc_runtime_->signaling_thread();
  dependencies.socket_factory = rtc_runtime_->network_thread()->socketserver();
  if (consent_ice_policy_) {
    dependencies.network_manager = std::make_unique<HybridIceNetworkManager>(
        env_, dependencies.socket_factory, consent_ice_policy_);
    dependencies.packet_socket_factory = std::make_unique<HybridIcePacketSocketFactory>(
        dependencies.socket_factory, loopback_listeners_, consent_ice_policy_);
  } else if (loopback_listeners_) {
    dependencies.network_manager = std::make_unique<LoopbackIceNetworkManager>();
    dependencies.packet_socket_factory =
        std::make_unique<LoopbackIcePacketSocketFactory>(
            dependencies.socket_factory, loopback_listeners_);
  }
  dependencies.event_log_factory = std::make_unique<webrtc::RtcEventLogFactory>();
  // TODO:
  // dependencies.trials = std::make_unique<webrtc::FieldTrialBasedConfig>();

  audio_device_ = rtc_runtime_->worker_thread()->BlockingCall([&] {
    return webrtc::make_ref_counted<livekit_ffi::AudioDevice>(env_);
  });

  dependencies.adm = audio_device_;

  dependencies.video_encoder_factory =
      std::move(std::make_unique<livekit_ffi::VideoEncoderFactory>());
  dependencies.video_decoder_factory =
      std::move(std::make_unique<livekit_ffi::VideoDecoderFactory>());
  dependencies.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  dependencies.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  dependencies.audio_processing_builder = std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();

  webrtc::EnableMedia(dependencies);
  peer_factory_ =
      webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

  if (peer_factory_.get() == nullptr) {
    RTC_LOG_ERR(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return;
  }

  if (ice_network_policy_ != IceNetworkPolicy::Default) {
    webrtc::PeerConnectionFactoryInterface::Options options;
    options.network_ignore_mask &= ~webrtc::ADAPTER_TYPE_LOOPBACK;
    peer_factory_->SetOptions(options);
  }
}

PeerConnectionFactory::~PeerConnectionFactory() {
  RTC_LOG(LS_VERBOSE) << "PeerConnectionFactory::~PeerConnectionFactory()";

  peer_factory_ = nullptr;
  rtc_runtime_->worker_thread()->BlockingCall(
      [this] { audio_device_ = nullptr; });
}

std::shared_ptr<PeerConnection> PeerConnectionFactory::create_peer_connection(
    RtcConfiguration config,
    rust::Box<PeerConnectionObserverWrapper> observer) const {
  return create_peer_connection_with_ice_options(
      std::move(config), TcpCandidatePolicy::Default, false, std::move(observer));
}

std::shared_ptr<PeerConnection>
PeerConnectionFactory::create_peer_connection_with_ice_options(
    RtcConfiguration config,
    TcpCandidatePolicy tcp_candidate_policy,
    bool enable_ice_renomination,
    rust::Box<PeerConnectionObserverWrapper> observer) const {
  auto native = to_native_rtc_configuration(std::move(config));
  using Interface = webrtc::PeerConnectionInterface;
  if (tcp_candidate_policy == TcpCandidatePolicy::Enabled) {
    native.tcp_candidate_policy = Interface::kTcpCandidatePolicyEnabled;
  } else if (tcp_candidate_policy == TcpCandidatePolicy::Disabled) {
    native.tcp_candidate_policy = Interface::kTcpCandidatePolicyDisabled;
  }
  if (enable_ice_renomination) {
    native.enable_ice_renomination = true;
  }
  if (loopback_listeners_) {
    std::set<webrtc::SocketAddress> stun_dns;
    const bool valid_servers = consent_ice_policy_
                                   ? ParseHybridIceServers(native.servers, stun_dns)
                                   : native.servers.empty();
    if (!valid_servers || private_connection_created_.exchange(true)) {
      throw BridgeException(serialize_error(to_error(webrtc::RTCError(
          webrtc::RTCErrorType::INVALID_PARAMETER,
          "Invalid ICE servers or duplicate isolated peer connection"))));
    }
  }
  std::shared_ptr<PeerConnection> pc = std::make_shared<PeerConnection>(
      rtc_runtime_, peer_factory_, std::move(observer));

  if (!pc->Initialize(std::move(native),
                      loopback_listeners_,
                      tcp_candidate_policy != TcpCandidatePolicy::Default ||
                          enable_ice_renomination,
                      consent_ice_policy_)) {
    throw BridgeException(serialize_error(to_error(webrtc::RTCError(
        webrtc::RTCErrorType::INTERNAL_ERROR, "failed to initialize pc"))));
  }

  return pc;
}

std::shared_ptr<VideoTrack> PeerConnectionFactory::create_video_track(
    rust::String label,
    std::shared_ptr<VideoTrackSource> source) const {
  return std::static_pointer_cast<VideoTrack>(
      rtc_runtime_->get_or_create_media_stream_track(
          peer_factory_->CreateVideoTrack(source->get(), label.c_str())));
}

std::shared_ptr<AudioTrack> PeerConnectionFactory::create_audio_track(
    rust::String label,
    std::shared_ptr<AudioTrackSource> source) const {
  return std::static_pointer_cast<AudioTrack>(
      rtc_runtime_->get_or_create_media_stream_track(
          peer_factory_->CreateAudioTrack(label.c_str(), source->get().get())));
}

RtpCapabilities PeerConnectionFactory::rtp_sender_capabilities(
    MediaType type) const {
  return to_rust_rtp_capabilities(peer_factory_->GetRtpSenderCapabilities(
      static_cast<webrtc::MediaType>(type)));
}

RtpCapabilities PeerConnectionFactory::rtp_receiver_capabilities(
    MediaType type) const {
  return to_rust_rtp_capabilities(peer_factory_->GetRtpReceiverCapabilities(
      static_cast<webrtc::MediaType>(type)));
}

std::shared_ptr<PeerConnectionFactory> create_peer_connection_factory() {
  return std::make_shared<PeerConnectionFactory>(RtcRuntime::create());
}

std::shared_ptr<PeerConnectionFactory>
create_peer_connection_factory_with_loopback_ice() {
  return std::make_shared<PeerConnectionFactory>(
      RtcRuntime::create(), IceNetworkPolicy::IncludeLoopback);
}

std::shared_ptr<PeerConnectionFactory>
create_peer_connection_factory_with_loopback_only_ice() {
  return std::make_shared<PeerConnectionFactory>(
      RtcRuntime::create(), IceNetworkPolicy::LoopbackOnly);
}

std::shared_ptr<PeerConnectionFactory>
create_peer_connection_factory_with_consent_fenced_ice() {
  return std::make_shared<PeerConnectionFactory>(
      RtcRuntime::create(), IceNetworkPolicy::ConsentFenced);
}

}  // namespace livekit_ffi
