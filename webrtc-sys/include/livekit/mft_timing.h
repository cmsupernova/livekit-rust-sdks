#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace livekit {

inline uint64_t mft_now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void mft_max(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t previous = target.load(std::memory_order_relaxed);
  while (value > previous && !target.compare_exchange_weak(
      previous, value, std::memory_order_relaxed)) {}
}

struct MftPhase {
  std::atomic<uint64_t> us{0}, count{0}, max_us{0};
  void Note(uint64_t elapsed) {
    us.fetch_add(elapsed, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    mft_max(max_us, elapsed);
  }
};

class MftPhaseScope {
 public:
  explicit MftPhaseScope(MftPhase& phase)
      : phase_(phase), start_(mft_now_us()) {}
  ~MftPhaseScope() { phase_.Note(mft_now_us() - start_); }
 private:
  MftPhase& phase_;
  uint64_t start_;
};

// Process-global like the existing NVENC counters. Read by the one native
// screen publisher; do not attribute these to a track if native cameras or
// simultaneous native publishers are introduced.
struct MftTimingCounters {
  MftPhase input_wait, copy, submit, output_wait, output, residence;
  std::atomic<uint64_t> last_output_us{0}, output_gap_max_us{0};
  std::atomic<uint64_t> pending{0}, pending_max{0}, dropped{0};
  std::atomic<uint64_t> oldest_pending_us{0};
  std::atomic<bool> event_driven{false};
  // Frames handed over as a GPU texture copy vs. read from system memory
  // (including CPU frames uploaded while texture input is on).
  std::atomic<uint64_t> texture_frames{0}, memory_frames{0};
};

inline MftTimingCounters& mft_timing() {
  static MftTimingCounters counters;
  return counters;
}

inline void mft_note_pending(uint64_t count, uint64_t oldest_us = 0) {
  mft_timing().pending.store(count, std::memory_order_relaxed);
  mft_timing().oldest_pending_us.store(oldest_us, std::memory_order_relaxed);
  mft_max(mft_timing().pending_max, count);
}

inline void mft_note_output(uint64_t now) {
  auto& counters = mft_timing();
  const auto previous = counters.last_output_us.exchange(now, std::memory_order_relaxed);
  if (previous && now > previous) mft_max(counters.output_gap_max_us, now - previous);
}

}  // namespace livekit
