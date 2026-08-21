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

struct NvencTimingCounters {
  std::atomic<uint64_t> copy_us{0};
  std::atomic<uint64_t> submit_us{0};
  std::atomic<uint64_t> wait_us{0};
  std::atomic<uint64_t> frames{0};
};

// Defined inline (C++17) so both the encoder impl and the NvEncoder wrapper
// share one instance without a dedicated translation unit.
inline NvencTimingCounters& nvenc_timing() {
  static NvencTimingCounters counters;
  return counters;
}

}  // namespace livekit
