#include "livekit/d3d11_texture_buffer.h"

#ifdef _WIN32

#include <cstdio>

#include "api/make_ref_counted.h"
#include "api/video/nv12_buffer.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"

namespace livekit_ffi {
namespace {

constexpr char kStorage[] = "d3d11-nv12-keyed-mutex";

// RTC_LOG formats each argument on its own, so std::hex never reaches the
// value after it.
std::string Hex(HRESULT hr) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

// Read-backs serve fallback paths only, so waiting out a producer that is
// mid-render beats returning no picture at all.
constexpr DWORD kReadbackAcquireMs = 100;

// One device for every read-back in the process, on the adapter of the last
// texture read. Read-backs are rare (software fallback frames, rescaling), so
// serializing them on one lock costs nothing that matters.
struct ReadbackDevice {
  uint64_t luid = 0;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  int staging_width = 0;
  int staging_height = 0;
};

std::mutex& ReadbackLock() {
  static std::mutex lock;
  return lock;
}

ReadbackDevice& Readback() {
  static ReadbackDevice device;
  return device;
}

bool EnsureReadbackDevice(ReadbackDevice& rb, uint64_t luid) {
  // A removed device (driver reset, TDR) keeps its LUID but fails every
  // call from then on; start over with a fresh one.
  if (rb.device && FAILED(rb.device->GetDeviceRemovedReason()))
    rb = ReadbackDevice{};
  if (rb.device && rb.luid == luid)
    return true;
  rb = ReadbackDevice{};
  auto adapter = FindAdapterByLuid(luid);
  if (!adapter)
    return false;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, 0, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &rb.device, nullptr,
                                 &rb.context);
  if (hr == E_INVALIDARG) {
    // Runtimes without 11.1 reject the whole list.
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                           levels + 1, 1, D3D11_SDK_VERSION, &rb.device,
                           nullptr, &rb.context);
  }
  if (FAILED(hr)) {
    rb = ReadbackDevice{};
    return false;
  }
  rb.luid = luid;
  return true;
}

}  // namespace

Microsoft::WRL::ComPtr<IDXGIAdapter1> FindAdapterByLuid(uint64_t luid) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (luid == 0 || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return nullptr;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      return nullptr;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        LuidValue(desc.AdapterLuid) == luid)
      return adapter;
  }
}

webrtc::scoped_refptr<D3D11TextureBuffer> D3D11TextureBuffer::Create(
    ID3D11Texture2D* texture,
    HANDLE expected_handle,
    uint64_t texture_id,
    uint64_t adapter_luid,
    int width,
    int height) {
  if (!texture || adapter_luid == 0 || width <= 0 || height <= 0 ||
      (width & 1) || (height & 1))
    return nullptr;
  // Consumers copy subresource 0 whole and map it as one NV12 plane pair.
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_NV12 ||
      desc.Width != static_cast<UINT>(width) ||
      desc.Height != static_cast<UINT>(height) || desc.MipLevels != 1 ||
      desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
      !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
    return nullptr;
  // The handle must be this texture's: the buffer's reference keeps only
  // this texture alive, and a stale legacy handle could name anything.
  Microsoft::WRL::ComPtr<IDXGIResource> resource;
  HANDLE handle = nullptr;
  if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) ||
      FAILED(resource->GetSharedHandle(&handle)) || !handle ||
      (expected_handle && expected_handle != handle))
    return nullptr;
  return webrtc::make_ref_counted<D3D11TextureBuffer>(
      Microsoft::WRL::ComPtr<ID3D11Texture2D>(texture), handle, texture_id,
      adapter_luid, width, height);
}

D3D11TextureBuffer::D3D11TextureBuffer(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    HANDLE shared_handle,
    uint64_t texture_id,
    uint64_t adapter_luid,
    int width,
    int height)
    : texture_(std::move(texture)),
      shared_handle_(shared_handle),
      texture_id_(texture_id),
      adapter_luid_(adapter_luid),
      width_(width),
      height_(height) {}

D3D11TextureBuffer* D3D11TextureBuffer::From(webrtc::VideoFrameBuffer* buffer) {
  // No RTTI in this build. The storage tag is unique to this class, so a
  // matching tag on a native buffer makes the static_cast sound.
  if (!buffer || buffer->type() != Type::kNative ||
      buffer->storage_representation() != kStorage)
    return nullptr;
  return static_cast<D3D11TextureBuffer*>(buffer);
}

webrtc::VideoFrameBuffer::Type D3D11TextureBuffer::type() const {
  return Type::kNative;
}

int D3D11TextureBuffer::width() const {
  return width_;
}

int D3D11TextureBuffer::height() const {
  return height_;
}

std::string D3D11TextureBuffer::storage_representation() const {
  return kStorage;
}

webrtc::scoped_refptr<webrtc::I420BufferInterface>
D3D11TextureBuffer::ToI420() {
  auto buffer = ReadBack();
  return buffer ? buffer->ToI420() : nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
D3D11TextureBuffer::CropAndScale(int offset_x,
                                 int offset_y,
                                 int crop_width,
                                 int crop_height,
                                 int scaled_width,
                                 int scaled_height) {
  auto buffer = ReadBack();
  if (!buffer)
    return nullptr;
  return buffer->CropAndScale(offset_x, offset_y, crop_width, crop_height,
                              scaled_width, scaled_height);
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
D3D11TextureBuffer::GetMappedFrameBuffer(webrtc::ArrayView<Type> types) {
  for (Type type : types) {
    if (type == Type::kNV12)
      return ReadBack();
  }
  for (Type type : types) {
    if (type == Type::kI420)
      return ToI420();
  }
  return nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3D11TextureBuffer::ReadBack() {
  std::lock_guard<std::mutex> own(readback_mutex_);
  if (readback_)
    return readback_;

  std::lock_guard<std::mutex> shared(ReadbackLock());
  auto& rb = Readback();
  if (!EnsureReadbackDevice(rb, adapter_luid_)) {
    RTC_LOG(LS_WARNING) << "D3D11 texture read-back: no device for adapter";
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = rb.device->OpenSharedResource(shared_handle_,
                                             IID_PPV_ARGS(&texture));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "D3D11 texture read-back: open failed " << Hex(hr);
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC source{};
  texture->GetDesc(&source);
  if (source.Format != DXGI_FORMAT_NV12 ||
      source.Width != static_cast<UINT>(width_) ||
      source.Height != static_cast<UINT>(height_))
    return nullptr;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
  if (FAILED(texture.As(&mutex)))
    return nullptr;

  if (!rb.staging || rb.staging_width != width_ ||
      rb.staging_height != height_) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    rb.staging.Reset();
    rb.staging_width = 0;
    rb.staging_height = 0;
    if (FAILED(rb.device->CreateTexture2D(&desc, nullptr, &rb.staging)))
      return nullptr;
    rb.staging_width = width_;
    rb.staging_height = height_;
  }

  if (!AcquireKeyedMutex(mutex.Get(), 0, kReadbackAcquireMs))
    return nullptr;
  rb.context->CopyResource(rb.staging.Get(), texture.Get());
  mutex->ReleaseSync(0);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(rb.context->Map(rb.staging.Get(), 0, D3D11_MAP_READ, 0,
                             &mapped)))
    return nullptr;
  auto nv12 = webrtc::NV12Buffer::Create(width_, height_);
  const auto* luma = static_cast<const uint8_t*>(mapped.pData);
  // A mapped NV12 texture is one allocation: the interleaved chroma plane
  // starts right after `Height` luma rows, at the same pitch.
  const uint8_t* chroma = luma + static_cast<size_t>(mapped.RowPitch) * height_;
  libyuv::CopyPlane(luma, mapped.RowPitch, nv12->MutableDataY(),
                    nv12->StrideY(), width_, height_);
  libyuv::CopyPlane(chroma, mapped.RowPitch, nv12->MutableDataUV(),
                    nv12->StrideUV(), 2 * nv12->ChromaWidth(),
                    nv12->ChromaHeight());
  rb.context->Unmap(rb.staging.Get(), 0);
  readback_ = nv12;
  return readback_;
}

}  // namespace livekit_ffi

#endif  // _WIN32
