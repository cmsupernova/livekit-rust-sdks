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

#include "livekit/peer_connection.h"
#include "livekit/peer_connection_factory.h"
#include "livekit/hybrid_ice.h"

#include <memory>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "livekit/candidate.h"
#include "livekit/data_channel.h"
#include "livekit/jsep.h"
#include "livekit/media_stream.h"
#include "livekit/rtc_error.h"
#include "livekit/rtp_transceiver.h"
#include "rtc_base/logging.h"

namespace livekit_ffi {

webrtc::PeerConnectionInterface::RTCConfiguration to_native_rtc_configuration(
    RtcConfiguration config) {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config{};

  for (auto item : config.ice_servers) {
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.username = item.username.c_str();
    ice_server.password = item.password.c_str();

    for (auto url : item.urls)
      ice_server.urls.emplace_back(url.c_str());

    rtc_config.servers.push_back(ice_server);
  }

  rtc_config.continual_gathering_policy =
      static_cast<webrtc::PeerConnectionInterface::ContinualGatheringPolicy>(
          config.continual_gathering_policy);

  rtc_config.type =
      static_cast<webrtc::PeerConnectionInterface::IceTransportsType>(
          config.ice_transport_type);

  return rtc_config;
}

inline webrtc::PeerConnectionInterface::RTCOfferAnswerOptions
to_native_offer_answer_options(const RtcOfferAnswerOptions& options) {
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions rtc_options;
  rtc_options.offer_to_receive_video = options.offer_to_receive_video;
  rtc_options.offer_to_receive_audio = options.offer_to_receive_audio;
  rtc_options.voice_activity_detection = options.voice_activity_detection;
  rtc_options.ice_restart = options.ice_restart;
  rtc_options.use_rtp_mux = options.use_rtp_mux;
  rtc_options.raw_packetization_for_video = options.raw_packetization_for_video;
  rtc_options.num_simulcast_layers = options.num_simulcast_layers;
  rtc_options.use_obsolete_sctp_sdp = options.use_obsolete_sctp_sdp;
  return rtc_options;
}

PeerConnection::PeerConnection(
    std::shared_ptr<RtcRuntime> rtc_runtime,
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory,
    rust::Box<PeerConnectionObserverWrapper> observer)
    : rtc_runtime_(std::move(rtc_runtime)),
      pc_factory_(std::move(pc_factory)),
      observer_(std::move(observer)) {
  RTC_LOG(LS_VERBOSE) << "PeerConnection::PeerConnection()";
}

PeerConnection::~PeerConnection() {
  RTC_LOG(LS_VERBOSE) << "PeerConnection::~PeerConnection()";
}

bool PeerConnection::Initialize(
    webrtc::PeerConnectionInterface::RTCConfiguration config,
    std::shared_ptr<LoopbackIceListeners> loopback_listeners,
    bool preserve_ice_options,
    std::shared_ptr<ConsentFencedIcePolicy> consent_ice_policy) {
  webrtc::PeerConnectionDependencies deps{this};
  loopback_listeners_ = std::move(loopback_listeners);
  consent_ice_policy_ = std::move(consent_ice_policy);
  preserve_ice_options_ = preserve_ice_options;
  if (consent_ice_policy_) {
    std::set<webrtc::SocketAddress> stun_dns;
    if (!ParseHybridIceServers(config.servers, stun_dns)) return false;
    rtc_runtime_->network_thread()->BlockingCall(
        [&] { consent_ice_policy_->SetStunDns(std::move(stun_dns)); });
    deps.async_dns_resolver_factory = std::make_unique<FencedIceDnsFactory>(consent_ice_policy_);
  } else if (loopback_listeners_) {
    deps.async_dns_resolver_factory = std::make_unique<LoopbackIceDnsFactory>();
  }
  auto result =
      pc_factory_->CreatePeerConnectionOrError(config, std::move(deps));

  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to create peer connection: "
                      << result.error().message();
    return false;
  }
  peer_connection_ = std::move(result.value());
  return true;
}

void PeerConnection::set_configuration(RtcConfiguration config) const {
  auto native = to_native_rtc_configuration(std::move(config));
  std::set<webrtc::SocketAddress> stun_dns;
  if (loopback_listeners_) {
    const bool valid_servers = consent_ice_policy_
                                   ? ParseHybridIceServers(native.servers, stun_dns)
                                   : native.servers.empty();
    if (!valid_servers) {
      throw BridgeException(serialize_error(to_error(webrtc::RTCError(
          webrtc::RTCErrorType::INVALID_PARAMETER,
          "Invalid ICE servers for the isolated peer connection"))));
    }
  }
  bool renew_physical_ports = false;
  if (preserve_ice_options_ || consent_ice_policy_) {
    const auto previous = peer_connection_->GetConfiguration();
    if (preserve_ice_options_) {
      native.tcp_candidate_policy = previous.tcp_candidate_policy;
      native.enable_ice_renomination = previous.enable_ice_renomination;
    }
    renew_physical_ports = consent_ice_policy_ && previous.servers != native.servers;
  }
  if (renew_physical_ports) {
    // Close the old direct allocations before installing their replacement.
    // Only the native consent owner can explicitly reopen the physical gate.
    rtc_runtime_->network_thread()->BlockingCall(
        [&] { consent_ice_policy_->SetAllowed(false); });
  }
  auto result = peer_connection_->SetConfiguration(native);

  if (!result.ok()) {
    throw BridgeException(serialize_error(to_error(result)));
  }
  if (consent_ice_policy_) {
    rtc_runtime_->network_thread()->BlockingCall(
        [&] {
          consent_ice_policy_->SetStunDns(std::move(stun_dns));
        });
  }
}

bool PeerConnection::set_direct_ice_allowed(bool allowed) const {
  if (!consent_ice_policy_) return false;
  return rtc_runtime_->network_thread()->BlockingCall([&] {
    if (loopback_listeners_->retired) return false;
    consent_ice_policy_->SetAllowed(allowed);
    return true;
  });
}

bool PeerConnection::with_loopback_ice_listener(
    uint16_t port,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(uint16_t, rust::Box<PeerContext>)> dial) const {
  if (!loopback_listeners_ || port <= 9) return false;
  // A socket cannot be retired or replaced while this sequence is checking and
  // opening the OS TCP connection. The callback must use a short dial timeout.
  return rtc_runtime_->network_thread()->BlockingCall([&] {
    if (loopback_listeners_->retired) return false;
    auto match = loopback_listeners_->sockets.find(port);
    if (match == loopback_listeners_->sockets.end()) return false;
    const auto* socket = match->second;
    auto address = socket->GetLocalAddress();
    if (socket->GetState() != webrtc::AsyncListenSocket::State::kBound ||
        address.family() != AF_INET || !address.IsLoopbackIP() ||
        address.port() != port) {
      return false;
    }
    dial(port, std::move(ctx));
    return true;
  });
}

#ifdef LIVEKIT_ICE_CONSENT_SELFTEST
namespace {
struct FakeIceIo {
  int closes = 0;
  int sends = 0;
};

class FakeIceSocket final : public webrtc::Socket {
 public:
  explicit FakeIceSocket(FakeIceIo& io) : io_(io) {}
  webrtc::SocketAddress GetLocalAddress() const override { return {"192.0.2.1", 1234}; }
  webrtc::SocketAddress GetRemoteAddress() const override { return {"192.0.2.2", 2345}; }
  int Bind(const webrtc::SocketAddress&) override { return 0; }
  int Connect(const webrtc::SocketAddress&) override { return 0; }
  int Send(const void*, size_t size) override { ++io_.sends; return static_cast<int>(size); }
  int SendTo(const void* data, size_t size, const webrtc::SocketAddress&) override { return Send(data, size); }
  int Recv(void*, size_t, int64_t*) override { return -1; }
  int Listen(int) override { return 0; }
  webrtc::Socket* Accept(webrtc::SocketAddress* address) override {
    *address = GetRemoteAddress();
    return new FakeIceSocket(io_);
  }
  int Close() override { ++io_.closes; return 0; }
  int GetError() const override { return error_; }
  void SetError(int error) override { error_ = error; }
  ConnState GetState() const override { return CS_CONNECTED; }
  int GetOption(Option, int*) override { return 0; }
  int SetOption(Option, int) override { return 0; }

 private:
  FakeIceIo& io_;
  int error_ = 0;
};
}  // namespace

extern "C" bool livekit_test_fenced_ice_accepted_socket_epoch() {
  auto policy = std::make_shared<ConsentFencedIcePolicy>();
  const webrtc::SocketAddress stun("stun.example", 3478);
  const webrtc::SocketAddress turns("turn.example", 443);
  webrtc::SocketAddress resolved;
  if (!PreserveIceServerHostname(turns, {"192.0.2.3", 443}, &resolved) ||
      resolved.hostname() != "turn.example" || resolved.IsUnresolvedIP() ||
      PreserveIceServerHostname(turns, {"127.0.0.1", 443}, &resolved) ||
      PreserveIceServerHostname(turns, {"192.0.2.3", 3478}, &resolved)) return false;
  policy->SetStunDns({stun, turns});
  if (policy->allowed() || policy->AllowsStunDns(stun, policy->epoch())) return false;
  policy->SetAllowed(true);
  const uint64_t old_epoch = policy->epoch();
  if (!policy->AllowsStunDns(stun, old_epoch) || !policy->AllowsStunDns(turns, old_epoch) ||
      policy->AllowsStunDns({"other.example", 3478}, old_epoch) ||
      policy->AllowsStunDns({"turn.example", 3478}, old_epoch)) return false;
  FakeIceIo io;
  FencedIceSocket listener(std::make_unique<FakeIceSocket>(io), policy, policy->epoch());
  auto accepted = std::unique_ptr<webrtc::Socket>(listener.Accept(nullptr));
  if (!accepted || accepted->Send("x", 1) != 1) return false;
  bool fenced_before_close = false;
  accepted->SubscribeCloseEvent(&fenced_before_close, [&](webrtc::Socket* socket, int) {
    fenced_before_close = !policy->allowed() && socket->Send("y", 1) == -1;
  });
  policy->SetAllowed(false);
  if (io.closes != 2 || !fenced_before_close || accepted->GetState() != webrtc::Socket::CS_CLOSED) return false;
  policy->SetAllowed(true);
  if (accepted->Send("y", 1) != -1 || listener.Accept(nullptr) || io.sends != 1 ||
      policy->AllowsStunDns(stun, old_epoch) || !policy->AllowsStunDns(stun, policy->epoch())) return false;
  policy->SetStunDns({});
  if (policy->AllowsStunDns(stun, policy->epoch())) return false;
  FencedIceSocket current(std::make_unique<FakeIceSocket>(io), policy, policy->epoch());
  return current.Send("z", 1) == 1 && io.sends == 2;
}
#endif

void PeerConnection::set_bitrate(int32_t min_bitrate_bps,
                                 int32_t start_bitrate_bps,
                                 int32_t max_bitrate_bps) const {
  // Call-level bitrate constraints for the bandwidth estimator / allocator.
  // Values <= 0 leave the corresponding constraint untouched.
  webrtc::BitrateSettings settings;
  if (min_bitrate_bps > 0)
    settings.min_bitrate_bps = min_bitrate_bps;
  if (start_bitrate_bps > 0)
    settings.start_bitrate_bps = start_bitrate_bps;
  if (max_bitrate_bps > 0)
    settings.max_bitrate_bps = max_bitrate_bps;

  auto error = peer_connection_->SetBitrate(settings);
  if (!error.ok()) {
    throw BridgeException(serialize_error(to_error(error)));
  }
}

void PeerConnection::create_offer(
    RtcOfferAnswerOptions options,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, std::unique_ptr<SessionDescription>)>
        on_success,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_error) const {
  webrtc::scoped_refptr<NativeCreateSdpObserver> observer =
      webrtc::make_ref_counted<NativeCreateSdpObserver>(std::move(ctx), on_success,
                                                     on_error);

  peer_connection_->CreateOffer(observer.get(),
                                to_native_offer_answer_options(options));
}

void PeerConnection::create_answer(
    RtcOfferAnswerOptions options,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, std::unique_ptr<SessionDescription>)>
        on_success,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_error) const {
  webrtc::scoped_refptr<NativeCreateSdpObserver> observer =
      webrtc::make_ref_counted<NativeCreateSdpObserver>(std::move(ctx), on_success,
                                                     on_error);

  peer_connection_->CreateAnswer(observer.get(),
                                 to_native_offer_answer_options(options));
}

void PeerConnection::set_local_description(
    std::unique_ptr<SessionDescription> desc,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_complete) const {
  webrtc::scoped_refptr<NativeSetLocalSdpObserver> observer =
      webrtc::make_ref_counted<NativeSetLocalSdpObserver>(std::move(ctx),
                                                       on_complete);

  peer_connection_->SetLocalDescription(desc->clone()->release(), observer);
}

void PeerConnection::set_remote_description(
    std::unique_ptr<SessionDescription> desc,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_complete) const {
  webrtc::scoped_refptr<NativeSetRemoteSdpObserver> observer =
      webrtc::make_ref_counted<NativeSetRemoteSdpObserver>(std::move(ctx),
                                                        on_complete);

  peer_connection_->SetRemoteDescription(desc->clone()->release(), observer);
}

void PeerConnection::restart_ice() const {
  peer_connection_->RestartIce();
}

void PeerConnection::add_ice_candidate(
    std::shared_ptr<IceCandidate> candidate,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_complete) const {
  // The completion can run after this function returns (upstream #1392).
  auto owned_ctx = std::make_shared<rust::Box<PeerContext>>(std::move(ctx));
  peer_connection_->AddIceCandidate(
      candidate->release(), [owned_ctx, on_complete](const webrtc::RTCError& err) {
        on_complete(std::move(*owned_ctx), to_error(err));
      });
}

std::shared_ptr<DataChannel> PeerConnection::create_data_channel(
    rust::String label,
    DataChannelInit init) const {
  webrtc::DataChannelInit rtc_init = to_native_data_channel_init(init);
  auto result =
      peer_connection_->CreateDataChannelOrError(label.c_str(), &rtc_init);

  if (!result.ok()) {
    throw BridgeException(serialize_error(to_error(result.error())));
  }

  return std::make_shared<DataChannel>(rtc_runtime_, result.value());
}

std::shared_ptr<RtpSender> PeerConnection::add_track(
    std::shared_ptr<MediaStreamTrack> track,
    const rust::Vec<rust::String>& stream_ids) const {
  std::vector<std::string> std_stream_ids(stream_ids.begin(), stream_ids.end());
  auto result = peer_connection_->AddTrack(track->rtc_track(), std_stream_ids);
  if (!result.ok()) {
    throw BridgeException(serialize_error(to_error(result.error())));
  }

  return std::make_shared<RtpSender>(rtc_runtime_, result.value(),
                                     peer_connection_);
}

void PeerConnection::remove_track(std::shared_ptr<RtpSender> sender) const {
  auto error = peer_connection_->RemoveTrackOrError(sender->rtc_sender());
  if (!error.ok())
    throw BridgeException(serialize_error(to_error(error)));
}

void PeerConnection::get_stats(
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, rust::String)> on_stats) const {
  auto observer = webrtc::make_ref_counted<NativeRtcStatsCollector<PeerContext>>(
      std::move(ctx), on_stats);
  peer_connection_->GetStats(observer.get());
}

std::shared_ptr<RtpTransceiver> PeerConnection::add_transceiver(
    std::shared_ptr<MediaStreamTrack> track,
    RtpTransceiverInit init) const {
  auto result = peer_connection_->AddTransceiver(
      track->rtc_track(), to_native_rtp_transceiver_init(init));
  if (!result.ok())
    throw BridgeException(serialize_error(to_error(result.error())));

  return std::make_shared<RtpTransceiver>(rtc_runtime_, result.value(),
                                          peer_connection_);
}

std::shared_ptr<RtpTransceiver> PeerConnection::add_transceiver_for_media(
    MediaType media_type,
    RtpTransceiverInit init) const {
  auto result = peer_connection_->AddTransceiver(
      static_cast<webrtc::MediaType>(media_type),
      to_native_rtp_transceiver_init(init));

  if (!result.ok())
    throw BridgeException(serialize_error(to_error(result.error())));

  return std::make_shared<RtpTransceiver>(rtc_runtime_, result.value(),
                                          peer_connection_);
}

rust::Vec<RtpSenderPtr> PeerConnection::get_senders() const {
  rust::Vec<RtpSenderPtr> vec;
  for (auto sender : peer_connection_->GetSenders())
    vec.push_back(RtpSenderPtr{
        std::make_shared<RtpSender>(rtc_runtime_, sender, peer_connection_)});

  return vec;
}

rust::Vec<RtpReceiverPtr> PeerConnection::get_receivers() const {
  rust::Vec<RtpReceiverPtr> vec;
  for (auto receiver : peer_connection_->GetReceivers())
    vec.push_back(RtpReceiverPtr{std::make_shared<RtpReceiver>(
        rtc_runtime_, receiver, peer_connection_)});

  return vec;
}

rust::Vec<RtpTransceiverPtr> PeerConnection::get_transceivers() const {
  rust::Vec<RtpTransceiverPtr> vec;
  for (auto transceiver : peer_connection_->GetTransceivers())
    vec.push_back(RtpTransceiverPtr{std::make_shared<RtpTransceiver>(
        rtc_runtime_, transceiver, peer_connection_)});

  return vec;
}

std::unique_ptr<SessionDescription> PeerConnection::current_local_description()
    const {
  auto local_description = peer_connection_->current_local_description();
  if (local_description)
    return std::make_unique<SessionDescription>(local_description->Clone());

  return nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::current_remote_description()
    const {
  auto remote_description = peer_connection_->current_remote_description();
  if (remote_description)
    return std::make_unique<SessionDescription>(remote_description->Clone());

  return nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::pending_local_description()
    const {
  auto local_description = peer_connection_->pending_local_description();
  if (local_description)
    return std::make_unique<SessionDescription>(local_description->Clone());

  return nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::pending_remote_description()
    const {
  auto remote_description = peer_connection_->pending_remote_description();
  if (remote_description)
    return std::make_unique<SessionDescription>(remote_description->Clone());

  return nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::local_description() const {
  auto local_description = peer_connection_->local_description();
  if (local_description)
    return std::make_unique<SessionDescription>(local_description->Clone());

  return nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::remote_description() const {
  auto remote_description = peer_connection_->remote_description();
  if (remote_description)
    return std::make_unique<SessionDescription>(remote_description->Clone());

  return nullptr;
}

PeerConnectionState PeerConnection::connection_state() const {
  return static_cast<PeerConnectionState>(
      peer_connection_->peer_connection_state());
}

SignalingState PeerConnection::signaling_state() const {
  return static_cast<SignalingState>(peer_connection_->signaling_state());
}

IceGatheringState PeerConnection::ice_gathering_state() const {
  return static_cast<IceGatheringState>(
      peer_connection_->ice_gathering_state());
}

IceConnectionState PeerConnection::ice_connection_state() const {
  return static_cast<IceConnectionState>(
      peer_connection_->ice_connection_state());
}

void PeerConnection::close() const {
  if (loopback_listeners_) {
    rtc_runtime_->network_thread()->BlockingCall([&] {
      loopback_listeners_->retired = true;
      if (consent_ice_policy_) consent_ice_policy_->SetAllowed(false);
    });
  }
  peer_connection_->Close();
}

// PeerConnectionObserver

void PeerConnection::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  observer_->on_signaling_change(static_cast<SignalingState>(new_state));
}

void PeerConnection::OnAddStream(
    webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
  observer_->on_add_stream(std::make_unique<MediaStream>(rtc_runtime_, stream));
}

void PeerConnection::OnRemoveStream(
    webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
  // Find current MediaStream
  // observer_->on_remove_stream(std::make_unique<MediaStream>(rtc_runtime_,
  // stream));
}

void PeerConnection::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  observer_->on_data_channel(
      std::make_shared<DataChannel>(rtc_runtime_, data_channel));
}

void PeerConnection::OnRenegotiationNeeded() {
  observer_->on_renegotiation_needed();
}

void PeerConnection::OnNegotiationNeededEvent(uint32_t event_id) {
  observer_->on_negotiation_needed_event(event_id);
}

void PeerConnection::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  observer_->on_ice_connection_change(
      static_cast<IceConnectionState>(new_state));
}

void PeerConnection::OnStandardizedIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  observer_->on_standardized_ice_connection_change(
      static_cast<IceConnectionState>(new_state));
}

void PeerConnection::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  observer_->on_connection_change(static_cast<PeerConnectionState>(new_state));
}

void PeerConnection::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  observer_->on_ice_gathering_change(static_cast<IceGatheringState>(new_state));
}

void PeerConnection::OnIceCandidate(
    const webrtc::IceCandidate* candidate) {
  auto new_candidate = webrtc::CreateIceCandidate(candidate->sdp_mid(),
                                                  candidate->sdp_mline_index(),
                                                  candidate->candidate());
  observer_->on_ice_candidate(
      std::make_unique<IceCandidate>(std::move(new_candidate)));
}

void PeerConnection::OnIceCandidateError(const std::string& address,
                                         int port,
                                         const std::string& url,
                                         int error_code,
                                         const std::string& error_text) {
  observer_->on_ice_candidate_error(address, port, url, error_code, error_text);
}

void PeerConnection::OnIceCandidateRemoved(const webrtc::IceCandidate* ice_candidate) {
  rust::Vec<CandidatePtr> vec;
  if(ice_candidate != nullptr) {
    vec.push_back(CandidatePtr{std::make_unique<Candidate>(ice_candidate->candidate())});
  }
  observer_->on_ice_candidates_removed(std::move(vec));
}

void PeerConnection::OnIceConnectionReceivingChange(bool receiving) {
  observer_->on_ice_connection_receiving_change(receiving);
}

void PeerConnection::OnIceSelectedCandidatePairChanged(
    const webrtc::CandidatePairChangeEvent& event) {
  CandidatePairChangeEvent e{};
  e.selected_candidate_pair.local =
      std::make_unique<Candidate>(event.selected_candidate_pair.local);
  e.selected_candidate_pair.remote =
      std::make_unique<Candidate>(event.selected_candidate_pair.remote);
  e.last_data_received_ms = event.last_data_received_ms;
  e.reason = event.reason;
  e.estimated_disconnected_time_ms = event.estimated_disconnected_time_ms;

  observer_->on_ice_selected_candidate_pair_changed(std::move(e));
}

void PeerConnection::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  rust::Vec<MediaStreamPtr> vec;

  for (const auto& item : streams) {
    vec.push_back(
        MediaStreamPtr{std::make_unique<MediaStream>(rtc_runtime_, item)});
  }

  observer_->on_add_track(
      std::make_unique<RtpReceiver>(rtc_runtime_, receiver, peer_connection_),
      std::move(vec));
}

void PeerConnection::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  observer_->on_track(std::make_unique<RtpTransceiver>(
      rtc_runtime_, transceiver, peer_connection_));
}

void PeerConnection::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  observer_->on_remove_track(
      std::make_unique<RtpReceiver>(rtc_runtime_, receiver, peer_connection_));
}

void PeerConnection::OnInterestingUsage(int usage_pattern) {
  observer_->on_interesting_usage(usage_pattern);
}

}  // namespace livekit_ffi
