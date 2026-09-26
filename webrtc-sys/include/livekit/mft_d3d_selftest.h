#pragma once

#include <cstdint>

#include "rust/cxx.h"

namespace livekit_ffi {
struct MftD3dSelfTest;
}  // namespace livekit_ffi

#include "webrtc-sys/src/mft_selftest.rs.h"

namespace livekit_ffi {

// Hardware test of MFT texture input (built with the mft-selftest feature).
MftD3dSelfTest mft_d3d_selftest(uint32_t adapter_ordinal,
                                uint32_t width,
                                uint32_t height,
                                uint32_t cpu_frames,
                                uint32_t texture_frames);

}  // namespace livekit_ffi
