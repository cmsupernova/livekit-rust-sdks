#pragma once

#include <atomic>
#include <cstdint>

// Aggregated NVENC timing counters.
//
// A screenshare encode on this path is three distinct costs that the single
// `encode_ms_per_frame` WebRTC stat lumps together:
//
//   copy   - host->device transfer of the NV12 frame into the encoder input
//            surface (`NvEncoderCuda::CopyToDeviceFrame`, synchronous).
//   submit - MapResources + nvEncEncodePicture.
//   wait   - blocking until the bitstream for that frame is available.
//
// Field logs showed encode_ms pinned near (window / frames) - the signature of
// an encoder thread that is simply never idle - but not WHICH of the three is
// responsible. Copy dominating points at the CPU round trip and argues for a
// GPU-native path; wait dominating points at serialization and argues for
// pipelining (the wrapper is currently constructed with nExtraOutputDelay = 0,
// so there is exactly one input buffer and no overlap at all).
//
// Deliberately counters rather than per-frame logs: this runs at up to 60Hz on
// the hot path, so it accumulates lock-free and is drained on the stats tick.
// Relaxed ordering is right here - these are diagnostics, never control flow,
// and a torn read costs at most one skewed sample.
namespace livekit {

// Upper edges, in microseconds, of the bitstream-wait histogram. Means hide
// exactly the shape that matters here: a 30ms average with a 2.8s maximum and
// a flat 30ms are the same number and completely different experiences, and
// only one of them is a viewer watching a frozen screen. Roughly doubling
// edges, dense around the frame periods that decide whether a rate is
// sustainable (16ms, 33ms, 66ms).
// Dense between 12ms and 66ms on purpose. That band holds every frame budget
// the pacing ladder can ask for (16.7ms at 60fps through 66.7ms at 15fps), so
// it is where a percentile has to be sharp to answer whether a configuration
// change actually bought back the budget. A coarser ladder here reported a
// 21ms mean wait as a p50 of "33" simply because 16-33 was one bucket, which
// is a true upper bound and a useless one.
inline constexpr uint32_t kNvencWaitBucketCount = 20;
inline constexpr uint64_t kNvencWaitBucketUpperUs[kNvencWaitBucketCount] = {
    1000,   2000,   4000,   8000,    12000,   16000,   20000,
    25000,  33000,  40000,  50000,   66000,   100000,  125000,
    250000, 500000, 1000000, 2000000, 4000000, 8000000};

struct NvencTimingCounters {
  std::atomic<uint64_t> copy_us{0};
  std::atomic<uint64_t> submit_us{0};
  std::atomic<uint64_t> wait_us{0};
  std::atomic<uint64_t> wait_frames{0};
  std::atomic<uint64_t> frames{0};
  // Distribution of the bitstream wait, drained with the sums.
  std::atomic<uint64_t> wait_max_us{0};
  std::atomic<uint64_t> wait_hist[kNvencWaitBucketCount];
  // Longest stretch the encoder produced NO output at all. This is the number
  // that corresponds to what a viewer calls a freeze; it is not recoverable
  // from per-frame timings, because during a stall there are no frames.
  std::atomic<uint64_t> output_gap_max_us{0};
  std::atomic<uint64_t> last_output_us{0};
  // Submit -> bitstream-available, per frame. Distinct from the bitstream
  // wait: with a non-zero output delay the encoder returns a packet for an
  // EARLIER frame, so the wait can fall while the latency each frame actually
  // experiences rises. Pipelining that improves throughput by adding delay is
  // not a win for a live screen share, and only this number can tell them
  // apart.
  std::atomic<uint64_t> latency_us{0};
  std::atomic<uint64_t> latency_max_us{0};
  std::atomic<uint64_t> latency_frames{0};
  // Actual encoded picture type, classified after the H.264 bitstream is
  // returned. This includes periodic GOP IDRs (which are not marked in the
  // input pic params), so it can answer whether the 2 s keyframe cadence is
  // responsible for the observed wait spikes.
  std::atomic<uint64_t> key_wait_us{0};
  std::atomic<uint64_t> key_wait_max_us{0};
  std::atomic<uint64_t> key_wait_frames{0};
  std::atomic<uint64_t> delta_wait_us{0};
  std::atomic<uint64_t> delta_wait_max_us{0};
  std::atomic<uint64_t> delta_wait_frames{0};
};

// Extra NVENC output surfaces (`nExtraOutputDelay`). 0 = fully serialized:
// one input buffer, no overlap between the copy for frame N+1 and the encode
// of frame N. Higher values let those overlap at the cost of holding frames
// longer, which is why it is a knob and not a constant: for a live share the
// tradeoff has to be measured on real hardware, not assumed.
//
// Read once when the encoder is created, so a change takes effect on the next
// share rather than mid-stream.
// Encoder effort profile for screen shares, selected per publish so two arms
// can be compared on the same machine and the same game.
//
//   0  quality      P5 + quarter-resolution two-pass  (legacy baseline)
//   1  single-pass  P5, multipass disabled
//   2  fast         P3, multipass disabled (production default)
//   3  fast-gop10   P3, multipass disabled, 10 s periodic GOP
//
// The field measurement this exists to settle: NVENC bitstream waits of 20-25ms
// against a 16.7ms budget at 2560x1350, while QP sat at 14-28. That much QP
// headroom means the encoder was buying quality nobody needed at a frame rate
// the streamer could feel. Both arms trade analysis effort, which is the only
// encoder cost large enough to matter here - the copy is 0.5ms and the submit
// is 0.3ms.
//
// Read once when the encoder is created, so preset and multipass always agree
// and every frame in a measurement window ran under one configuration.
inline std::atomic<uint32_t>& nvenc_screen_profile() {
  static std::atomic<uint32_t> profile{2};
  return profile;
}

// Staff isolation selector read when a native video encoder is created.
// 0 keeps the normal factory order, while 1/2/3 force the existing NVIDIA,
// MFT, or software H.264 path. It is intentionally process-global: Rift has
// one native screen publisher and camera remains in WebView2 today.
// Media Foundation encoder diagnostics.
//
// An AMD field test (RX 6900 XT) ran seven shares across five staff arms and
// every one silently produced OpenH264 software, including the arm that
// explicitly forced MFT. Nothing in the diagnostics said why: the MFT failure
// HRESULTs only went to RTC_LOG, which the desktop log file does not capture.
// These atomics carry the last MFT attempt's outcome to the app's own logs:
// which stage failed, with what HRESULT, and what the factory registration
// actually looked like.
//
// Stages: 0 not attempted, 1 MFStartup failed, 2 hardware enum found nothing
// (production uses the outer software fallback), 3 no MFT at all (legacy),
// 4 ActivateObject failed,
// 5 async unlock failed, 6 SetOutputType failed, 7 SetInputType failed,
// 8 event generator unavailable, 9 begin/start streaming failed,
// 10 initialized, 11 async feed timeout at runtime, 12 ProcessInput failed,
// 13 rate control unavailable or a live bitrate update failed,
// 14 fatal runtime error, 15 sustained no-output stall requesting fallback.
// Init/runtime failures
// make production fall back to software; isolation arms remain fail-closed.
//
// Flags: 1 NVIDIA factory registered, 2 MFT factory registered, 4 active MFT
// is async, 8 active MFT is hardware, 16 software-MFT enum fallback used (legacy),
// 32 CBR was accepted before SetOutputType, 64 CBR readback confirmed it,
// 128 the initial mean bitrate was accepted before SetOutputType, 256 a live
// mean-bitrate update was accepted, 512 mean-bitrate readback matched target.
struct MftDiagCounters {
  std::atomic<uint32_t> stage{0};
  std::atomic<uint32_t> hr{0};
  std::atomic<uint32_t> flags{0};
};

inline MftDiagCounters& mft_diag() {
  static MftDiagCounters d;
  return d;
}

inline void mft_diag_stage(uint32_t stage, uint32_t hr) {
  mft_diag().stage.store(stage, std::memory_order_relaxed);
  mft_diag().hr.store(hr, std::memory_order_relaxed);
}

inline void mft_diag_flag(uint32_t bit) {
  mft_diag().flags.fetch_or(bit, std::memory_order_relaxed);
}

inline std::atomic<uint32_t>& screen_encoder_mode() {
  static std::atomic<uint32_t> mode{0};
  return mode;
}

inline std::atomic<uint32_t>& nvenc_output_delay() {
  static std::atomic<uint32_t> delay{0};
  return delay;
}

// Defined inline (C++17) so both the encoder impl and the NvEncoder wrapper
// share one instance without a dedicated translation unit.
inline NvencTimingCounters& nvenc_timing() {
  // Static storage duration, so every counter (including the histogram array)
  // is zero-initialized before any dynamic initialization runs.
  static NvencTimingCounters counters;
  return counters;
}

// Records one bitstream wait: sum, running maximum, and histogram bucket.
inline void nvenc_note_wait(uint64_t wait_us) {
  NvencTimingCounters& c = nvenc_timing();
  c.wait_us.fetch_add(wait_us, std::memory_order_relaxed);
  c.wait_frames.fetch_add(1, std::memory_order_relaxed);

  uint64_t prev_max = c.wait_max_us.load(std::memory_order_relaxed);
  while (wait_us > prev_max &&
         !c.wait_max_us.compare_exchange_weak(prev_max, wait_us,
                                              std::memory_order_relaxed)) {
  }

  uint32_t bucket = kNvencWaitBucketCount - 1;
  for (uint32_t i = 0; i < kNvencWaitBucketCount; ++i) {
    if (wait_us < kNvencWaitBucketUpperUs[i]) {
      bucket = i;
      break;
    }
  }
  c.wait_hist[bucket].fetch_add(1, std::memory_order_relaxed);
}

// Starts a new encoder output timeline. Without this reset, the first output
// of a later share measures all time since the previous share's final output
// and reports stopped time as an NVENC freeze in the new A/B arm.
inline void nvenc_note_encoder_start(uint64_t now_us) {
  nvenc_timing().last_output_us.store(now_us, std::memory_order_relaxed);
}

// Records how long one frame spent inside the encoder, submit to output.
inline void nvenc_note_latency(uint64_t latency_us) {
  NvencTimingCounters& c = nvenc_timing();
  c.latency_us.fetch_add(latency_us, std::memory_order_relaxed);
  c.latency_frames.fetch_add(1, std::memory_order_relaxed);
  uint64_t prev_max = c.latency_max_us.load(std::memory_order_relaxed);
  while (latency_us > prev_max &&
         !c.latency_max_us.compare_exchange_weak(prev_max, latency_us,
                                                 std::memory_order_relaxed)) {
  }
}

inline void nvenc_note_frame_type_wait(uint64_t wait_us, bool keyframe) {
  if (wait_us == 0) {
    return;
  }
  NvencTimingCounters& c = nvenc_timing();
  auto& sum = keyframe ? c.key_wait_us : c.delta_wait_us;
  auto& maximum = keyframe ? c.key_wait_max_us : c.delta_wait_max_us;
  auto& frames = keyframe ? c.key_wait_frames : c.delta_wait_frames;
  sum.fetch_add(wait_us, std::memory_order_relaxed);
  frames.fetch_add(1, std::memory_order_relaxed);
  uint64_t previous = maximum.load(std::memory_order_relaxed);
  while (wait_us > previous &&
         !maximum.compare_exchange_weak(previous, wait_us,
                                        std::memory_order_relaxed)) {
  }
}

// Records that encoded output appeared at `now_us` (steady clock), tracking
// the longest gap between consecutive outputs.
inline void nvenc_note_output(uint64_t now_us) {
  NvencTimingCounters& c = nvenc_timing();
  uint64_t prev = c.last_output_us.exchange(now_us, std::memory_order_relaxed);
  if (prev == 0 || now_us <= prev) {
    return;
  }
  uint64_t gap = now_us - prev;
  uint64_t prev_max = c.output_gap_max_us.load(std::memory_order_relaxed);
  while (gap > prev_max &&
         !c.output_gap_max_us.compare_exchange_weak(prev_max, gap,
                                                    std::memory_order_relaxed)) {
  }
}

}  // namespace livekit
