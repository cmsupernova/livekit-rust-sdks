/*
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

#pragma once

#include <cerrno>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "api/async_dns_resolver.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/network.h"
#include "rtc_base/thread.h"

namespace livekit_ffi {

// Do not enumerate physical interfaces, use an OS network monitor, or permit
// libwebrtc to fall back to a wildcard bind.
class LoopbackIceNetworkManager final : public webrtc::NetworkManagerBase {
 public:
  void StartUpdating() override {
    if (!initialized_) {
      SeedNetworks();
      initialized_ = true;
    }
    webrtc::Thread::Current()->PostTask(
        webrtc::SafeTask(safety_.flag(), [this] { NotifyNetworksChanged(); }));
  }

  void StopUpdating() override {}

  std::vector<const webrtc::Network*> GetAnyAddressNetworks() override {
    return {};
  }

 private:
  void SeedNetworks() {
    std::vector<std::unique_ptr<webrtc::Network>> networks;
    for (const auto& [text, prefix_length] :
         {std::pair{"127.0.0.1", 8}, std::pair{"::1", 128}}) {
      webrtc::IPAddress ip;
      if (!webrtc::IPFromString(text, &ip)) continue;
      auto network = CreateNetwork("loopback", "ICE loopback",
                                   webrtc::TruncateIP(ip, prefix_length),
                                   prefix_length, webrtc::ADAPTER_TYPE_LOOPBACK);
      network->AddIP(webrtc::InterfaceAddress(ip));
      networks.push_back(std::move(network));
    }
    bool changed = false;
    MergeNetworkList(std::move(networks), &changed);
  }

  bool initialized_ = false;
  webrtc::ScopedTaskSafetyDetached safety_;
};

// ICE can request hostname resolution through either the packet factory or
// the peer connection's independent DNS resolver factory. Refuse both paths.
class LoopbackIceDnsResolver final : public webrtc::AsyncDnsResolverInterface,
                                     private webrtc::AsyncDnsResolverResult {
 public:
  void Start(const webrtc::SocketAddress&,
             absl::AnyInvocable<void()> callback) override {
    webrtc::Thread::Current()->PostTask(webrtc::SafeTask(
        safety_.flag(), [callback = std::move(callback)]() mutable { callback(); }));
  }

  void Start(const webrtc::SocketAddress& address,
             int,
             absl::AnyInvocable<void()> callback) override {
    Start(address, std::move(callback));
  }

  const webrtc::AsyncDnsResolverResult& result() const override { return *this; }

 private:
  bool GetResolvedAddress(int, webrtc::SocketAddress*) const override {
    return false;
  }

  int GetError() const override { return EACCES; }

  webrtc::ScopedTaskSafety safety_;
};

class LoopbackIceDnsFactory final
    : public webrtc::AsyncDnsResolverFactoryInterface {
 public:
  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAndResolve(
      const webrtc::SocketAddress& address,
      absl::AnyInvocable<void()> callback) override {
    auto resolver = Create();
    resolver->Start(address, std::move(callback));
    return resolver;
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAndResolve(
      const webrtc::SocketAddress& address,
      int family,
      absl::AnyInvocable<void()> callback) override {
    auto resolver = Create();
    resolver->Start(address, family, std::move(callback));
    return resolver;
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> Create() override {
    return std::make_unique<LoopbackIceDnsResolver>();
  }
};

// Each private PeerConnection has an exclusive factory. Access to this table
// is confined to the WebRTC network thread that owns the listening sockets.
struct LoopbackIceListeners {
  std::map<uint16_t, const webrtc::AsyncListenSocket*> sockets;
  bool retired = false;
};

class OwnedLoopbackIceListener final : public webrtc::AsyncListenSocket {
 public:
  OwnedLoopbackIceListener(
      std::unique_ptr<webrtc::AsyncListenSocket> socket,
      std::shared_ptr<LoopbackIceListeners> listeners)
      : socket_(std::move(socket)),
        listeners_(std::move(listeners)),
        port_(static_cast<uint16_t>(socket_->GetLocalAddress().port())) {
    listeners_->sockets.emplace(port_, this);
    socket_->SubscribeNewConnection(
        this, [this](webrtc::AsyncListenSocket*, webrtc::AsyncPacketSocket* peer) {
          NotifyNewConnection(this, peer);
        });
  }

  ~OwnedLoopbackIceListener() override {
    auto match = listeners_->sockets.find(port_);
    if (match != listeners_->sockets.end() && match->second == this) {
      listeners_->sockets.erase(match);
    }
    socket_->UnsubscribeNewConnection(this);
  }

  State GetState() const override { return socket_->GetState(); }

  webrtc::SocketAddress GetLocalAddress() const override {
    return socket_->GetLocalAddress();
  }

 private:
  std::unique_ptr<webrtc::AsyncListenSocket> socket_;
  std::shared_ptr<LoopbackIceListeners> listeners_;
  uint16_t port_;
};

class LoopbackIcePacketSocketFactory final
    : public webrtc::BasicPacketSocketFactory {
 public:
  LoopbackIcePacketSocketFactory(
      webrtc::SocketFactory* socket_factory,
      std::shared_ptr<LoopbackIceListeners> listeners)
      : BasicPacketSocketFactory(socket_factory),
        listeners_(std::move(listeners)) {}

  std::unique_ptr<webrtc::AsyncPacketSocket> CreateUdpSocket(
      const webrtc::Environment&,
      const webrtc::SocketAddress&,
      uint16_t,
      uint16_t) override {
    return nullptr;
  }

  std::unique_ptr<webrtc::AsyncListenSocket> CreateServerTcpSocket(
      const webrtc::Environment& env,
      const webrtc::SocketAddress& address,
      uint16_t min_port,
      uint16_t max_port,
      int opts) override {
    if (!NumericLoopback(address)) return nullptr;
    auto socket = BasicPacketSocketFactory::CreateServerTcpSocket(
        env, address, min_port, max_port, opts);
    if (!socket || address.family() != AF_INET) return socket;
    const auto bound = socket->GetLocalAddress();
    if (!NumericLoopback(bound) || bound.ipaddr() != address.ipaddr() ||
        bound.port() <= 9) {
      return nullptr;
    }
    return std::make_unique<OwnedLoopbackIceListener>(std::move(socket),
                                                      listeners_);
  }

  std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientTcpSocket(
      const webrtc::Environment& env,
      const webrtc::SocketAddress& local,
      const webrtc::SocketAddress& remote,
      const webrtc::PacketSocketTcpOptions& opts) override {
    if (!NumericLoopback(local) || !NumericLoopback(remote)) return nullptr;
    return BasicPacketSocketFactory::CreateClientTcpSocket(env, local, remote,
                                                           opts);
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface>
  CreateAsyncDnsResolver() override {
    return std::make_unique<LoopbackIceDnsResolver>();
  }

 private:
  std::shared_ptr<LoopbackIceListeners> listeners_;

  static bool NumericLoopback(const webrtc::SocketAddress& address) {
    return !address.IsUnresolvedIP() && address.IsLoopbackIP();
  }
};

}  // namespace livekit_ffi
