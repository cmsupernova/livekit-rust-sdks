#pragma once

#include <cstdint>

#include "rust/cxx.h"

namespace livekit_ffi {
struct MftD3dSelfTest;
struct MftRateSelfTest;
}  // namespace livekit_ffi

#include "webrtc-sys/src/mft_selftest.rs.h"

namespace livekit_ffi {

// Hardware test of MFT texture input (built with the mft-selftest feature).
MftD3dSelfTest mft_d3d_selftest(uint32_t adapter_ordinal,
                                uint32_t width,
                                uint32_t height,
                                uint32_t cpu_frames,
                                uint32_t texture_frames);

// Hardware test of live rate control: what each frame costs as the target
// moves, and what the MFT says about it.
MftRateSelfTest mft_rate_selftest(uint32_t candidate,
                                  bool unknown_vendor,
                                  uint32_t ceiling_kbps,
                                  uint32_t stretch,
                                  uint32_t width,
                                  uint32_t height);

}  // namespace livekit_ffi
