#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "api/array_view.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"

namespace livekit_ffi {

// An NV12 picture in a D3D11 texture that a capturer shares with a keyed
// mutex, the same handoff OBS uses for its AMF encoder. The producer renders
// it and releases key 0; a consumer on its own device opens the shared
// handle, acquires key 0, copies, and releases key 0. A texture-input encoder
// reads it on the GPU. Anything that needs pixels calls ToI420() or
// GetMappedFrameBuffer(), which read it back once and keep the result, so
// software fallback and rescaling keep working.
class D3D11TextureBuffer : public webrtc::VideoFrameBuffer {
 public:
  // Checks the texture really is what consumers assume (a single-subresource
  // NV12 texture of this size shared with a keyed mutex) and takes the shared
  // handle from the texture itself; a non-null `expected_handle` must match
  // it. Null if anything does not fit.
  static webrtc::scoped_refptr<D3D11TextureBuffer> Create(
      ID3D11Texture2D* texture,
      HANDLE expected_handle,
      uint64_t texture_id,
      uint64_t adapter_luid,
      int width,
      int height);

  // Use Create(); public only for make_ref_counted.
  D3D11TextureBuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                     HANDLE shared_handle,
                     uint64_t texture_id,
                     uint64_t adapter_luid,
                     int width,
                     int height);

  // The buffer as a D3D11TextureBuffer, or nullptr for any other kind.
  static D3D11TextureBuffer* From(webrtc::VideoFrameBuffer* buffer);

  Type type() const override;
  int width() const override;
  int height() const override;
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(
      webrtc::ArrayView<Type> types) override;
  // The default goes through ToI420() and dereferences it unchecked, which
  // crashes when the read-back fails. This reads back once, scales the NV12,
  // and returns null on failure.
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
      int offset_x,
      int offset_y,
      int crop_width,
      int crop_height,
      int scaled_width,
      int scaled_height) override;
  std::string storage_representation() const override;

  HANDLE shared_handle() const { return shared_handle_; }
  // Unique per producer texture for the life of the process, unlike the
  // handle value or the pointer, so a consumer may cache what it opened.
  uint64_t texture_id() const { return texture_id_; }
  uint64_t adapter_luid() const { return adapter_luid_; }

 private:
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> ReadBack();

  // Keeps the shared allocation alive while frames that reference it are in
  // flight, even after the producer has retired its ring.
  const Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  const HANDLE shared_handle_;
  const uint64_t texture_id_;
  const uint64_t adapter_luid_;
  const int width_;
  const int height_;
  std::mutex readback_mutex_;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> readback_;
};

inline uint64_t LuidValue(const LUID& luid) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
         luid.LowPart;
}

// The adapter with this LUID, or null.
Microsoft::WRL::ComPtr<IDXGIAdapter1> FindAdapterByLuid(uint64_t luid);

// Only S_OK means the mutex was acquired. WAIT_TIMEOUT and WAIT_ABANDONED
// are success codes, so SUCCEEDED() would treat a timeout as ownership.
inline bool AcquireKeyedMutex(IDXGIKeyedMutex* mutex, UINT64 key, DWORD ms) {
  return mutex->AcquireSync(key, ms) == S_OK;
}

}  // namespace livekit_ffi

#endif  // _WIN32
