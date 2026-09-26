#include "mft_h264_encoder_impl.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <wrl/implements.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include "livekit/d3d11_texture_buffer.h"
#include "livekit/nvenc_timing.h"
#include "livekit/mft_timing.h"
#include "mft_callback_gate.h"

#include "common_video/h264/h264_common.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/utility/simulcast_rate_allocator.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

namespace webrtc {

// The callback owns its fence, never the encoder. All access to owner_ is
// under gate_; Stop() clears it while holding that same mutex. A late queued
// callback therefore cannot touch a stopped/recreated encoder. EndGetEvent,
// ProcessInput/Output and SetRates are serialized, without polling or an
// additional thread/queue of unencoded VideoFrames.
class MftH264EncoderImpl::EventPump final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFAsyncCallback> {
 public:
  EventPump(MftH264EncoderImpl* owner, IMFMediaEventGenerator* generator)
      : gate_(owner), generator_(generator) {}

  HRESULT Start() {
    auto lock = Lock();
    return generator_->BeginGetEvent(this, nullptr);
  }
  void Stop() {
    auto lock = Lock();
    gate_.Close();
    // Break generator -> pending callback -> generator ownership before the
    // transform is shut down. In-flight Invoke retains its own COM reference.
    generator_.Reset();
  }
  std::unique_lock<std::recursive_mutex> Lock() {
    return gate_.Lock();
  }
  std::unique_lock<std::recursive_mutex> TryLock() {
    return gate_.TryLock();
  }
  STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
    if (!flags || !queue) return E_POINTER;
    *flags = 0;
    *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
    return S_OK;
  }
  STDMETHODIMP Invoke(IMFAsyncResult* result) override {
    auto lock = Lock();
    auto* owner = gate_.OwnerWhileLocked();
    if (!owner || !generator_) return S_OK;
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    HRESULT hr = generator_->EndGetEvent(result, event.GetAddressOf());
    if (FAILED(hr)) {
      owner->event_result_ = owner->RuntimeFailure("EndGetEvent", hr);
      return S_OK;
    }
    if (!owner->HandleMftEvent(event.Get())) {
      owner->event_result_ = WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      return S_OK;
    }
    owner->event_result_ = owner->DrainReadyOutput();
    if (owner->event_result_ != WEBRTC_VIDEO_CODEC_OK) return S_OK;
    hr = generator_->BeginGetEvent(this, nullptr);
    if (FAILED(hr))
      owner->event_result_ = owner->RuntimeFailure("BeginGetEvent", hr);
    return S_OK;
  }
 private:
  MftCallbackGate<MftH264EncoderImpl> gate_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator_;
};

namespace {

// Texture input is on for these adapter vendors only: AMD, whose MFT is the
// production encoder there and whose system-memory path was measured
// starving (ProcessInput 80-220 ms at 1080p). Other MFTs keep the path they
// have always had.
constexpr UINT kAmdVendorId = 0x1002;
// An async MFT pipelines a few inputs; the allocator saying "empty" is
// backpressure and drops the frame rather than growing without bound.
constexpr DWORD kMaxTextureSamples = 8;
// Opened producer textures kept by id: the capturer's ring across one
// capture swap.
constexpr size_t kOpenedTextureCache = 8;
// The producer holds a texture's mutex only while submitting its render.
// Waiting longer than this means it is starved; drop the frame.
constexpr DWORD kTextureAcquireMs = 10;
// Encode-internal: drop this frame and carry on. Never returned to WebRTC.
constexpr int32_t kSkipFrame = 2;

// MFTEnum2 arrived in Windows 10 1703. It is looked up at run time: as a
// load-time import it would stop the whole app from starting on older builds.
using MftEnum2Fn = HRESULT(WINAPI*)(GUID,
                                    UINT32,
                                    const MFT_REGISTER_TYPE_INFO*,
                                    const MFT_REGISTER_TYPE_INFO*,
                                    IMFAttributes*,
                                    IMFActivate***,
                                    UINT32*);

MftEnum2Fn ResolveMftEnum2() {
  static const MftEnum2Fn fn = [] {
    const HMODULE mfplat = GetModuleHandleW(L"mfplat.dll");
    return mfplat ? reinterpret_cast<MftEnum2Fn>(
                        GetProcAddress(mfplat, "MFTEnum2"))
                  : nullptr;
  }();
  return fn;
}

}  // namespace

struct MftH264EncoderImpl::D3DInput {
  ~D3DInput() {
    if (allocator)
      allocator->UninitializeSampleAllocator();
  }

  uint64_t luid = 0;
  // Texture frames are copied in. Cleared after a runtime texture failure,
  // which leaves uploads of CPU frames on the same device.
  bool texture_input = false;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager;
  UINT reset_token = 0;
  Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> upload;
  struct Opened {
    uint64_t id = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
  };
  std::deque<Opened> opened;
};

MftH264EncoderImpl::MftH264EncoderImpl(const Environment& env)
    : env_(env), instance_id_(livekit::d3d_input_next_owner()) {}

MftH264EncoderImpl::~MftH264EncoderImpl() { Release(); }

void MftH264EncoderImpl::I420ToNV12(const I420BufferInterface* i420,
                                     uint8_t* nv12_data,
                                     int nv12_stride) {
  const int w = i420->width();
  const int h = i420->height();

  const uint8_t* src_y = i420->DataY();
  int src_stride_y = i420->StrideY();
  for (int row = 0; row < h; row++) {
    memcpy(nv12_data + row * nv12_stride, src_y + row * src_stride_y, w);
  }

  const uint8_t* u = i420->DataU();
  const uint8_t* v = i420->DataV();
  uint8_t* uv = nv12_data + nv12_stride * h;
  const int uv_width = (w + 1) / 2;
  const int uv_height = (h + 1) / 2;

  libyuv::MergeUVPlane(u, i420->StrideU(), v, i420->StrideV(), uv,
                       nv12_stride, uv_width, uv_height);
}

bool MftH264EncoderImpl::CreateMftEncoder(UINT32 candidate_index,
                                        UINT32* candidate_count,
                                        uint64_t adapter_luid) {
  MFT_REGISTER_TYPE_INFO input_type = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_type = {MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  HRESULT hr = S_OK;
  if (adapter_luid) {
    // Only the MFTs on the capturer's adapter. MFTEnumEx activates usually
    // carry no adapter at all, so this is the one way to know which GPU an
    // encoder sits on.
    Microsoft::WRL::ComPtr<IMFAttributes> filter;
    LUID luid{};
    luid.LowPart = static_cast<DWORD>(adapter_luid & 0xffffffffu);
    luid.HighPart = static_cast<LONG>(adapter_luid >> 32);
    const MftEnum2Fn mft_enum2 = ResolveMftEnum2();
    hr = mft_enum2 ? MFCreateAttributes(filter.GetAddressOf(), 1) : E_NOTIMPL;
    if (SUCCEEDED(hr))
      hr = filter->SetBlob(MFT_ENUM_ADAPTER_LUID,
                           reinterpret_cast<const UINT8*>(&luid), sizeof(luid));
    if (SUCCEEDED(hr))
      hr = mft_enum2(MFT_CATEGORY_VIDEO_ENCODER,
                     MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                     &input_type, &output_type, filter.Get(), &activates,
                     &count);
    *candidate_count = count;
    if (FAILED(hr) || count == 0 || candidate_index >= count) {
      if (candidate_index == 0)
        livekit::mft_d3d_stage(1, static_cast<uint32_t>(hr));
      if (activates) {
        for (UINT32 i = 0; i < count; ++i)
          activates[i]->Release();
        CoTaskMemFree(activates);
      }
      return false;
    }
  } else {
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &input_type, &output_type, &activates, &count);
  }
  *candidate_count = count;
  if (FAILED(hr) || count == 0) {
    // Software is owned by the outer camera-mode fallback factory. Falling
    // back to a software MFT here both bypasses that policy and mislabels the
    // forced-hardware test as a successful hardware encode.
    RTC_LOG(LS_WARNING) << "No hardware H.264 MFT found.";
    livekit::mft_diag_stage(2, static_cast<uint32_t>(hr));
    if (activates) {
      for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
      CoTaskMemFree(activates);
    }
    return false;
  }
  livekit::mft_diag_flag(8);
  if (candidate_index >= count) {
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    return false;
  }

  // Expose the actual vendor MFT in the existing encoder= stats field. A
  // Radeon desktop with an Intel iGPU may select either; "MFT" alone cannot
  // tell an AMD test which encoder Windows selected. Query only at init.
  WCHAR* friendly_name = nullptr;
  UINT32 name_length = 0;
  if (SUCCEEDED(activates[candidate_index]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                &friendly_name, &name_length))) {
    if (friendly_name && name_length > 0 && name_length <= 512) {
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, friendly_name,
          static_cast<int>(name_length), nullptr, 0, nullptr, nullptr);
      if (bytes > 0) {
        std::string name(bytes, '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, friendly_name,
              static_cast<int>(name_length), name.data(), bytes, nullptr, nullptr) == bytes)
          encoder_name_ += " (" + name + ")";
      }
    }
  }
  CoTaskMemFree(friendly_name);

  // Which adapter this hardware MFT runs on, for texture input: the filter
  // when there was one, else whatever the activate says (often nothing).
  adapter_luid_ = adapter_luid;
  LUID activate_luid{};
  UINT32 activate_luid_size = 0;
  if (!adapter_luid_ &&
      SUCCEEDED(activates[candidate_index]->GetBlob(
          MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&activate_luid),
          sizeof(activate_luid), &activate_luid_size)) &&
      activate_luid_size == sizeof(activate_luid))
    adapter_luid_ = livekit_ffi::LuidValue(activate_luid);

  RTC_LOG(LS_INFO) << "Trying hardware MFT candidate " << candidate_index + 1
                   << "/" << count << ": " << encoder_name_;
  hr = activates[candidate_index]->ActivateObject(IID_PPV_ARGS(transform_.GetAddressOf()));
  for (UINT32 i = 0; i < count; i++)
    activates[i]->Release();
  CoTaskMemFree(activates);

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to activate MFT encoder: 0x" << std::hex
                      << hr;
    livekit::mft_diag_stage(4, static_cast<uint32_t>(hr));
    return false;
  }

  transform_->QueryInterface(IID_PPV_ARGS(&codec_api_));

  // Hardware MFTs are asynchronous MFTs, and an async MFT boots LOCKED:
  // SetOutputType / SetInputType / ProcessInput all return
  // MF_E_TRANSFORM_ASYNC_LOCKED until the client sets
  // MF_TRANSFORM_ASYNC_UNLOCK on the transform's attribute store, and after
  // that the transform must be driven by its NeedInput/HaveOutput events.
  // This implementation only spoke the synchronous model, so on every
  // machine whose fallback is a hardware MFT (AMD VCE/VCN, Intel QSV) init
  // failed at the first SetOutputType and the encoder silently became
  // OpenH264 - an RX 6900 XT field run never encoded one hardware frame and
  // no log said why. Unlock here; Encode() speaks the event model when
  // `is_async_` is set.
  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(transform_->GetAttributes(attributes.GetAddressOf())) &&
      attributes) {
    UINT32 is_async = 0;
    if (FAILED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)))
      is_async = 0;
    if (is_async) {
      hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "MFT async unlock failed: 0x" << std::hex << hr;
        livekit::mft_diag_stage(5, static_cast<uint32_t>(hr));
        return false;
      }
      hr = transform_->QueryInterface(IID_PPV_ARGS(event_gen_.GetAddressOf()));
      if (FAILED(hr) || !event_gen_) {
        RTC_LOG(LS_ERROR) << "Async MFT has no event generator: 0x"
                          << std::hex << hr;
        livekit::mft_diag_stage(8, static_cast<uint32_t>(hr));
        return false;
      }
      is_async_ = true;
      livekit::mft_diag_flag(4);
    }
  }
  return true;
}

bool MftH264EncoderImpl::ConfigureOutputType() {
  Microsoft::WRL::ComPtr<IMFMediaType> out_type;
  HRESULT hr = MFCreateMediaType(out_type.GetAddressOf());
  if (FAILED(hr)) return false;

  out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, max_framerate_, 1);
  out_type->SetUINT32(MF_MT_AVG_BITRATE, target_bps_);
  out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

  hr = transform_->SetOutputType(output_stream_id_, out_type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT SetOutputType failed: 0x" << std::hex << hr;
    livekit::mft_diag_stage(6, static_cast<uint32_t>(hr));
    return false;
  }
  return true;
}

bool MftH264EncoderImpl::ConfigureCodecBeforeMediaType() {
  if (!codec_api_) {
    // A live WebRTC encoder must follow congestion-control updates. The media
    // type can seed a bitrate but cannot provide that contract by itself, so
    // let SimulcastEncoderAdapter select Rift's software fallback.
    RTC_LOG(LS_WARNING)
        << "MFT exposes no ICodecAPI; falling back to software H264";
    livekit::mft_diag_stage(13, static_cast<uint32_t>(E_NOINTERFACE));
    return false;
  }

  // Rate-control mode is a STATIC property. Microsoft documents that it only
  // takes effect after SetOutputType, so this must precede
  // ConfigureOutputType(). Setting it later in StartStreaming can return
  // success while leaving a vendor MFT in its default quality/VBR mode.
  VARIANT rc;
  VariantInit(&rc);
  rc.vt = VT_UI4;
  rc.ulVal = eAVEncCommonRateControlMode_CBR;
  HRESULT hr = codec_api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &rc);
  if (SUCCEEDED(hr)) {
    livekit::mft_diag_flag(32);
  } else {
    RTC_LOG(LS_WARNING) << "MFT rejected CBR before SetOutputType: 0x"
                        << std::hex << hr;
    livekit::mft_diag_stage(13, static_cast<uint32_t>(hr));
    return false;
  }

  // Set both the CodecAPI value and MF_MT_AVG_BITRATE (in
  // ConfigureOutputType). Certified Windows hardware encoders are expected to
  // support the former, while the latter keeps older/vendor MFTs usable.
  VARIANT bitrate;
  VariantInit(&bitrate);
  bitrate.vt = VT_UI4;
  bitrate.ulVal = target_bps_;
  hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &bitrate);
  if (SUCCEEDED(hr)) {
    livekit::mft_diag_flag(128);
  } else {
    RTC_LOG(LS_WARNING) << "MFT rejected initial mean bitrate: 0x" << std::hex
                        << hr;
    // MF_MT_AVG_BITRATE is set on the output type as the compatibility path.
    // A later SetRates call still gets a chance to prove that live updates
    // work; only inability to select CBR is fatal at initialization.
  }

  VARIANT low_latency;
  VariantInit(&low_latency);
  low_latency.vt = VT_BOOL;
  low_latency.boolVal = VARIANT_TRUE;
  hr = codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT rejected low-latency mode: 0x" << std::hex
                        << hr;
  }

  VARIANT b_frames;
  VariantInit(&b_frames);
  b_frames.vt = VT_UI4;
  b_frames.ulVal = 0;
  hr = codec_api_->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &b_frames);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT rejected zero B-frames: 0x" << std::hex << hr;
  }

  if (codec_.mode == VideoCodecMode::kScreensharing) {
    VARIANT gop;
    VariantInit(&gop);
    gop.vt = VT_UI4;
    gop.ulVal = std::max<UINT32>(1u, max_framerate_ * 2);
    hr = codec_api_->SetValue(&CODECAPI_AVEncMPVGOPSize, &gop);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MFT rejected screen-share GOP: 0x" << std::hex
                          << hr;
    }
  }
  return true;
}

bool MftH264EncoderImpl::ReadBackRateControl() {
  if (!codec_api_)
    return false;

  VARIANT value;
  VariantInit(&value);
  HRESULT hr = codec_api_->GetValue(&CODECAPI_AVEncCommonRateControlMode,
                                    &value);
  if (SUCCEEDED(hr) && value.vt == VT_UI4) {
    if (value.ulVal == eAVEncCommonRateControlMode_CBR) {
      livekit::mft_diag_flag(64);
    } else {
      RTC_LOG(LS_WARNING) << "MFT rate-control readback is not CBR: "
                          << value.ulVal;
      livekit::mft_diag_stage(13, static_cast<uint32_t>(E_FAIL));
      VariantClear(&value);
      return false;
    }
  } else {
    RTC_LOG(LS_WARNING) << "MFT rate-control readback failed: 0x" << std::hex
                        << hr;
  }
  VariantClear(&value);

  VariantInit(&value);
  hr = codec_api_->GetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
  if (SUCCEEDED(hr) && value.vt == VT_UI4) {
    const uint32_t delta = value.ulVal > target_bps_
                               ? value.ulVal - target_bps_
                               : target_bps_ - value.ulVal;
    const uint32_t tolerance =
        std::max<uint32_t>(100000u, target_bps_ / 20u);
    if (delta <= tolerance)
      livekit::mft_diag_flag(512);
  }
  VariantClear(&value);
  return true;
}

long MftH264EncoderImpl::CreateInputType(IMFMediaType** type) {
  Microsoft::WRL::ComPtr<IMFMediaType> in_type;
  HRESULT hr = MFCreateMediaType(in_type.GetAddressOf());
  if (FAILED(hr)) return hr;

  in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, max_framerate_, 1);
  in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  // Describe the NV12 input as BT.709 limited range so the driver's SPS
  // generation matches the colour space our capture pipeline actually
  // converts in (BGRA -> YUV in BT.709 limited). Without this the MFT emits an
  // SD-defaulted / unsignaled VUI and receivers desaturate / hue-shift the
  // frame. Whether a given MFT propagates these to the output SPS VUI is
  // driver-dependent; NVENC is the primary path and signals the VUI directly.
  in_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
  in_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  in_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  *type = in_type.Detach();
  return S_OK;
}

bool MftH264EncoderImpl::ConfigureInputType() {
  Microsoft::WRL::ComPtr<IMFMediaType> in_type;
  HRESULT hr = CreateInputType(in_type.GetAddressOf());
  if (FAILED(hr)) {
    livekit::mft_diag_stage(7, static_cast<uint32_t>(hr));
    return false;
  }

  hr = transform_->SetInputType(input_stream_id_, in_type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT SetInputType failed: 0x" << std::hex << hr;
    livekit::mft_diag_stage(7, static_cast<uint32_t>(hr));
    return false;
  }
  return true;
}

bool MftH264EncoderImpl::StartStreaming() {
  HRESULT hr =
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT begin streaming failed: 0x" << std::hex << hr;
    livekit::mft_diag_stage(9, static_cast<uint32_t>(hr));
    return false;
  }
  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT start of stream failed: 0x" << std::hex << hr;
    livekit::mft_diag_stage(9, static_cast<uint32_t>(hr));
    return false;
  }
  return true;
}

int32_t MftH264EncoderImpl::InitEncode(const VideoCodec* inst,
                                        const VideoEncoder::Settings&) {
  // Texture input first, from the MFTs on the capturer's adapter. It counts
  // only if texture input really comes on; anything short of that (no offer,
  // not AMD, not D3D11-aware, a failed init with the manager) falls through
  // to the classic enumeration below, which runs exactly as it always has.
  const uint64_t texture_adapter =
      livekit::d3d_input_requested().load(std::memory_order_relaxed);
  if (inst && inst->mode == VideoCodecMode::kScreensharing &&
      texture_adapter != 0 && TextureInputAllowed(texture_adapter)) {
    // This attempt's outcome, not a previous encoder's.
    livekit::mft_d3d_stage(0, 0);
    UINT32 count = 1;
    for (UINT32 index = 0; index < count && index < 4; ++index) {
      const int32_t result =
          InitEncodeCandidate(inst, index, &count, texture_adapter);
      if (result == WEBRTC_VIDEO_CODEC_OK ||
          result == WEBRTC_VIDEO_CODEC_ERR_PARAMETER)
        return result;
      const bool manager_accepted = d3d_attempted_;
      const uint32_t failed_hr =
          livekit::mft_diag().hr.load(std::memory_order_relaxed);
      Release();
      // Keep a specific decline (stages 1-8); anything else, including the
      // MFT failing before texture setup even began, is stage 9.
      if (manager_accepted ||
          livekit::mft_diag().d3d_stage.load(std::memory_order_relaxed) == 0) {
        RTC_LOG(LS_WARNING) << "MFT failed to initialize for texture input";
        livekit::mft_d3d_stage(9, failed_hr);
      }
    }
  }
  return InitEncodeClassic(inst);
}

int32_t MftH264EncoderImpl::InitEncodeClassic(const VideoCodec* inst) {
  // Keep the OS-preferred hardware first. A second adapter is useful only
  // if it accepts this actual codec/resolution/rate-control configuration.
  // Startup only: no GPU hopping, no extra simultaneous encoders, and no
  // change at all for the already-working first candidate.
  UINT32 count = 1;
  int32_t result = WEBRTC_VIDEO_CODEC_ERROR;
  for (UINT32 index = 0; index < count && index < 4; ++index) {
    result = InitEncodeCandidate(inst, index, &count, /*texture_adapter=*/0);
    if (result == WEBRTC_VIDEO_CODEC_OK || result == WEBRTC_VIDEO_CODEC_ERR_PARAMETER)
      return result;
    RTC_LOG(LS_WARNING) << "Hardware MFT candidate " << index + 1
                        << " failed initialization; checking next candidate";
    Release();
  }
  return result;
}

bool MftH264EncoderImpl::TextureInputAllowed(uint64_t luid) {
  if (livekit::d3d_input_failed().load(std::memory_order_relaxed) == luid) {
    // Failed at runtime earlier this session; keep reporting that, with the
    // HRESULT it failed with.
    livekit::mft_d3d_stage(
        12, livekit::d3d_input_failed_hr().load(std::memory_order_relaxed));
    return false;
  }
  // Checked before any MFT is activated, so an offer on another vendor costs
  // nothing but this lookup.
  auto adapter = livekit_ffi::FindAdapterByLuid(luid);
  DXGI_ADAPTER_DESC1 desc{};
  if (!adapter || FAILED(adapter->GetDesc1(&desc))) {
    livekit::mft_d3d_stage(3, static_cast<uint32_t>(E_FAIL));
    return false;
  }
  if (desc.VendorId != kAmdVendorId &&
      !livekit::d3d_input_any_vendor().load(std::memory_order_relaxed)) {
    livekit::mft_d3d_stage(3, desc.VendorId);
    return false;
  }
  return true;
}

int32_t MftH264EncoderImpl::InitEncodeCandidate(const VideoCodec* inst,
                                               UINT32 candidate_index,
                                               UINT32* candidate_count,
                                               uint64_t texture_adapter) {
  if (!inst || inst->codecType != kVideoCodecH264)
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  if (inst->maxFramerate == 0 || inst->width < 1 || inst->height < 1)
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

  Release();
  codec_ = *inst;

  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  width_ = codec_.width;
  height_ = codec_.height;
  max_framerate_ = codec_.maxFramerate;
  target_bps_ = codec_.startBitrate * 1000;

  const size_t capacity = CalcBufferSize(VideoType::kI420, width_, height_);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(capacity));
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.set_size(0);

  livekit::mft_diag_stage(0, 0);
  // Rate-control/async bits belong to this candidate, not a failed predecessor.
  livekit::mft_diag().flags.fetch_and(3u, std::memory_order_relaxed);
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFStartup failed: 0x" << std::hex << hr;
    livekit::mft_diag_stage(1, static_cast<uint32_t>(hr));
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  mf_started_ = true;

  if (!CreateMftEncoder(candidate_index, candidate_count, texture_adapter))
    return WEBRTC_VIDEO_CODEC_ERROR;

  const bool event_driven =
      livekit::screen_encoder_mode().load(std::memory_order_relaxed) == 5;
  if (event_driven &&
      (!is_async_ || codec_.mode != VideoCodecMode::kScreensharing)) {
    // An isolation run must never silently compare polling against polling.
    return RuntimeFailure("event-driven screen test requires an async MFT", E_NOTIMPL);
  }

  // The D3D manager must reach the MFT before any media type. A texture
  // attempt that declines (not AMD, not D3D11-aware, any setup failure) ends
  // here and hands over to the classic path.
  if (texture_adapter && !EnableD3DInput())
    return WEBRTC_VIDEO_CODEC_ERROR;

  if (!ConfigureCodecBeforeMediaType())
    return WEBRTC_VIDEO_CODEC_ERROR;

  DWORD in_count = 0, out_count = 0;
  hr = transform_->GetStreamCount(&in_count, &out_count);
  if (SUCCEEDED(hr) && in_count > 0 && out_count > 0) {
    hr = transform_->GetStreamIDs(1, &input_stream_id_, 1, &output_stream_id_);
    if (hr == E_NOTIMPL) {
      input_stream_id_ = 0;
      output_stream_id_ = 0;
    }
  }

  if (!ConfigureOutputType())
    return WEBRTC_VIDEO_CODEC_ERROR;
  if (!ReadBackRateControl())
    return WEBRTC_VIDEO_CODEC_ERROR;
  if (!ConfigureInputType() || !StartStreaming())
    return WEBRTC_VIDEO_CODEC_ERROR;

  livekit::mft_diag_stage(10, 0);
  RTC_LOG(LS_INFO) << "MFT H264 encoder initialized: " << width_ << "x"
                   << height_ << " @ " << max_framerate_ << "fps ("
                   << (is_async_ ? "async event model" : "sync model")
                   << "), target_bps=" << target_bps_;

  SimulcastRateAllocator init_allocator(env_, codec_);
  VideoBitrateAllocation allocation =
      init_allocator.Allocate(VideoBitrateAllocationParameters(
          DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));
  livekit::mft_timing().event_driven.store(event_driven, std::memory_order_relaxed);
  livekit::mft_timing().last_output_us.store(0, std::memory_order_relaxed);
  if (d3d_ && d3d_->texture_input) {
    // Only now may the capturer send textures: the MFT holds our manager,
    // negotiated its types with it, and is streaming. Published before the
    // staff event pump starts, so a failure it reports can only withdraw it.
    texture_input_on_.store(true, std::memory_order_relaxed);
    livekit::d3d_input_publish(instance_id_, d3d_->luid);
    livekit::mft_d3d_stage(10, 0);
    livekit::mft_diag_flag(1024);
    RTC_LOG(LS_INFO) << "MFT texture input on for this encoder";
  }
  if (event_driven) {
    event_pump_ = Microsoft::WRL::Make<EventPump>(this, event_gen_.Get());
    if (!event_pump_) return WEBRTC_VIDEO_CODEC_MEMORY;
    hr = event_pump_->Start();
    if (FAILED(hr)) {
      RuntimeFailure("start event pump", hr);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  auto lock = event_pump_ ? event_pump_->Lock()
                          : std::unique_lock<std::recursive_mutex>();
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::Release() {
  if (event_pump_) {
    event_pump_->Stop();
    event_pump_.Reset();
  }
  event_result_ = WEBRTC_VIDEO_CODEC_OK;
  event_key_frame_request_.store(false, std::memory_order_relaxed);
  livekit::mft_note_pending(0);
  livekit::mft_timing().last_output_us.store(0, std::memory_order_relaxed);
  livekit::mft_timing().event_driven.store(false, std::memory_order_relaxed);
  if (transform_) {
    // Async transforms own an event queue and worker resources. Releasing
    // COM references without Shutdown can leave them alive after fallback.
    Microsoft::WRL::ComPtr<IMFShutdown> shutdown;
    if (SUCCEEDED(transform_.As(&shutdown))) {
      shutdown->Shutdown();
    } else {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    transform_.Reset();
  }
  if (codec_api_) {
    codec_api_->Release();
    codec_api_ = nullptr;
  }
  event_gen_.Reset();
  // After the transform has let go of our manager and samples, and before
  // MFShutdown.
  WithdrawTextureInput();
  d3d_.reset();
  adapter_luid_ = 0;
  d3d_attempted_ = false;
  retry_in_memory_.store(false, std::memory_order_relaxed);
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
  is_async_ = false;
  input_credits_ = 0;
  output_credits_ = 0;
  pending_meta_.clear();
  sequence_header_.clear();
  key_frame_request_ = false;
  last_key_frame_us_ = 0;
  sending_ = false;
  bitrate_failure_logged_ = false;
  runtime_failed_ = false;
  progress_.Reset();
  encoder_name_ = "Windows MFT H264 Encoder";
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::RuntimeFailure(const char* operation, HRESULT hr) {
  if (!runtime_failed_) {
    RTC_LOG(LS_ERROR) << "MFT " << operation << " failed: 0x" << std::hex
                      << hr << std::dec << "; requesting software fallback"
                      << " input_credits=" << input_credits_
                      << " output_credits=" << output_credits_
                      << " pending_frames=" << pending_meta_.size();
    livekit::mft_diag_stage(14, static_cast<uint32_t>(hr));
  }
  runtime_failed_ = true;
  // In texture mode the failure may be texture input itself (an MFT that
  // takes the D3D manager and then rejects or starves on texture samples).
  // Encode brings the classic encoder up instead of handing the share to
  // software; see RetryInMemory. Staff isolation arms keep failing closed.
  if (d3d_ && livekit::screen_encoder_mode().load(std::memory_order_relaxed) == 0)
    retry_in_memory_.store(true, std::memory_order_relaxed);
  // Stop the capturer sending textures now rather than after the adapter
  // swaps encoders.
  WithdrawTextureInput();
  // A generic ENCODER_FAILURE does not request WebRTC's runtime fallback.
  // Staff isolation still fails closed because it has no fallback factory.
  return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
}

bool MftH264EncoderImpl::EnableD3DInput() {
  const uint64_t requested =
      livekit::d3d_input_requested().load(std::memory_order_relaxed);
  if (codec_.mode != VideoCodecMode::kScreensharing || requested == 0) {
    livekit::mft_d3d_stage(0, 0);
    return false;
  }
  if (adapter_luid_ == 0 || adapter_luid_ != requested) {
    livekit::mft_d3d_stage(1, 0);
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  UINT32 d3d11_aware = 0;
  if (FAILED(transform_->GetAttributes(attributes.GetAddressOf())) ||
      !attributes ||
      FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) ||
      !d3d11_aware) {
    livekit::mft_d3d_stage(2, 0);
    return false;
  }

  auto adapter = livekit_ffi::FindAdapterByLuid(adapter_luid_);
  DXGI_ADAPTER_DESC1 adapter_desc{};
  if (!adapter || FAILED(adapter->GetDesc1(&adapter_desc))) {
    livekit::mft_d3d_stage(3, static_cast<uint32_t>(E_FAIL));
    return false;
  }
  if (adapter_desc.VendorId != kAmdVendorId &&
      !livekit::d3d_input_any_vendor().load(std::memory_order_relaxed)) {
    livekit::mft_d3d_stage(3, adapter_desc.VendorId);
    return false;
  }

  auto d3d = std::make_unique<D3DInput>();
  d3d->luid = adapter_luid_;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  const UINT flags =
      D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, flags, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &d3d->device, nullptr,
                                 &d3d->context);
  if (hr == E_INVALIDARG) {
    // Runtimes without 11.1 reject the whole list.
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           flags, levels + 1, 1, D3D11_SDK_VERSION,
                           &d3d->device, nullptr, &d3d->context);
  }
  if (FAILED(hr)) {
    livekit::mft_d3d_stage(4, static_cast<uint32_t>(hr));
    return false;
  }
  // The MFT's worker threads use this device alongside the encoder thread.
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
  hr = d3d->device.As(&multithread);
  if (FAILED(hr)) {
    livekit::mft_d3d_stage(4, static_cast<uint32_t>(hr));
    return false;
  }
  multithread->SetMultithreadProtected(TRUE);

  // Opening another device's NV12 texture needs extended resource sharing.
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  UINT nv12_support = 0;
  if (FAILED(d3d->device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
                                              &options, sizeof(options))) ||
      !options.ExtendedResourceSharing ||
      FAILED(d3d->device->CheckFormatSupport(DXGI_FORMAT_NV12,
                                             &nv12_support)) ||
      !(nv12_support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
    livekit::mft_d3d_stage(5, 0);
    return false;
  }

  hr = MFCreateDXGIDeviceManager(&d3d->reset_token,
                                 d3d->manager.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = d3d->manager->ResetDevice(d3d->device.Get(), d3d->reset_token);
  if (FAILED(hr)) {
    livekit::mft_d3d_stage(6, static_cast<uint32_t>(hr));
    return false;
  }

  // Input samples come from a D3D11 allocator: the MFT hands each back when
  // done with it, which recycles the texture. Bind flags as OBS gives its
  // AMF pool textures.
  Microsoft::WRL::ComPtr<IMFAttributes> allocator_attributes;
  Microsoft::WRL::ComPtr<IMFMediaType> input_type;
  hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&d3d->allocator));
  if (SUCCEEDED(hr))
    hr = d3d->allocator->SetDirectXManager(d3d->manager.Get());
  if (SUCCEEDED(hr))
    hr = MFCreateAttributes(allocator_attributes.GetAddressOf(), 2);
  if (SUCCEEDED(hr)) {
    UINT32 bind = D3D11_BIND_SHADER_RESOURCE;
    if (nv12_support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
      bind |= D3D11_BIND_RENDER_TARGET;
    allocator_attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, bind);
    allocator_attributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
    hr = CreateInputType(input_type.GetAddressOf());
  }
  if (SUCCEEDED(hr))
    hr = d3d->allocator->InitializeSampleAllocatorEx(
        2, kMaxTextureSamples, allocator_attributes.Get(), input_type.Get());
  if (FAILED(hr)) {
    livekit::mft_d3d_stage(7, static_cast<uint32_t>(hr));
    return false;
  }

  hr = transform_->ProcessMessage(
      MFT_MESSAGE_SET_D3D_MANAGER,
      reinterpret_cast<ULONG_PTR>(d3d->manager.Get()));
  if (FAILED(hr)) {
    livekit::mft_d3d_stage(8, static_cast<uint32_t>(hr));
    return false;
  }
  // From here the MFT is in D3D mode; a failed init retries in memory.
  d3d_attempted_ = true;
  d3d->texture_input = true;
  d3d_ = std::move(d3d);
  return true;
}

void MftH264EncoderImpl::WithdrawTextureInput() {
  texture_input_on_.store(false, std::memory_order_relaxed);
  // Only this encoder's claim; a newer encoder's stays.
  livekit::d3d_input_withdraw(instance_id_);
  if (!d3d_)
    return;
  d3d_->texture_input = false;
  d3d_->opened.clear();
}

int32_t MftH264EncoderImpl::TextureInputFailed(const char* operation,
                                               long hr) {
  RTC_LOG(LS_WARNING) << "MFT texture input " << operation << " failed: 0x"
                      << std::hex << hr << std::dec
                      << "; taking frames from memory from here on";
  livekit::mft_d3d_stage(11, static_cast<uint32_t>(hr));
  WithdrawTextureInput();
  return kSkipFrame;
}

int32_t MftH264EncoderImpl::AllocateTextureSample(
    Microsoft::WRL::ComPtr<IMFSample>* sample,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>* texture,
    UINT* subresource) {
  HRESULT hr = d3d_->allocator->AllocateSample(sample->ReleaseAndGetAddressOf());
  if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
    return kSkipFrame;
  if (FAILED(hr))
    return RuntimeFailure("allocate texture sample", hr);
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgi_buffer;
  hr = (*sample)->GetBufferByIndex(0, buffer.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = buffer.As(&dxgi_buffer);
  if (SUCCEEDED(hr))
    hr = dxgi_buffer->GetResource(IID_PPV_ARGS(texture->ReleaseAndGetAddressOf()));
  if (SUCCEEDED(hr))
    hr = dxgi_buffer->GetSubresourceIndex(subresource);
  if (FAILED(hr))
    return RuntimeFailure("texture sample buffer", hr);
  // Some MFTs read the current length even of a GPU buffer.
  Microsoft::WRL::ComPtr<IMF2DBuffer> buffer_2d;
  DWORD length = 0;
  if (SUCCEEDED(buffer.As(&buffer_2d)) &&
      SUCCEEDED(buffer_2d->GetContiguousLength(&length)))
    buffer->SetCurrentLength(length);
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::TextureSample(
    const livekit_ffi::D3D11TextureBuffer& frame,
    Microsoft::WRL::ComPtr<IMFSample>* sample) {
  auto& d3d = *d3d_;
  auto opened = std::find_if(d3d.opened.begin(), d3d.opened.end(),
                             [&](const D3DInput::Opened& entry) {
                               return entry.id == frame.texture_id();
                             });
  if (opened == d3d.opened.end()) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = d3d.device->OpenSharedResource(frame.shared_handle(),
                                                IID_PPV_ARGS(&texture));
    if (FAILED(hr))
      return TextureInputFailed("open shared texture", hr);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != width_ ||
        desc.Height != height_)
      return TextureInputFailed("shared texture shape", E_INVALIDARG);
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    hr = texture.As(&mutex);
    if (FAILED(hr))
      return TextureInputFailed("shared texture mutex", hr);
    if (d3d.opened.size() >= kOpenedTextureCache)
      d3d.opened.pop_front();
    d3d.opened.push_back({frame.texture_id(), std::move(texture),
                          std::move(mutex)});
    opened = std::prev(d3d.opened.end());
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
  UINT subresource = 0;
  const int32_t allocated = AllocateTextureSample(sample, &target, &subresource);
  if (allocated != WEBRTC_VIDEO_CODEC_OK)
    return allocated;

  const HRESULT acquired = opened->mutex->AcquireSync(0, kTextureAcquireMs);
  if (acquired == static_cast<HRESULT>(WAIT_TIMEOUT))
    return kSkipFrame;
  if (acquired != S_OK) {
    // Abandoned (the producer's device went away) or a device error.
    d3d.opened.erase(opened);
    return TextureInputFailed("acquire shared texture", acquired);
  }
  d3d.context->CopySubresourceRegion(target.Get(), subresource, 0, 0, 0,
                                     opened->texture.Get(), 0, nullptr);
  opened->mutex->ReleaseSync(0);
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::UploadSample(
    const NV12BufferInterface* nv12,
    const I420BufferInterface* i420,
    Microsoft::WRL::ComPtr<IMFSample>* sample) {
  auto& d3d = *d3d_;
  if (!d3d.upload) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr = d3d.device->CreateTexture2D(&desc, nullptr, &d3d.upload);
    if (FAILED(hr))
      return RuntimeFailure("create upload texture", hr);
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
  UINT subresource = 0;
  const int32_t allocated = AllocateTextureSample(sample, &target, &subresource);
  if (allocated != WEBRTC_VIDEO_CODEC_OK)
    return allocated;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT hr =
      d3d.context->Map(d3d.upload.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
  if (FAILED(hr))
    return RuntimeFailure("map upload texture", hr);
  // One allocation: the interleaved chroma plane follows `Height` luma rows
  // at the same pitch, which is the layout I420ToNV12 writes.
  auto* luma = static_cast<uint8_t*>(mapped.pData);
  if (nv12) {
    libyuv::CopyPlane(nv12->DataY(), nv12->StrideY(), luma, mapped.RowPitch,
                      width_, height_);
    libyuv::CopyPlane(nv12->DataUV(), nv12->StrideUV(),
                      luma + static_cast<size_t>(mapped.RowPitch) * height_,
                      mapped.RowPitch, width_, height_ / 2);
  } else {
    I420ToNV12(i420, luma, static_cast<int>(mapped.RowPitch));
  }
  d3d.context->Unmap(d3d.upload.Get(), 0);
  d3d.context->CopySubresourceRegion(target.Get(), subresource, 0, 0, 0,
                                     d3d.upload.Get(), 0, nullptr);
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::FinishEncodeAttempt() {
  if (progress_.EndAttempt(GetTickCount64())) {
    const HRESULT hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const int32_t result = RuntimeFailure("sustained no-output stall", hr);
    livekit::mft_diag_stage(15, static_cast<uint32_t>(hr));
    return result;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

bool MftH264EncoderImpl::PumpMftEvents() {
  // Do not let a broken driver monopolize the encoder thread with events.
  // Any remainder is consumed on the next pump; credits survive this bound.
  for (int events = 0; events < 64; ++events) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    HRESULT hr =
        event_gen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.GetAddressOf());
    if (hr == MF_E_NO_EVENTS_AVAILABLE)
      return true;
    if (FAILED(hr) || !event) {
      RuntimeFailure("GetEvent", FAILED(hr) ? hr : E_UNEXPECTED);
      return false;
    }
    if (!HandleMftEvent(event.Get())) return false;
  }
  return true;
}

bool MftH264EncoderImpl::HandleMftEvent(IMFMediaEvent* event) {
  if (!event) {
    RuntimeFailure("null event", E_UNEXPECTED);
    return false;
  }
  MediaEventType type = MEUnknown;
  HRESULT hr = event->GetType(&type);
  if (FAILED(hr)) {
    RuntimeFailure("GetType", hr);
    return false;
  }
  HRESULT status = S_OK;
  hr = event->GetStatus(&status);
  if (FAILED(hr) || FAILED(status) || type == MEError) {
    RuntimeFailure("event", FAILED(hr) ? hr : FAILED(status) ? status : E_FAIL);
    return false;
  }
  if (type == METransformNeedInput) ++input_credits_;
  if (type == METransformHaveOutput) ++output_credits_;
  if (input_credits_ > 64 || output_credits_ > 64) {
    RuntimeFailure("event credit overflow", E_UNEXPECTED);
    return false;
  }
  return true;
}

int32_t MftH264EncoderImpl::DrainReadyOutput() {
  // Only consume credits already collected by the event pump. Output must
  // not depend on accepting another input: some drivers stop asking for
  // input until their completed output has been consumed.
  while (output_credits_ > 0) {
    --output_credits_;
    const int32_t result = ProcessEncodedOutput(nullptr, true);
    if (result != WEBRTC_VIDEO_CODEC_OK)
      return result;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  const int32_t result = EncodeFrame(input_frame, frame_types);
  if (result != WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE ||
      !retry_in_memory_.load(std::memory_order_relaxed))
    return result;
  return RetryInMemory();
}

int32_t MftH264EncoderImpl::RetryInMemory() {
  // Texture input failed at runtime. Before texture input existed this
  // machine got the classic system-memory MFT, so give it exactly that back
  // rather than software for the rest of the share, and do not try texture
  // input on this adapter again until the app restarts. The failing frame is
  // lost; the new transform starts with an IDR.
  const uint64_t luid = d3d_ ? d3d_->luid : adapter_luid_;
  const uint32_t failed_hr =
      livekit::mft_diag().hr.load(std::memory_order_relaxed);
  livekit::d3d_input_failed_hr().store(failed_hr, std::memory_order_relaxed);
  livekit::d3d_input_failed().store(luid, std::memory_order_relaxed);
  const VideoCodec codec = codec_;
  const uint32_t target_bps = target_bps_;
  const uint32_t framerate = max_framerate_;
  const bool sending = sending_;
  EncodedImageCallback* const callback = callback_;
  Release();
  if (InitEncodeClassic(&codec) != WEBRTC_VIDEO_CODEC_OK) {
    Release();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  callback_ = callback;
  if (sending && target_bps > 0) {
    VideoBitrateAllocation allocation;
    allocation.SetBitrate(0, 0, target_bps);
    SetRates(RateControlParameters(allocation, static_cast<double>(framerate)));
  }
  livekit::mft_d3d_stage(12, failed_hr);
  RTC_LOG(LS_WARNING) << "MFT texture input failed at runtime; the classic "
                         "encoder took over";
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::EncodeFrame(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  const bool force_requested = frame_types && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kVideoFrameKey;
  if (event_pump_ && force_requested)
    event_key_frame_request_.store(true, std::memory_order_relaxed);
  auto lock = event_pump_ ? event_pump_->TryLock()
                          : std::unique_lock<std::recursive_mutex>();
  if (event_pump_ && !lock.owns_lock()) {
    // Never queue stale source frames behind a slow ProcessOutput/callback.
    // Keyframe intent survives this cheap pre-copy drop.
    livekit::mft_timing().dropped.fetch_add(1, std::memory_order_relaxed);
    return WEBRTC_VIDEO_CODEC_OK;
  }
  if (!transform_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!callback_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (runtime_failed_)
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  if (event_pump_ && event_result_ != WEBRTC_VIDEO_CODEC_OK)
    return event_result_;

  // Preserve keyframe intent even if this input is dropped for backpressure.
  const bool remembered_key = event_key_frame_request_.exchange(false, std::memory_order_relaxed);
  if (force_requested || remembered_key)
    key_frame_request_ = true;
  if (!sending_) {
    progress_.Reset();
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }
  if (frame_types && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kEmptyFrame)
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;

  progress_.BeginAttempt(GetTickCount64());
  if (event_pump_ && !MftEventCanSubmit(input_credits_, pending_meta_.size())) {
    livekit::mft_timing().dropped.fetch_add(1, std::memory_order_relaxed);
    return FinishEncodeAttempt();
  }
  if (is_async_ && !event_pump_) {
    livekit::MftPhaseScope timing(livekit::mft_timing().input_wait);
    const ULONGLONG feed_deadline = GetTickCount64() + 250;
    for (;;) {
      if (!PumpMftEvents())
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      const int32_t result = DrainReadyOutput();
      if (result != WEBRTC_VIDEO_CODEC_OK)
        return result;
      if (input_credits_ > 0)
        break;
      if (GetTickCount64() > feed_deadline) {
        livekit::mft_timing().dropped.fetch_add(1, std::memory_order_relaxed);
        const int32_t result = FinishEncodeAttempt();
        if (result != WEBRTC_VIDEO_CODEC_OK)
          return result;
        // Keep a transient backpressure breadcrumb without per-frame log
        // spam. Persistent failure is latched and requests fallback above.
        livekit::mft_diag_stage(11, 0);
        return WEBRTC_VIDEO_CODEC_OK;
      }
      Sleep(1);
    }
  }

  // Only allocate/copy pixels after the transform can accept them.
  const uint64_t copy_start = livekit::mft_now_us();
  auto* frame_buffer = input_frame.video_frame_buffer().get();
  // A texture on our adapter is copied on the GPU. Anything else, including
  // a texture still in flight after texture input was withdrawn, goes in as
  // pixels: a texture buffer reads itself back.
  const livekit_ffi::D3D11TextureBuffer* texture =
      d3d_ && d3d_->texture_input
          ? livekit_ffi::D3D11TextureBuffer::From(frame_buffer)
          : nullptr;
  if (texture && texture->adapter_luid() != d3d_->luid)
    texture = nullptr;
  const NV12BufferInterface* nv12_buffer = nullptr;
  webrtc::scoped_refptr<I420BufferInterface> i420_buffer;
  if (!texture) {
    if (frame_buffer->type() == VideoFrameBuffer::Type::kNV12)
      nv12_buffer = frame_buffer->GetNV12();
    if (!nv12_buffer) {
      i420_buffer = frame_buffer->ToI420();
      if (!i420_buffer) {
        // A texture that could not be read back is one lost frame, not a
        // broken encoder.
        if (frame_buffer->type() == VideoFrameBuffer::Type::kNative) {
          livekit::mft_timing().dropped.fetch_add(1, std::memory_order_relaxed);
          return FinishEncodeAttempt();
        }
        return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
      }
    }
  }

  // The screen-share GOP is sized once at init (2 s at the init frame rate)
  // and not changed mid-stream, which some MFTs reject. Once SetRates lowers
  // the frame rate that GOP spans 4 s at 30 fps and ~18 s at the idle
  // keep-alive rate, past libwebrtc's ~3 s no-decodable-frame timeout the
  // 2 s IDR exists to bound. Force an IDR on elapsed time instead; 2.2 s lets
  // the GOP's own IDR land first at full frame rate.
  if (codec_.mode == VideoCodecMode::kScreensharing) {
    constexpr uint64_t kMaxKeyFrameGapUs = 2200000;
    const uint64_t now_us = livekit::mft_now_us();
    if (last_key_frame_us_ == 0) {
      last_key_frame_us_ = now_us;
    } else if (now_us > last_key_frame_us_ + kMaxKeyFrameGapUs) {
      key_frame_request_ = true;
      // Restart the clock at the request, not only at the IDR's output, so a
      // pipelined MFT is not asked again on every frame in between.
      last_key_frame_us_ = now_us;
    }
  }

  const bool force_key = key_frame_request_;

  if (force_key && codec_api_) {
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_UI4;
    // CODECAPI_AVEncVideoForceKeyFrame takes a VT_UI4 interpreted as a
    // boolean: 1 = force the next frame to be a keyframe, 0 = do nothing.
    // Previously this was 0, making every PLI/FIR-triggered keyframe
    // request a no-op and leaving receivers stuck on missed IDRs.
    val.ulVal = 1;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &val);
  }

  // MFT is configured for one fixed, even NV12 frame size. Reject a malformed
  // or unaligned frame before conversion rather than guessing a different
  // stride: MergeUVPlane writes width+1 bytes per chroma row for odd widths,
  // while the configured MFT media type still describes a width-byte stride.
  // requested_resolution_alignment = 2 keeps valid WebRTC traffic on the fast
  // path; this guard makes a contract violation fail closed instead of risking
  // heap corruption or chroma-row overlap.
  if (frame_buffer->width() != width_ || frame_buffer->height() != height_ ||
      (width_ & 1) != 0 || (height_ & 1) != 0) {
    RTC_LOG(LS_ERROR) << "MFT received invalid NV12 frame dimensions: "
                      << frame_buffer->width() << "x" << frame_buffer->height()
                      << " (configured " << width_ << "x" << height_ << ")";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  Microsoft::WRL::ComPtr<IMFSample> input_sample;
  HRESULT hr = S_OK;
  if (d3d_) {
    const int32_t result =
        texture ? TextureSample(*texture, &input_sample)
                : UploadSample(nv12_buffer, i420_buffer.get(), &input_sample);
    if (result == kSkipFrame) {
      livekit::mft_timing().dropped.fetch_add(1, std::memory_order_relaxed);
      return FinishEncodeAttempt();
    }
    if (result != WEBRTC_VIDEO_CODEC_OK)
      return result;
    // Tests only (never set in production): stand in for an MFT that takes
    // the D3D manager and then fails on texture-mode samples.
    if (livekit::d3d_input_fail_next().exchange(false, std::memory_order_relaxed))
      return RuntimeFailure("simulated texture input failure", E_FAIL);
    if (texture) {
      livekit::mft_timing().texture_frames.fetch_add(1, std::memory_order_relaxed);
      livekit::mft_diag_flag(2048);
    } else {
      livekit::mft_timing().memory_frames.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    // With the validated even dimensions this is the canonical contiguous
    // NV12 layout: a full-resolution Y plane followed by a half-height
    // interleaved UV plane, both with width-byte stride.
    const DWORD luma_size = width_ * height_;
    const DWORD chroma_size = width_ * (height_ / 2);
    const DWORD nv12_size = luma_size + chroma_size;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> input_buffer;
    hr = MFCreateMemoryBuffer(nv12_size, input_buffer.GetAddressOf());
    if (FAILED(hr))
      return RuntimeFailure("allocate input buffer", hr);

    BYTE* buffer_data = nullptr;
    hr = input_buffer->Lock(&buffer_data, nullptr, nullptr);
    if (FAILED(hr))
      return RuntimeFailure("lock input buffer", hr);

    if (nv12_buffer) {
      libyuv::CopyPlane(nv12_buffer->DataY(), nv12_buffer->StrideY(),
                        buffer_data, width_, width_, height_);
      libyuv::CopyPlane(nv12_buffer->DataUV(), nv12_buffer->StrideUV(),
                        buffer_data + luma_size, width_, width_, height_ / 2);
    } else {
      I420ToNV12(i420_buffer.get(), buffer_data, width_);
    }

    hr = input_buffer->Unlock();
    if (FAILED(hr))
      return RuntimeFailure("unlock input buffer", hr);
    hr = input_buffer->SetCurrentLength(nv12_size);
    if (FAILED(hr))
      return RuntimeFailure("set input buffer length", hr);

    hr = MFCreateSample(input_sample.GetAddressOf());
    if (FAILED(hr))
      return RuntimeFailure("create input sample", hr);

    hr = input_sample->AddBuffer(input_buffer.Get());
    if (FAILED(hr))
      return RuntimeFailure("attach input buffer", hr);
    livekit::mft_timing().memory_frames.fetch_add(1, std::memory_order_relaxed);
  }
  // MF sample time is in 100-ns units. rtp_timestamp() is a 90kHz RTP tick
  // count, not 100-ns, so feeding it here fed QSV/AMF rate control a bogus
  // timeline. timestamp_us() * 10 converts microseconds to 100-ns units, which
  // matches the SampleDuration below (10^7 100-ns == 1 second).
  const LONGLONG sample_time_100ns =
      static_cast<LONGLONG>(input_frame.timestamp_us()) * 10;
  hr = input_sample->SetSampleTime(sample_time_100ns);
  if (FAILED(hr))
    return RuntimeFailure("set input sample timestamp", hr);
  hr = input_sample->SetSampleDuration(10000000LL / max_framerate_);
  if (FAILED(hr))
    return RuntimeFailure("set input sample duration", hr);
  livekit::mft_timing().copy.Note(livekit::mft_now_us() - copy_start);

  if (!is_async_) {
    const uint64_t submitted_us = livekit::mft_now_us();
    {
      livekit::MftPhaseScope timing(livekit::mft_timing().submit);
      hr = transform_->ProcessInput(input_stream_id_, input_sample.Get(), 0);
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "MFT ProcessInput failed: 0x" << std::hex << hr;
      livekit::mft_diag_stage(12, static_cast<uint32_t>(hr));
      return RuntimeFailure("ProcessInput", hr);
    }
    key_frame_request_ = false;
    const int32_t result = ProcessEncodedOutput(&input_frame, false, submitted_us);
    return result == WEBRTC_VIDEO_CODEC_OK ? FinishEncodeAttempt() : result;
  }

  FrameMeta meta;
  meta.submitted_us = livekit::mft_now_us();
  meta.sample_time_100ns = sample_time_100ns;
  meta.rtp_timestamp = input_frame.rtp_timestamp();
  meta.ntp_time_ms = input_frame.ntp_time_ms();
  meta.render_time_ms = input_frame.render_time_ms();
  meta.rotation = input_frame.rotation();
  meta.color_space = input_frame.color_space();
  pending_meta_.push_back(meta);
  // Bound metadata, not output latency. Keep enough timestamps for a driver
  // buffering a burst so later output can still be matched exactly. The
  // progress watchdog handles a transform that absorbs input indefinitely.
  while (pending_meta_.size() > 64)
    pending_meta_.pop_front();
  livekit::mft_note_pending(pending_meta_.size(), pending_meta_.front().submitted_us);

  {
    livekit::MftPhaseScope timing(livekit::mft_timing().submit);
    hr = transform_->ProcessInput(input_stream_id_, input_sample.Get(), 0);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT ProcessInput failed: 0x" << std::hex << hr;
    livekit::mft_diag_stage(12, static_cast<uint32_t>(hr));
    pending_meta_.pop_back();
    livekit::mft_note_pending(pending_meta_.size(),
        pending_meta_.empty() ? 0 : pending_meta_.front().submitted_us);
    return RuntimeFailure("ProcessInput", hr);
  }
  input_credits_--;
  key_frame_request_ = false;
  if (event_pump_) return FinishEncodeAttempt();

  // Deliver with minimal added latency: hardware encoders in low-latency
  // mode return in single-digit milliseconds, so poll briefly for this
  // frame's HaveOutput. If it is not ready inside the window, the next
  // Encode's pump delivers it - deferred, never lost.
  const ULONGLONG out_deadline = GetTickCount64() + 20;
  livekit::MftPhaseScope timing(livekit::mft_timing().output_wait);
  for (;;) {
    if (!PumpMftEvents())
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    const bool delivered = output_credits_ > 0;
    const int32_t result = DrainReadyOutput();
    if (result != WEBRTC_VIDEO_CODEC_OK)
      return result;
    if (delivered || GetTickCount64() > out_deadline)
      break;
    Sleep(1);
  }
  return FinishEncodeAttempt();
}

int32_t MftH264EncoderImpl::ProcessEncodedOutput(
    const VideoFrame* input_frame,
    bool single_shot, uint64_t submitted_us) {
  MFT_OUTPUT_STREAM_INFO stream_info = {};
  HRESULT hr =
      transform_->GetOutputStreamInfo(output_stream_id_, &stream_info);
  if (FAILED(hr))
    return RuntimeFailure("GetOutputStreamInfo", hr);

  for (int outputs = 0; outputs < 64; ++outputs) {
    const bool provides_samples =
        (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = output_stream_id_;

    Microsoft::WRL::ComPtr<IMFSample> our_sample;
    if (!provides_samples) {
      Microsoft::WRL::ComPtr<IMFMediaBuffer> out_buf;
      DWORD buf_size =
          stream_info.cbSize > 0 ? stream_info.cbSize : width_ * height_ * 2;
      hr = MFCreateMemoryBuffer(buf_size, out_buf.GetAddressOf());
      if (FAILED(hr))
        return RuntimeFailure("allocate output buffer", hr);

      hr = MFCreateSample(our_sample.GetAddressOf());
      if (FAILED(hr))
        return RuntimeFailure("create output sample", hr);
      hr = our_sample->AddBuffer(out_buf.Get());
      if (FAILED(hr))
        return RuntimeFailure("attach output buffer", hr);
      output_data.pSample = our_sample.Get();
    }

    DWORD status = 0;
    {
      livekit::MftPhaseScope timing(livekit::mft_timing().output);
      hr = transform_->ProcessOutput(0, 1, &output_data, &status);
    }

    // Own everything the transform returns before branching on HRESULT.
    // Stream-change and error responses can carry events too.
    Microsoft::WRL::ComPtr<IMFCollection> output_events;
    output_events.Attach(output_data.pEvents);
    Microsoft::WRL::ComPtr<IMFSample> returned_sample;
    if (output_data.pSample && output_data.pSample != our_sample.Get())
      returned_sample.Attach(output_data.pSample);

    if (!single_shot && hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      break;

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // The encoder has finalized its output format and is telling us what
      // SPS+PPS it will use. We must (a) accept the new output type so the
      // MFT actually starts producing samples, and (b) cache the sequence
      // header blob so we can prepend it to every IDR below — many MFT
      // implementations (Intel QuickSync, AMD VCE, the NVENC-over-MFT shim)
      // do NOT inline SPS/PPS in the bitstream by default, and without them
      // the receiver cannot initialize its decoder and shows a black frame
      // for the entire session.
      bool output_type_set = false;
      DWORD type_index = 0;
      while (type_index < 64) {
        Microsoft::WRL::ComPtr<IMFMediaType> new_output_type;
        HRESULT gt_hr = transform_->GetOutputAvailableType(
            output_stream_id_, type_index, new_output_type.GetAddressOf());
        if (FAILED(gt_hr))
          break;

        GUID subtype = {};
        if (SUCCEEDED(new_output_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_H264) {
          std::vector<uint8_t> new_header;
          UINT32 header_size = 0;
          if (SUCCEEDED(new_output_type->GetBlobSize(
                  MF_MT_MPEG_SEQUENCE_HEADER, &header_size)) &&
              header_size > 0) {
            new_header.resize(header_size);
            if (SUCCEEDED(new_output_type->GetBlob(
                    MF_MT_MPEG_SEQUENCE_HEADER, new_header.data(),
                    header_size, nullptr))) {
              RTC_LOG(LS_INFO)
                  << "MFT captured SPS+PPS sequence header (" << header_size
                  << " bytes) for IDR prepend.";
            } else {
              new_header.clear();
            }
          }
          hr = transform_->SetOutputType(output_stream_id_, new_output_type.Get(), 0);
          if (FAILED(hr)) {
            ++type_index;
            continue;
          }
          sequence_header_ = std::move(new_header);
          output_type_set = true;
          break;
        }
        type_index++;
      }
      if (!output_type_set) {
        // Preserve the old compatibility path, but do not pretend success
        // when it fails or retain a header from a rejected media type.
        if (!ConfigureOutputType())
          return RuntimeFailure("negotiate changed output type", MF_E_INVALIDMEDIATYPE);
        sequence_header_.clear();
      }
      if (sequence_header_.empty()) {
        RTC_LOG(LS_WARNING)
            << "MFT stream change did not yield an SPS+PPS header blob; "
               "decoders may be unable to initialize until a subsequent "
               "stream change delivers one.";
      }
      // The stream-change consumed this call's one HaveOutput credit; the
      // first real bitstream arrives as its own event. Looping here would
      // call ProcessOutput with no pending output.
      if (single_shot)
        return WEBRTC_VIDEO_CODEC_OK;
      hr = transform_->GetOutputStreamInfo(output_stream_id_, &stream_info);
      if (FAILED(hr))
        return RuntimeFailure("refresh output stream info", hr);
      continue;
    }

    if (FAILED(hr)) {
      return RuntimeFailure("ProcessOutput", hr);
    }

    IMFSample* result_sample =
        provides_samples ? output_data.pSample : our_sample.Get();
    if (!result_sample) {
      if (single_shot)
        return WEBRTC_VIDEO_CODEC_OK;
      continue;
    }

    // Resolve whose frame this bitstream is. Sync MFTs answer within the
    // same Encode call, so the caller's frame is authoritative. The async
    // pipeline returns output for an EARLIER submission; its metadata comes
    // from the pending queue, matched by the sample time the transform is
    // required to carry through.
    FrameMeta meta;
    bool have_meta = false;
    if (input_frame) {
      meta.submitted_us = submitted_us;
      meta.rtp_timestamp = input_frame->rtp_timestamp();
      meta.ntp_time_ms = input_frame->ntp_time_ms();
      meta.render_time_ms = input_frame->render_time_ms();
      meta.rotation = input_frame->rotation();
      meta.color_space = input_frame->color_space();
      have_meta = true;
    } else {
      LONGLONG out_time = 0;
      hr = result_sample->GetSampleTime(&out_time);
      if (FAILED(hr))
        return RuntimeFailure("output sample timestamp", hr);
      const auto match = std::find_if(pending_meta_.begin(), pending_meta_.end(),
          [out_time](const FrameMeta& entry) {
            return entry.sample_time_100ns == out_time;
          });
      if (match != pending_meta_.end()) {
        // Earlier entries represent encoder-dropped inputs. An older output
        // whose metadata aged out must NOT steal the next input's RTP stamp.
        meta = *match;
        pending_meta_.erase(pending_meta_.begin(), std::next(match));
        livekit::mft_note_pending(pending_meta_.size(),
            pending_meta_.empty() ? 0 : pending_meta_.front().submitted_us);
        have_meta = true;
      }
    }
    if (!have_meta) {
      RTC_LOG(LS_WARNING)
          << "MFT output with no pending frame metadata; discarding";
      key_frame_request_ = true;
      if (single_shot)
        return WEBRTC_VIDEO_CODEC_OK;
      continue;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> result_buffer;
    hr = result_sample->ConvertToContiguousBuffer(result_buffer.GetAddressOf());
    if (FAILED(hr))
      return RuntimeFailure("read output buffer", hr);

    BYTE* data = nullptr;
    DWORD data_length = 0;
    hr = result_buffer->Lock(&data, nullptr, &data_length);
    if (FAILED(hr))
      return RuntimeFailure("lock output buffer", hr);
    if (data_length == 0) {
      result_buffer->Unlock();
      if (single_shot)
        return WEBRTC_VIDEO_CODEC_OK;
      continue;
    }

    encoded_image_._encodedWidth = width_;
    encoded_image_._encodedHeight = height_;
    encoded_image_.SetRtpTimestamp(meta.rtp_timestamp);
    encoded_image_.SetSimulcastIndex(0);
    encoded_image_.ntp_time_ms_ = meta.ntp_time_ms;
    encoded_image_.capture_time_ms_ = meta.render_time_ms;
    encoded_image_.rotation_ = meta.rotation;
    // Tag RTP content type from codec_.mode. Leaving this permanently
    // UNSPECIFIED told the SFU that screenshare streams were generic
    // video, which breaks screenshare-aware bandwidth estimation and
    // any downstream tooling that filters on content type.
    encoded_image_.content_type_ = (codec_.mode == VideoCodecMode::kScreensharing)
                                       ? VideoContentType::SCREENSHARE
                                       : VideoContentType::UNSPECIFIED;
    encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
    encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
    encoded_image_.SetColorSpace(meta.color_space);

    bool has_inline_sps = false;
    auto nalu_indices =
        H264::FindNaluIndices(MakeArrayView(data, data_length));
    for (const auto& nalu : nalu_indices) {
      H264::NaluType nalu_type =
          H264::ParseNaluType(data[nalu.payload_start_offset]);
      if (nalu_type == H264::kIdr) {
        encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
      } else if (nalu_type == H264::kSps) {
        has_inline_sps = true;
      }
    }

    // If this is an IDR without inline parameter sets, prepend the cached
    // SPS+PPS sequence header so the receiver's decoder can initialize.
    // Non-IDR frames and IDRs that already carry SPS inline pass through
    // unchanged.
    const bool should_prepend_header =
        encoded_image_._frameType == VideoFrameType::kVideoFrameKey &&
        !has_inline_sps && !sequence_header_.empty();

    if (should_prepend_header) {
      std::vector<uint8_t> combined;
      combined.reserve(sequence_header_.size() +
                       static_cast<size_t>(data_length));
      combined.insert(combined.end(), sequence_header_.begin(),
                      sequence_header_.end());
      combined.insert(combined.end(), data, data + data_length);
      encoded_image_.SetEncodedData(
          EncodedImageBuffer::Create(combined.data(), combined.size()));
      encoded_image_.set_size(combined.size());
    } else {
      encoded_image_.SetEncodedData(
          EncodedImageBuffer::Create(data, data_length));
      encoded_image_.set_size(data_length);
    }
    result_buffer->Unlock();

    h264_bitstream_parser_.ParseBitstream(encoded_image_);
    encoded_image_.qp_ =
        h264_bitstream_parser_.GetLastSliceQp().value_or(-1);

    CodecSpecificInfo codec_info;
    codec_info.codecType = kVideoCodecH264;
    codec_info.codecSpecific.H264.packetization_mode =
        H264PacketizationMode::NonInterleaved;

    if (callback_) {
      auto result = callback_->OnEncodedImage(encoded_image_, &codec_info);
      if (result.error != EncodedImageCallback::Result::OK) {
        RTC_LOG(LS_ERROR) << "MFT encode callback failed: " << result.error;
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
    }
    progress_.OutputDelivered(GetTickCount64());
    const auto output_now = livekit::mft_now_us();
    livekit::mft_note_output(output_now);
    // Feeds the screen-share keyframe bound in Encode(). GOP IDRs count too,
    // and only the NAL parse above can see those.
    if (encoded_image_._frameType == VideoFrameType::kVideoFrameKey)
      last_key_frame_us_ = output_now;
    if (meta.submitted_us)
      livekit::mft_timing().residence.Note(output_now - meta.submitted_us);
    // Do not let a past feed timeout look like an active failure after real
    // output resumed. Leave rate-control errors visible until SetRates fixes
    // them; a successful bitrate setter is not proof of encoded output.
    if (livekit::mft_diag().stage.load(std::memory_order_relaxed) == 11)
      livekit::mft_diag_stage(10, 0);
    if (single_shot)
      return WEBRTC_VIDEO_CODEC_OK;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo MftH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  // Texture frames pass straight through only while texture input is on;
  // otherwise WebRTC maps them to the preferred formats below first.
  info.supports_native_handle =
      texture_input_on_.load(std::memory_order_relaxed);
  info.implementation_name = encoder_name_;
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  // Ask WebRTC for even width/height. The I420->NV12 conversion assumes even
  // chroma dimensions; odd widths otherwise overran the NV12 buffer during
  // MergeUVPlane (see Encode()).
  info.requested_resolution_alignment = 2;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNV12,
                                  VideoFrameBuffer::Type::kI420};
  return info;
}

void MftH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  auto lock = event_pump_ ? event_pump_->Lock()
                          : std::unique_lock<std::recursive_mutex>();
  // Keep the failure breadcrumb intact while the adapter switches encoders
  // (or while a fail-closed isolation arm reports the error).
  if (runtime_failed_)
    return;
  if (!transform_) {
    RTC_LOG(LS_WARNING) << "MFT SetRates() while uninitialized.";
    return;
  }

  // isfinite too, not just the range check: NaN compares false against
  // everything, so it sails past `< 1.0` and the cast below is UB. Ported
  // from upstream #1297, which also confirmed our bps->kbps maxBitrate fix.
  if (!std::isfinite(parameters.framerate_fps) ||
      parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  if (parameters.bitrate.get_sum_bps() == 0) {
    sending_ = false;
    progress_.Reset();
    return;
  }

  const uint32_t new_target_bps = parameters.bitrate.GetSpatialLayerSum(0);
  const uint32_t new_framerate =
      static_cast<uint32_t>(parameters.framerate_fps);
  const uint32_t bitrate_delta =
      new_target_bps > target_bps_
          ? new_target_bps - target_bps_
          : target_bps_ - new_target_bps;
  const uint32_t bitrate_threshold =
      std::max<uint32_t>(100000u, target_bps_ / 20u);
  const bool framerate_changed = new_framerate != max_framerate_;

  // CODECAPI rate changes can flush/re-prime some vendor MFTs. As on the
  // direct NVENC path, accumulate insignificant BWE noise rather than
  // injecting a cadence hitch several times per second.
  if (bitrate_delta < bitrate_threshold && !framerate_changed) {
    if (!sending_)
      key_frame_request_ = true;
    sending_ = true;
    return;
  }

  max_framerate_ = new_framerate;

  if (codec_api_) {
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_UI4;
    val.ulVal = new_target_bps;
    HRESULT hr =
        codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &val);
    auto& diag = livekit::mft_diag();
    diag.flags.fetch_and(~512u, std::memory_order_relaxed);
    if (SUCCEEDED(hr)) {
      livekit::mft_diag_flag(256);

      VARIANT actual;
      VariantInit(&actual);
      HRESULT read_hr =
          codec_api_->GetValue(&CODECAPI_AVEncCommonMeanBitRate, &actual);
      if (SUCCEEDED(read_hr) && actual.vt == VT_UI4) {
        const uint32_t actual_delta = actual.ulVal > new_target_bps
                                          ? actual.ulVal - new_target_bps
                                          : new_target_bps - actual.ulVal;
        const uint32_t tolerance =
            std::max<uint32_t>(100000u, new_target_bps / 20u);
        if (actual_delta <= tolerance) {
          livekit::mft_diag_flag(512);
          if (diag.stage.load(std::memory_order_relaxed) != 11)
            livekit::mft_diag_stage(10, 0);
          target_bps_ = new_target_bps;
          bitrate_failure_logged_ = false;
        } else {
          // A repeated write of the same value will not make a driver that
          // clamps or ignores it change its mind, and some MFTs re-prime on a
          // CODECAPI update. Remember the requested target so ordinary BWE
          // polling cannot turn this diagnostic into a cadence hitch storm;
          // the next materially different target still gets a fresh attempt.
          target_bps_ = new_target_bps;
          livekit::mft_diag_stage(13, static_cast<uint32_t>(E_FAIL));
          if (!bitrate_failure_logged_) {
            bitrate_failure_logged_ = true;
            RTC_LOG(LS_ERROR)
                << "MFT accepted but did not apply bitrate update: "
                << "requested=" << new_target_bps
                << " readback=" << actual.ulVal;
          }
        }
      } else {
        // Some vendor MFTs implement SetValue but not GetValue. A successful
        // setter is still the strongest signal available; trust it while the
        // application-side realised-bitrate telemetry watches the result.
        if (diag.stage.load(std::memory_order_relaxed) != 11)
          livekit::mft_diag_stage(10, 0);
        target_bps_ = new_target_bps;
        bitrate_failure_logged_ = false;
      }
      VariantClear(&actual);
    } else {
      livekit::mft_diag_stage(13, static_cast<uint32_t>(hr));
      if (!bitrate_failure_logged_) {
        bitrate_failure_logged_ = true;
        RTC_LOG(LS_ERROR) << "MFT rejected live bitrate update to "
                          << new_target_bps << " bps: 0x" << std::hex << hr;
      }
    }
  }

  if (new_target_bps > 0) {
    if (!sending_)
      key_frame_request_ = true;
    sending_ = true;
  } else {
    sending_ = false;
  }
}

}  // namespace webrtc
