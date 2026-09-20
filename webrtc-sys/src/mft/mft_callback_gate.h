#pragma once

#include <cstddef>
#include <mutex>

namespace webrtc {

// The callback owns this gate, so the mutex outlives the encoder it fences.
// Every callback holds a lease while using owner. Close must finish before
// owner is destroyed. Each encoder generation creates a NEW gate.
template <typename Owner>
class MftCallbackGate {
 public:
  explicit MftCallbackGate(Owner* owner) : owner_(owner) {}
  std::unique_lock<std::recursive_mutex> Lock() {
    return std::unique_lock<std::recursive_mutex>(mutex_);
  }
  std::unique_lock<std::recursive_mutex> TryLock() {
    return std::unique_lock<std::recursive_mutex>(mutex_, std::try_to_lock);
  }
  // Caller holds Lock/TryLock throughout use of the returned pointer.
  Owner* OwnerWhileLocked() const { return owner_; }
  void Close() {
    auto lock = Lock();
    owner_ = nullptr;
  }
 private:
  std::recursive_mutex mutex_;
  Owner* owner_;
};

constexpr bool MftEventCanSubmit(int input_credits, size_t pending) {
  return input_credits > 0 && pending < 2;
}
static_assert(!MftEventCanSubmit(0, 0));
static_assert(MftEventCanSubmit(1, 0));
static_assert(MftEventCanSubmit(1, 1));
static_assert(!MftEventCanSubmit(1, 2));
static_assert(!MftEventCanSubmit(64, 64));

}  // namespace webrtc
