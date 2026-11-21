
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
#include <functional>
#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "pc/ice_server_parsing.h"
#include "livekit/loopback_ice.h"
#include "rtc_base/async_dns_resolver.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_factory.h"

namespace livekit_ffi {

class FencedIceSocket;
class FencedIceDnsResolver;

class ConsentFencedIcePolicy {
 public:
  // This object is used only on the owning WebRTC network sequence.
  bool allowed() const { return allowed_; }
  uint64_t epoch() const { return epoch_; }
  bool permits(uint64_t epoch) const { return allowed_ && epoch_ == epoch; }
  void SetAllowed(bool allowed);
  void SetStunDns(std::set<webrtc::SocketAddress> servers);
  bool AllowsStunDns(const webrtc::SocketAddress& server, uint64_t epoch) const {
    return permits(epoch) && server.IsUnresolvedIP() && stun_dns_.contains(server);
  }
  void RegisterDns(FencedIceDnsResolver* resolver) { resolvers_.insert(resolver); }
  void UnregisterDns(FencedIceDnsResolver* resolver) { resolvers_.erase(resolver); }
  void Register(FencedIceSocket* socket) { sockets_.insert(socket); }
  void Unregister(FencedIceSocket* socket) { sockets_.erase(socket); }
  void SetNetworkChange(std::function<void(bool)> change) {
    network_change_ = std::move(change);
  }

 private:
  bool allowed_ = false;
  uint64_t epoch_ = 0;
  void CancelDns();
  std::set<FencedIceSocket*> sockets_;
  std::set<webrtc::SocketAddress> stun_dns_;
  std::set<FencedIceDnsResolver*> resolvers_;
  std::function<void(bool)> network_change_;
};

inline bool PhysicalIceEndpoint(const webrtc::SocketAddress& address) {
  const webrtc::SocketAddress normalized(address.ipaddr().Normalized(), address.port());
  return !address.IsUnresolvedIP() && !normalized.IsNil() &&
         !normalized.IsAnyIP() && !normalized.IsLoopbackIP();
}

inline bool ParseHybridIceServers(
    const webrtc::PeerConnectionInterface::IceServers& servers,
    std::set<webrtc::SocketAddress>& stun_dns) {
  if (servers.empty()) return true;
  std::set<webrtc::SocketAddress> stun;
  std::vector<webrtc::RelayServerConfig> turn;
  if (!webrtc::ParseIceServersOrError(servers, &stun, &turn).ok()) return false;
  const auto accept = [&](const webrtc::SocketAddress& endpoint) {
    if (PhysicalIceEndpoint(endpoint)) return true;
    const auto& host = endpoint.hostname();
    if (!endpoint.IsUnresolvedIP() || host.size() > 253 || host.find('.') == std::string::npos ||
        !std::all_of(host.begin(), host.end(), [](char ch) {
          return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
        })) {
      return false;
    }
    stun_dns.insert(endpoint);
    return true;
  };
  for (const auto& endpoint : stun) {
    if (!accept(endpoint)) return false;
  }
  for (const auto& relay : turn) {
    if (relay.tls_cert_policy != webrtc::TlsCertPolicy::TLS_CERT_POLICY_SECURE) return false;
    for (const auto& endpoint : relay.ports) {
      if (!accept(endpoint.address)) return false;
    }
  }
  return !stun.empty() || !turn.empty();
}


inline bool PreserveIceServerHostname(const webrtc::SocketAddress& configured,
                                      const webrtc::SocketAddress& resolved,
                                      webrtc::SocketAddress* result) {
  if (configured.port() != resolved.port() || !PhysicalIceEndpoint(resolved)) return false;
  // TLS TURN uses hostname() for certificate verification and SNI, while
  // the socket connects to the independently validated numeric address.
  *result = configured;
  result->SetResolvedIP(resolved.ipaddr());
  return true;
}

class FencedIceDnsResolver final : public webrtc::AsyncDnsResolverInterface,
                                   private webrtc::AsyncDnsResolverResult {
 public:
  explicit FencedIceDnsResolver(std::shared_ptr<ConsentFencedIcePolicy> policy)
      : policy_(std::move(policy)) {
    policy_->RegisterDns(this);
  }

  ~FencedIceDnsResolver() override { policy_->UnregisterDns(this); }

  void Start(const webrtc::SocketAddress& address,
             absl::AnyInvocable<void()> callback) override {
    Start(address, AF_UNSPEC, std::move(callback));
  }

  void Start(const webrtc::SocketAddress& address, int family,
             absl::AnyInvocable<void()> callback) override {
    query_ = address;
    epoch_ = policy_->epoch();
    callback_ = std::move(callback);
    if (!Allowed()) {
      Revoke();
      return;
    }
    // Use an absolute query so local search suffixes cannot change the
    // authenticated STUN hostname supplied by the call owner.
    auto host = address.hostname();
    if (!host.ends_with('.')) host += '.';
    resolver_ = std::make_unique<webrtc::AsyncDnsResolver>();
    resolver_->Start(webrtc::SocketAddress(host, address.port()), family, [this] {
      auto callback = std::move(callback_);
      if (callback) callback();
    });
  }

  const webrtc::AsyncDnsResolverResult& result() const override { return *this; }

  void Revoke() {
    resolver_.reset();
    if (!callback_) return;
    webrtc::Thread::Current()->PostTask(webrtc::SafeTask(
        safety_.flag(), [callback = std::move(callback_)]() mutable { callback(); }));
  }

 private:
  bool Allowed() const { return query_ && policy_->AllowsStunDns(*query_, epoch_); }

  int GetError() const override {
    return Allowed() && resolver_ ? resolver_->result().GetError() : EACCES;
  }

  bool GetResolvedAddress(int family, webrtc::SocketAddress* address) const override {
    webrtc::SocketAddress resolved;
    return Allowed() && resolver_ && resolver_->result().GetResolvedAddress(family, &resolved) &&
           PreserveIceServerHostname(*query_, resolved, address);
  }

  std::shared_ptr<ConsentFencedIcePolicy> policy_;
  std::optional<webrtc::SocketAddress> query_;
  uint64_t epoch_ = 0;
  absl::AnyInvocable<void()> callback_;
  std::unique_ptr<webrtc::AsyncDnsResolver> resolver_;
  webrtc::ScopedTaskSafety safety_;
};

class FencedIceDnsFactory final : public webrtc::AsyncDnsResolverFactoryInterface {
 public:
  explicit FencedIceDnsFactory(std::shared_ptr<ConsentFencedIcePolicy> policy)
      : policy_(std::move(policy)) {}

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAndResolve(
      const webrtc::SocketAddress& address, absl::AnyInvocable<void()> callback) override {
    auto resolver = Create();
    resolver->Start(address, std::move(callback));
    return resolver;
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAndResolve(
      const webrtc::SocketAddress& address, int family, absl::AnyInvocable<void()> callback) override {
    auto resolver = Create();
    resolver->Start(address, family, std::move(callback));
    return resolver;
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> Create() override {
    return std::make_unique<FencedIceDnsResolver>(policy_);
  }

 private:
  std::shared_ptr<ConsentFencedIcePolicy> policy_;
};

// The physical socket path never uses a loopback or wildcard address.
// Accepted sockets inherit the listener epoch, never the latest policy.
class FencedIceSocket final : public webrtc::Socket {
 public:
  FencedIceSocket(std::unique_ptr<webrtc::Socket> socket,
                  std::shared_ptr<ConsentFencedIcePolicy> policy,
                  uint64_t epoch)
      : socket_(std::move(socket)), policy_(std::move(policy)), epoch_(epoch) {
    policy_->Register(this);
    socket_->SubscribeReadEvent(this, [this](webrtc::Socket*) {
      if (Live()) NotifyReadEvent(this);
    });
    socket_->SubscribeWriteEvent(this, [this](webrtc::Socket*) {
      if (Live()) NotifyWriteEvent(this);
    });
    socket_->SubscribeConnectEvent(this, [this](webrtc::Socket*) {
      if (Live()) NotifyConnectEvent(this);
    });
    socket_->SubscribeCloseEvent(this, [this](webrtc::Socket*, int error) {
      if (Live()) NotifyCloseEvent(this, error);
    });
  }

  ~FencedIceSocket() override {
    policy_->Unregister(this);
    socket_->UnsubscribeReadEvent(this);
    socket_->UnsubscribeWriteEvent(this);
    socket_->UnsubscribeConnectEvent(this);
    socket_->UnsubscribeCloseEvent(this);
  }

  void Revoke() {
    if (revoked_) return;
    revoked_ = true;
    socket_->Close();
    NotifyCloseEvent(this, EACCES);
  }

  webrtc::SocketAddress GetLocalAddress() const override {
    return socket_->GetLocalAddress();
  }

  webrtc::SocketAddress GetRemoteAddress() const override {
    return socket_->GetRemoteAddress();
  }

  int Bind(const webrtc::SocketAddress& address) override {
    return Live() && PhysicalIceEndpoint(address) ? socket_->Bind(address) : Deny();
  }

  int Connect(const webrtc::SocketAddress& address) override {
    // Do not let an implicit wildcard bind bypass the allocation decision.
    return Live() && PhysicalIceEndpoint(address) && PhysicalIceEndpoint(GetLocalAddress())
        ? socket_->Connect(address) : Deny();
  }

  int Send(const void* data, size_t size) override {
    return Live() ? socket_->Send(data, size) : Deny();
  }

  int SendTo(const void* data, size_t size, const webrtc::SocketAddress& address) override {
    return Live() && PhysicalIceEndpoint(address) && PhysicalIceEndpoint(GetLocalAddress())
        ? socket_->SendTo(data, size, address) : Deny();
  }

  int Recv(void* data, size_t size, int64_t* timestamp) override {
    return Live() ? socket_->Recv(data, size, timestamp) : Deny();
  }

  int RecvFrom(void* data, size_t size, webrtc::SocketAddress* address, int64_t* timestamp) override {
    return Live() ? socket_->RecvFrom(data, size, address, timestamp) : Deny();
  }

  int RecvFrom(ReceiveBuffer& buffer) override {
    return Live() ? socket_->RecvFrom(buffer) : Deny();
  }

  int Listen(int backlog) override {
    return Live() && PhysicalIceEndpoint(GetLocalAddress()) ? socket_->Listen(backlog) : Deny();
  }

  webrtc::Socket* Accept(webrtc::SocketAddress* address) override {
    if (!Live()) { Deny(); return nullptr; }
    webrtc::SocketAddress peer;
    auto socket = std::unique_ptr<webrtc::Socket>(socket_->Accept(&peer));
    if (!socket) return nullptr;
    if (!PhysicalIceEndpoint(peer)) { Deny(); return nullptr; }
    if (address) *address = peer;
    return new FencedIceSocket(std::move(socket), policy_, epoch_);
  }

  int Close() override { return socket_->Close(); }

  int GetError() const override { return Live() ? socket_->GetError() : EACCES; }

  void SetError(int error) override { socket_->SetError(error); }

  ConnState GetState() const override { return Live() ? socket_->GetState() : CS_CLOSED; }

  int GetOption(Option option, int* value) override {
    return socket_->GetOption(option, value);
  }

  int SetOption(Option option, int value) override {
    return socket_->SetOption(option, value);
  }

 private:
  bool Live() const { return !revoked_ && policy_->permits(epoch_); }

  int Deny() {
    socket_->SetError(EACCES);
    return -1;
  }

  std::unique_ptr<webrtc::Socket> socket_;
  std::shared_ptr<ConsentFencedIcePolicy> policy_;
  const uint64_t epoch_;
  bool revoked_ = false;
};

inline void ConsentFencedIcePolicy::CancelDns() {
  for (auto* resolver : resolvers_) resolver->Revoke();
}

inline void ConsentFencedIcePolicy::SetStunDns(std::set<webrtc::SocketAddress> servers) {
  if (stun_dns_ == servers) return;
  stun_dns_ = std::move(servers);
  CancelDns();
}

inline void ConsentFencedIcePolicy::SetAllowed(bool allowed) {
  if (allowed == allowed_) return;
  // Make all callbacks stale before closing any OS socket. Close callbacks
  // can destroy a different socket, so recheck its registration first.
  allowed_ = allowed;
  ++epoch_;
  if (!allowed_) {
    CancelDns();
    const std::vector<FencedIceSocket*> previous(sockets_.begin(), sockets_.end());
    for (auto* socket : previous) {
      if (sockets_.contains(socket)) socket->Revoke();
    }
  }
  if (network_change_) network_change_(allowed_);
}

class FencedIceSocketFactory final : public webrtc::SocketFactory {
 public:
  FencedIceSocketFactory(webrtc::SocketFactory* factory,
                         std::shared_ptr<ConsentFencedIcePolicy> policy)
      : factory_(factory), policy_(std::move(policy)) {}

  webrtc::Socket* CreateSocket(int family, int type) override {
    if (!policy_->allowed()) return nullptr;
    auto socket = factory_->Create(family, type);
    if (!socket) return nullptr;
    return new FencedIceSocket(std::move(socket), policy_, policy_->epoch());
  }

 private:
  webrtc::SocketFactory* factory_;
  std::shared_ptr<ConsentFencedIcePolicy> policy_;
};

class HybridIceNetworkManager final : public webrtc::NetworkManager {
 public:
  HybridIceNetworkManager(const webrtc::Environment& env, webrtc::SocketFactory* factory,
                          std::shared_ptr<ConsentFencedIcePolicy> policy)
      : policy_(std::move(policy)), physical_sockets_(factory, policy_),
        physical_(env, &physical_sockets_) {}

  ~HybridIceNetworkManager() override {
    policy_->SetNetworkChange({});
  }

  void StartUpdating() override {
    if (users_++ == 0) {
      loopback_.SubscribeNetworksChanged(this, [this] { NotifyNetworksChanged(); });
      physical_.SubscribeNetworksChanged(this, [this] { if (policy_->allowed()) NotifyNetworksChanged(); });
      policy_->SetNetworkChange([this](bool allowed) {
        if (allowed) physical_.StartUpdating();
        else physical_.StopUpdating();
        NotifyNetworksChanged();
      });
      loopback_.StartUpdating();
      if (policy_->allowed()) physical_.StartUpdating();
    } else {
      webrtc::Thread::Current()->PostTask(
          webrtc::SafeTask(safety_.flag(), [this] { NotifyNetworksChanged(); }));
    }
  }

  void StopUpdating() override {
    if (users_ == 0 || --users_ != 0) return;
    policy_->SetNetworkChange({});
    if (policy_->allowed()) physical_.StopUpdating();
    physical_.UnsubscribeNetworksChanged(this);
    loopback_.StopUpdating();
    loopback_.UnsubscribeNetworksChanged(this);
  }

  std::vector<const webrtc::Network*> GetNetworks() const override {
    auto networks = loopback_.GetNetworks();
    if (policy_->allowed()) {
      for (const auto* network : physical_.GetNetworks()) {
        if (network->type() != webrtc::ADAPTER_TYPE_LOOPBACK) networks.push_back(network);
      }
    }
    return networks;
  }

  std::vector<const webrtc::Network*> GetAnyAddressNetworks() override { return {}; }

 private:
  std::shared_ptr<ConsentFencedIcePolicy> policy_;
  FencedIceSocketFactory physical_sockets_;
  webrtc::BasicNetworkManager physical_;
  LoopbackIceNetworkManager loopback_;
  webrtc::ScopedTaskSafetyDetached safety_;
  size_t users_ = 0;
};

class HybridIcePacketSocketFactory final : public webrtc::PacketSocketFactory {
 public:
  HybridIcePacketSocketFactory(webrtc::SocketFactory* factory,
                               std::shared_ptr<LoopbackIceListeners> listeners,
                               std::shared_ptr<ConsentFencedIcePolicy> policy)
      : loopback_(factory, std::move(listeners)),
        physical_sockets_(factory, policy), physical_(&physical_sockets_), dns_(std::move(policy)) {}

  std::unique_ptr<webrtc::AsyncPacketSocket> CreateUdpSocket(
      const webrtc::Environment& env, const webrtc::SocketAddress& local,
      uint16_t min_port, uint16_t max_port) override {
    if (!PhysicalIceEndpoint(local)) return nullptr;
    return physical_.CreateUdpSocket(env, local, min_port, max_port);
  }

  std::unique_ptr<webrtc::AsyncListenSocket> CreateServerTcpSocket(
      const webrtc::Environment& env, const webrtc::SocketAddress& local,
      uint16_t min_port, uint16_t max_port, int opts) override {
    if (PhysicalIceEndpoint(local)) {
      return physical_.CreateServerTcpSocket(env, local, min_port, max_port, opts);
    }
    return loopback_.CreateServerTcpSocket(env, local, min_port, max_port, opts);
  }

  std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientTcpSocket(
      const webrtc::Environment& env, const webrtc::SocketAddress& local,
      const webrtc::SocketAddress& remote, const webrtc::PacketSocketTcpOptions& opts) override {
    if (PhysicalIceEndpoint(local)) {
      if (!PhysicalIceEndpoint(remote)) return nullptr;
      return physical_.CreateClientTcpSocket(env, local, remote, opts);
    }
    return loopback_.CreateClientTcpSocket(env, local, remote, opts);
  }

  std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAsyncDnsResolver() override {
    return dns_.Create();
  }

 private:
    LoopbackIcePacketSocketFactory loopback_;
    FencedIceSocketFactory physical_sockets_;
    webrtc::BasicPacketSocketFactory physical_;
  FencedIceDnsFactory dns_;
};

}  // namespace livekit_ffi
