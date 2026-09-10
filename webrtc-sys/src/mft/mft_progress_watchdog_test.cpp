#include "mft_progress_watchdog.h"

// Compile-time regression tests run in every Windows bridge build. They need
// neither a GPU nor a test-only runtime and exercise the production watchdog.
namespace {
using webrtc::MftProgressWatchdog;

constexpr bool FeedTimeoutEventuallyFallsBack() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 3000; now += 250) {
    progress.BeginAttempt(now);
    const bool stalled = progress.EndAttempt(now + 250);
    if (stalled != (now == 2750))
      return false;
  }
  return true;
}

constexpr bool AcceptedInputWithoutOutputEventuallyFallsBack() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 3000; now += 20) {
    progress.BeginAttempt(now);
    if (progress.EndAttempt(now))
      return false;
  }
  progress.BeginAttempt(3000);
  return progress.EndAttempt(3000);
}

constexpr bool RecoveredOutputWinsOverTimeout() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 3000; now += 250) {
    progress.BeginAttempt(now);
    if (now == 2750)
      progress.OutputDelivered(3000);
    if (progress.EndAttempt(now + 250))
      return false;
  }
  progress.BeginAttempt(3250);
  return !progress.EndAttempt(3500);
}

constexpr bool SlowButProductiveEncoderStaysHardware() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 20000; now += 250) {
    progress.BeginAttempt(now);
    // One real output every 2s is poor performance, but not a dead encoder.
    if (now % 2000 == 1750)
      progress.OutputDelivered(now + 250);
    if (progress.EndAttempt(now + 250))
      return false;
  }
  return true;
}

constexpr bool StaticCaptureIsNotAStall() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 120000; now += 2000) {
    progress.BeginAttempt(now);
    if (progress.EndAttempt(now + 250))
      return false;
  }
  return true;
}

constexpr bool SchedulerPauseGetsAnotherChance() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 2500; now += 250) {
    progress.BeginAttempt(now);
    if (progress.EndAttempt(now + 250))
      return false;
  }
  progress.BeginAttempt(30000);
  return !progress.EndAttempt(30250);
}

constexpr bool OneBlockedCallDoesNotForceFallback() {
  MftProgressWatchdog progress;
  progress.BeginAttempt(0);
  return !progress.EndAttempt(10000);
}

constexpr bool PauseOrReinitializeClearsHistory() {
  MftProgressWatchdog progress;
  for (uint64_t now = 0; now < 2500; now += 250) {
    progress.BeginAttempt(now);
    if (progress.EndAttempt(now + 250))
      return false;
  }
  progress.Reset();
  progress.BeginAttempt(2500);
  return !progress.EndAttempt(3500);
}

static_assert(FeedTimeoutEventuallyFallsBack());
static_assert(AcceptedInputWithoutOutputEventuallyFallsBack());
static_assert(RecoveredOutputWinsOverTimeout());
static_assert(SlowButProductiveEncoderStaysHardware());
static_assert(StaticCaptureIsNotAStall());
static_assert(SchedulerPauseGetsAnotherChance());
static_assert(OneBlockedCallDoesNotForceFallback());
static_assert(PauseOrReinitializeClearsHistory());
}  // namespace
