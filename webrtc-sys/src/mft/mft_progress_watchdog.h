#ifndef WEBRTC_MFT_PROGRESS_WATCHDOG_H_
#define WEBRTC_MFT_PROGRESS_WATCHDOG_H_

#include <cstdint>

namespace webrtc {

// Encoder-thread only. A timeout means repeated real Encode attempts with no
// delivered bitstream, not low FPS. Idle capture, paused sending and an isolated
// long scheduler gap must not be mistaken for a broken hardware encoder.
class MftProgressWatchdog {
 public:
  static constexpr uint64_t kTimeoutMs = 3000;
  static constexpr uint64_t kIdleGapMs = 1000;
  static constexpr uint32_t kMinAttempts = 8;

  constexpr void Reset() { *this = MftProgressWatchdog(); }

  constexpr void BeginAttempt(uint64_t now_ms) {
    if (active_ && now_ms - last_attempt_end_ms_ > kIdleGapMs)
      Reset();
    if (!active_) {
      active_ = true;
      no_output_since_ms_ = now_ms;
    }
    if (attempts_ < kMinAttempts)
      ++attempts_;
  }

  constexpr void OutputDelivered(uint64_t now_ms) {
    no_output_since_ms_ = now_ms;
    attempts_ = 0;
  }

  // Call only AFTER pumping/draining available output. A frame that recovered
  // during this attempt gets a chance to clear the stall before fallback.
  constexpr bool EndAttempt(uint64_t now_ms) {
    last_attempt_end_ms_ = now_ms;
    return active_ && attempts_ >= kMinAttempts &&
           now_ms - no_output_since_ms_ >= kTimeoutMs;
  }

 private:
  bool active_ = false;
  uint64_t no_output_since_ms_ = 0;
  uint64_t last_attempt_end_ms_ = 0;
  uint32_t attempts_ = 0;
};

}  // namespace webrtc

#endif  // WEBRTC_MFT_PROGRESS_WATCHDOG_H_
