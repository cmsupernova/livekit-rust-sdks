#include "mft_callback_gate.h"
#include "../../include/livekit/mft_timing.h"

#include <atomic>
#include <cassert>
#include <future>
#include <thread>

// Standalone unit executable, no COM initialization, GPU, or Rift runtime.
int main() {
  int owner = 7;
  webrtc::MftCallbackGate<int> gate(&owner);
  std::promise<void> entered, finish;
  auto allowed_to_finish = finish.get_future();
  std::thread callback([&] {
    auto lock = gate.Lock();
    assert(gate.OwnerWhileLocked() == &owner);
    entered.set_value();
    allowed_to_finish.wait();
    ++*gate.OwnerWhileLocked();
  });
  entered.get_future().wait();
  // Encode never waits behind output delivery.
  assert(!gate.TryLock().owns_lock());
  std::atomic<bool> closed{false};
  std::promise<void> closing;
  std::thread stop([&] {
    closing.set_value();
    gate.Close();
    closed.store(true);
  });
  closing.get_future().wait();
  assert(!closed.load());
  finish.set_value();
  callback.join();
  stop.join();
  assert(owner == 8 && closed.load());
  {
    auto lock = gate.Lock();
    assert(gate.OwnerWhileLocked() == nullptr); // Late callback is inert.
  }
  int replacement = 9;
  webrtc::MftCallbackGate<int> next(&replacement);
  {
    auto old_lock = gate.Lock();
    auto new_lock = next.Lock();
    assert(gate.OwnerWhileLocked() == nullptr);
    assert(next.OwnerWhileLocked() == &replacement);
  }
  // Every phase has its own count, total, and exact maximum.
  livekit::MftPhase phase;
  phase.Note(500);
  phase.Note(2500);
  assert(phase.us.load() == 3000);
  assert(phase.count.load() == 2);
  assert(phase.max_us.load() == 2500);
  livekit::mft_note_pending(2, 123);
  assert(livekit::mft_timing().oldest_pending_us.load() == 123);
  livekit::mft_note_pending(0);
  assert(livekit::mft_timing().oldest_pending_us.load() == 0);
  livekit::mft_timing().last_output_us.store(0);
  livekit::mft_timing().output_gap_max_us.store(0);
  livekit::mft_note_output(1000);
  assert(livekit::mft_timing().output_gap_max_us.load() == 0);
  livekit::mft_note_output(7000);
  assert(livekit::mft_timing().output_gap_max_us.load() == 6000);
}
