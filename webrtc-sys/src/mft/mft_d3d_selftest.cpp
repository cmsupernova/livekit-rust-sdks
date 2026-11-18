// Hardware tests of the MFT encoder, built only with the mft-selftest
// feature. Both run the production encoder class against a real hardware MFT.
//
// Texture input: a few system-memory frames first (the capturer's first frames
// arrive before texture input is on), then keyed-mutex NV12 textures produced
// on a second device, and finally a texture read back through ToI420().
//
// Rate control: the encoder is opened low and walked through a run of live
// targets on a picture that wants every bit, and each stretch reports what
// its frames cost and what the MFT said about its rate and its buffer.

#include "livekit/mft_d3d_selftest.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "livekit/d3d11_texture_buffer.h"
#include "livekit/mft_timing.h"
#include "livekit/nvenc_timing.h"
#include "mft_h264_encoder_impl.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace livekit_ffi {
namespace {

using Microsoft::WRL::ComPtr;

class CountingCallback : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(const webrtc::EncodedImage& image,
                        const webrtc::CodecSpecificInfo*) override {
    frames.fetch_add(1);
    if (image._frameType == webrtc::VideoFrameType::kVideoFrameKey)
      keys.fetch_add(1);
    return Result(Result::OK);
  }
  std::atomic<uint32_t> frames{0};
  std::atomic<uint32_t> keys{0};
};

UINT32 H264MftsOnAdapter(const LUID& luid) {
  MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_H264};
  ComPtr<IMFAttributes> filter;
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  if (FAILED(MFCreateAttributes(filter.GetAddressOf(), 1)) ||
      FAILED(filter->SetBlob(MFT_ENUM_ADAPTER_LUID,
                             reinterpret_cast<const UINT8*>(&luid),
                             sizeof(luid))) ||
      FAILED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER,
                      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                      &in, &out, filter.Get(), &activates, &count)))
    return 0;
  for (UINT32 i = 0; i < count; ++i)
    activates[i]->Release();
  CoTaskMemFree(activates);
  return count;
}

// The `ordinal`-th hardware adapter, in DXGI order (the default adapter
// first, as the capturer's device uses), with an H.264 encoder MFT on it.
uint64_t AdapterWithH264Mft(uint32_t ordinal, uint32_t* mft_count) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return 0;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      return 0;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) ||
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      continue;
    const UINT32 count = H264MftsOnAdapter(desc.AdapterLuid);
    if (count > 0 && ordinal-- == 0) {
      *mft_count = count;
      return LuidValue(desc.AdapterLuid);
    }
  }
}

uint8_t PatternY(int x, int y) {
  return static_cast<uint8_t>(16 + (x + y) % 220);
}
constexpr uint8_t kPatternU = 90;
constexpr uint8_t kPatternV = 160;

void FillPattern(uint8_t* luma, size_t luma_pitch, uint8_t* chroma,
                 size_t chroma_pitch, int width, int height) {
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      luma[y * luma_pitch + x] = PatternY(x, y);
  for (int y = 0; y < height / 2; ++y) {
    for (int x = 0; x < width; x += 2) {
      chroma[y * chroma_pitch + x] = kPatternU;
      chroma[y * chroma_pitch + x + 1] = kPatternV;
    }
  }
}

bool ReadbackMatches(const webrtc::scoped_refptr<webrtc::I420BufferInterface>& i420,
                     int width, int height) {
  if (!i420 || i420->width() != width || i420->height() != height)
    return false;
  const int points[][2] = {{0, 0}, {width / 2, height / 3}, {width - 1, height - 1},
                           {7, height - 5}, {width - 9, 3}};
  for (const auto& p : points) {
    if (i420->DataY()[p[1] * i420->StrideY() + p[0]] != PatternY(p[0], p[1]))
      return false;
    const int cx = p[0] / 2, cy = p[1] / 2;
    if (i420->DataU()[cy * i420->StrideU() + cx] != kPatternU ||
        i420->DataV()[cy * i420->StrideV() + cx] != kPatternV)
      return false;
  }
  return true;
}

}  // namespace

MftD3dSelfTest mft_d3d_selftest(uint32_t adapter_ordinal,
                                uint32_t width,
                                uint32_t height,
                                uint32_t cpu_frames,
                                uint32_t texture_frames) {
  MftD3dSelfTest r{};
  r.init_result = -1000;
  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);

  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  r.adapter_luid = AdapterWithH264Mft(adapter_ordinal, &r.mft_count);
  auto adapter = FindAdapterByLuid(r.adapter_luid);

  // The capturer's side: its own device, one shared NV12 texture filled with
  // a known pattern under key 0.
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> shared;
  HANDLE handle = nullptr;
  if (adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    r.vendor_id = desc.VendorId;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                    nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                    &device, nullptr, &context))) {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = width;
      td.Height = height;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_NV12;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      ComPtr<IDXGIResource> resource;
      ComPtr<IDXGIKeyedMutex> mutex;
      D3D11_TEXTURE2D_DESC staging_desc = td;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.BindFlags = 0;
      staging_desc.MiscFlags = 0;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ComPtr<ID3D11Texture2D> staging;
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &shared)) &&
          SUCCEEDED(shared.As(&resource)) &&
          SUCCEEDED(resource->GetSharedHandle(&handle)) &&
          SUCCEEDED(shared.As(&mutex)) &&
          SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &staging)) &&
          SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
        auto* luma = static_cast<uint8_t*>(mapped.pData);
        FillPattern(luma, mapped.RowPitch, luma + mapped.RowPitch * height,
                    mapped.RowPitch, w, h);
        context->Unmap(staging.Get(), 0);
        if (AcquireKeyedMutex(mutex.Get(), 0, 1000)) {
          context->CopyResource(shared.Get(), staging.Get());
          mutex->ReleaseSync(0);
          context->Flush();
        } else {
          shared.Reset();
        }
      } else {
        shared.Reset();
      }
    }
  }

  if (shared) {
    auto cpu = webrtc::NV12Buffer::Create(w, h);
    FillPattern(cpu->MutableDataY(), cpu->StrideY(), cpu->MutableDataUV(),
                cpu->StrideUV(), w, h);

    auto& timing = livekit::mft_timing();
    timing.texture_frames.store(0);
    timing.memory_frames.store(0);
    timing.dropped.store(0);
    livekit::d3d_input_failed().store(0);
    livekit::d3d_input_reset();
    livekit::d3d_input_any_vendor().store(true);
    livekit::d3d_input_requested().store(r.adapter_luid);
    auto& diag = livekit::mft_diag();
    // Flags accumulate for the process; each adapter reports its own.
    diag.flags.store(0);
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = static_cast<uint16_t>(width);
    codec.height = static_cast<uint16_t>(height);
    codec.maxFramerate = 30;
    codec.startBitrate = 2000;
    codec.minBitrate = 300;
    codec.maxBitrate = 4000;
    codec.mode = webrtc::VideoCodecMode::kScreensharing;
    codec.numberOfSimulcastStreams = 0;
    const webrtc::VideoEncoder::Settings settings(
        webrtc::VideoEncoder::Capabilities(false), 1, 1200);
    int64_t next_ts = 1000000;
    auto encode = [&](webrtc::MftH264EncoderImpl& encoder, bool texture,
                      bool key) {
      webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
      if (texture)
        buffer = D3D11TextureBuffer::Create(shared.Get(), handle,
                                            /*texture_id=*/1, r.adapter_luid,
                                            w, h);
      else
        buffer = cpu;
      const auto frame = webrtc::VideoFrame::Builder()
                             .set_video_frame_buffer(buffer)
                             .set_timestamp_us(next_ts)
                             .set_rtp_timestamp(static_cast<uint32_t>(next_ts * 9 / 100))
                             .build();
      next_ts += 33333;
      std::vector<webrtc::VideoFrameType> types{
          key ? webrtc::VideoFrameType::kVideoFrameKey
              : webrtc::VideoFrameType::kVideoFrameDelta};
      const int32_t result = encoder.Encode(frame, &types);
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
      return result;
    };
    {
      webrtc::MftH264EncoderImpl encoder(webrtc::CreateEnvironment());
      CountingCallback callback;
      encoder.RegisterEncodeCompleteCallback(&callback);
      r.init_result = encoder.InitEncode(&codec, settings);
      r.d3d_stage = diag.d3d_stage.load();
      r.d3d_hr = diag.d3d_hr.load();
      r.mft_stage = diag.stage.load();
      r.mft_hr = diag.hr.load();
      const auto info = encoder.GetEncoderInfo();
      r.native_handle = info.supports_native_handle;
      r.encoder_name = info.implementation_name;
      r.active_luid = livekit::d3d_input_active_luid();

      if (r.init_result == WEBRTC_VIDEO_CODEC_OK) {
        const uint32_t total = cpu_frames + texture_frames;
        for (uint32_t i = 0; i < total; ++i) {
          const int32_t result = encode(encoder, i >= cpu_frames, i == 0);
          if (result != WEBRTC_VIDEO_CODEC_OK && r.first_error == 0)
            r.first_error = result;
        }
      }
      r.flags = diag.flags.load();
      r.texture_frames = timing.texture_frames.exchange(0);
      r.memory_frames = timing.memory_frames.exchange(0);
      r.dropped = timing.dropped.exchange(0);
      r.encoded_frames = callback.frames.load();
      r.key_frames = callback.keys.load();

      // The safety net: a runtime failure in texture mode brings the same
      // encoder back up the classic way instead of handing it to software.
      if (r.init_result == WEBRTC_VIDEO_CODEC_OK) {
        livekit::d3d_input_fail_next().store(true);
        const uint32_t before = callback.frames.load();
        for (uint32_t i = 0; i < 20; ++i) {
          const int32_t result = encode(encoder, /*texture=*/true, false);
          if (result != WEBRTC_VIDEO_CODEC_OK && r.fallback_error == 0)
            r.fallback_error = result;
        }
        r.fallback_stage = diag.d3d_stage.load();
        r.fallback_hr = diag.d3d_hr.load();
        r.fallback_native_handle = encoder.GetEncoderInfo().supports_native_handle;
        r.fallback_active_luid = livekit::d3d_input_active_luid();
        r.fallback_encoded = callback.frames.load() - before;
        r.fallback_memory_frames = timing.memory_frames.exchange(0);
        r.fallback_latched = livekit::d3d_input_failed().load() == r.adapter_luid;
      }
      encoder.Release();
      r.active_after_release = livekit::d3d_input_active_luid();
    }
    if (r.init_result == WEBRTC_VIDEO_CODEC_OK) {
      // A later encoder in the same session does not try texture input on
      // the adapter that failed.
      webrtc::MftH264EncoderImpl encoder(webrtc::CreateEnvironment());
      CountingCallback callback;
      encoder.RegisterEncodeCompleteCallback(&callback);
      r.latched_init = encoder.InitEncode(&codec, settings);
      r.latched_native_handle = encoder.GetEncoderInfo().supports_native_handle;
      r.latched_stage = diag.d3d_stage.load();
      r.latched_hr = diag.d3d_hr.load();
      encoder.Release();
    }
    livekit::d3d_input_fail_next().store(false);
    livekit::d3d_input_failed().store(0);
    livekit::d3d_input_failed_hr().store(0);
    livekit::d3d_input_requested().store(0);
    livekit::d3d_input_any_vendor().store(false);

    // Everything that needs pixels goes through this read-back.
    auto readback = D3D11TextureBuffer::Create(shared.Get(), handle,
                                               /*texture_id=*/2,
                                               r.adapter_luid, w, h);
    r.readback_ok = readback && ReadbackMatches(readback->ToI420(), w, h);
    // Rescaling reads back once and scales the NV12 instead of crashing
    // through the default path.
    auto half = readback ? readback->CropAndScale(0, 0, w, h, w / 2, h / 2)
                         : nullptr;
    r.crop_ok = half && half->type() == webrtc::VideoFrameBuffer::Type::kNV12 &&
                half->width() == w / 2 && half->height() == h / 2;

    // Create() refuses anything consumers could not copy as one NV12
    // subresource, and a handle that is not the texture's own.
    D3D11_TEXTURE2D_DESC plain_desc{};
    shared->GetDesc(&plain_desc);
    plain_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> plain;
    D3D11_TEXTURE2D_DESC other_desc{};
    shared->GetDesc(&other_desc);
    ComPtr<ID3D11Texture2D> other;
    HANDLE other_handle = nullptr;
    ComPtr<IDXGIResource> other_resource;
    const bool made =
        SUCCEEDED(device->CreateTexture2D(&plain_desc, nullptr, &plain)) &&
        SUCCEEDED(device->CreateTexture2D(&other_desc, nullptr, &other)) &&
        SUCCEEDED(other.As(&other_resource)) &&
        SUCCEEDED(other_resource->GetSharedHandle(&other_handle));
    r.validation_ok =
        made &&
        D3D11TextureBuffer::Create(shared.Get(), nullptr, 3, r.adapter_luid, w, h) &&
        !D3D11TextureBuffer::Create(shared.Get(), other_handle, 4, r.adapter_luid, w, h) &&
        !D3D11TextureBuffer::Create(plain.Get(), nullptr, 5, r.adapter_luid, w, h) &&
        !D3D11TextureBuffer::Create(shared.Get(), handle, 6, r.adapter_luid, w + 2, h) &&
        !D3D11TextureBuffer::Create(shared.Get(), handle, 7, 0, w, h);
  }

  MFShutdown();
  if (SUCCEEDED(com))
    CoUninitialize();
  return r;
}

namespace {

// Frame sizes per stretch of the rate test.
class SizeCallback : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(const webrtc::EncodedImage& image,
                        const webrtc::CodecSpecificInfo*) override {
    const bool key = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    sizes.push_back({static_cast<uint32_t>(image.size()), key});
    return Result(Result::OK);
  }
  struct Frame {
    uint32_t bytes;
    bool key;
  };
  std::vector<Frame> sizes;
};

// Luma that every frame has to pay for: a slow gradient under low noise that
// is different in each buffer. A low QP spends hundreds of kilobytes on the
// noise and a high one quantizes it away, so the size of a frame is whatever
// the rate control allowed and nothing else.
webrtc::scoped_refptr<webrtc::NV12Buffer> NoiseFrame(int width, int height,
                                                    uint32_t seed) {
  auto buffer = webrtc::NV12Buffer::Create(width, height);
  uint32_t state = seed * 2654435761u + 1;
  uint8_t* luma = buffer->MutableDataY();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      const int noise = static_cast<int>(state % 25) - 12;
      luma[y * buffer->StrideY() + x] =
          static_cast<uint8_t>(96 + ((x + y) >> 4) % 64 + noise);
    }
  }
  memset(buffer->MutableDataUV(), 128,
         static_cast<size_t>(buffer->StrideUV()) * ((height + 1) / 2));
  return buffer;
}

}  // namespace

MftRateSelfTest mft_rate_selftest(uint32_t candidate,
                                  bool unknown_vendor,
                                  uint32_t ceiling_kbps,
                                  uint32_t stretch,
                                  uint32_t width,
                                  uint32_t height) {
  MftRateSelfTest r{};
  r.init_result = -1000;
  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);

  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

  std::vector<webrtc::scoped_refptr<webrtc::NV12Buffer>> noise;
  for (uint32_t i = 0; i < 6; ++i)
    noise.push_back(NoiseFrame(w, h, i + 1));

  // System memory in, the classic way: what a share gets on every adapter
  // but AMD's.
  livekit::d3d_input_failed().store(0);
  livekit::d3d_input_reset();
  livekit::d3d_input_requested().store(0);
  livekit::mft_test_first_candidate().store(candidate);
  livekit::mft_test_unknown_vendor().store(unknown_vendor);
  auto& diag = livekit::mft_diag();
  diag.flags.store(0);

  webrtc::VideoCodec codec;
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = static_cast<uint16_t>(width);
  codec.height = static_cast<uint16_t>(height);
  codec.maxFramerate = 60;
  // A resumed share on a slow link opens about here (field report, Nov 2026).
  codec.startBitrate = 1000;
  codec.minBitrate = 300;
  codec.maxBitrate = ceiling_kbps;
  codec.mode = webrtc::VideoCodecMode::kScreensharing;
  codec.numberOfSimulcastStreams = 0;
  const webrtc::VideoEncoder::Settings settings(
      webrtc::VideoEncoder::Capabilities(false), 1, 1200);

  webrtc::MftH264EncoderImpl encoder(webrtc::CreateEnvironment());
  SizeCallback callback;
  encoder.RegisterEncodeCompleteCallback(&callback);
  r.init_result = encoder.InitEncode(&codec, settings);
  r.encoder_name = encoder.GetEncoderInfo().implementation_name;

  struct Step {
    const char* name;
    uint32_t kbps;  // 0: leave the rate the encoder was opened with
    uint32_t fps;
    uint32_t frames;
    bool still = false;
    // Every sixth frame moves the target between `kbps` and 15% above it.
    bool wobble = false;
  };
  const Step steps[] = {
      {"start-1M", 0, 60, 60},
      {"up-8M", 8000, 60, 90},
      {"up-20M", 20000, 60, 90},
      {"down-2M", 2000, 60, 90},
      {"slow-8M", 8000, 15, 45},
      {"still-8M", 8000, 7, 28, true},
      {"back-8M", 8000, 60, 90},
      {"wobble-8M", 8000, 60, 120, false, true},
  };

  int64_t next_ts = 1000000;
  uint32_t frame_index = 0;
  for (const Step& step : steps) {
    if (r.init_result != WEBRTC_VIDEO_CODEC_OK)
      break;
    MftRatePhase phase{};
    phase.name = step.name;
    phase.target_kbps = step.kbps ? step.kbps : codec.startBitrate;
    phase.fps = step.fps;
    auto set_rate = [&](uint32_t kbps) {
      webrtc::VideoBitrateAllocation allocation;
      allocation.SetBitrate(0, 0, kbps * 1000);
      encoder.SetRates(webrtc::VideoEncoder::RateControlParameters(
          allocation, static_cast<double>(step.fps)));
    };
    if (step.kbps)
      set_rate(step.kbps);
    phase.stage = diag.stage.load();
    phase.hr = diag.hr.load();
    phase.flags = diag.flags.load();
    phase.rate_asked = diag.rate_asked.load();
    phase.rate_reported = diag.rate_reported.load();
    phase.hrd_asked = diag.hrd_asked.load();
    phase.hrd_reported = diag.hrd_reported.load();

    const size_t first = callback.sizes.size();
    const uint32_t frames = step.frames * std::max<uint32_t>(1u, stretch);
    for (uint32_t i = 0; i < frames; ++i) {
      if (step.wobble && i % 6 == 3)
        set_rate(i % 12 == 3 ? step.kbps * 23 / 20 : step.kbps);
      const auto& buffer =
          step.still ? noise[0] : noise[++frame_index % noise.size()];
      const auto frame =
          webrtc::VideoFrame::Builder()
              .set_video_frame_buffer(buffer)
              .set_timestamp_us(next_ts)
              .set_rtp_timestamp(static_cast<uint32_t>(next_ts * 9 / 100))
              .build();
      next_ts += 1000000 / step.fps;
      std::vector<webrtc::VideoFrameType> types{
          webrtc::VideoFrameType::kVideoFrameDelta};
      const auto before = std::chrono::steady_clock::now();
      const int32_t result = encoder.Encode(frame, &types);
      phase.encode_max_ms = std::max<uint32_t>(
          phase.encode_max_ms,
          static_cast<uint32_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - before)
                  .count()));
      if (result != WEBRTC_VIDEO_CODEC_OK && phase.first_error == 0)
        phase.first_error = result;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000 / step.fps));
    }

    phase.end_stage = diag.stage.load();
    phase.end_hr = diag.hr.load();
    phase.end_flags = diag.flags.load();
    phase.end_rate_asked = diag.rate_asked.load();
    phase.end_rate_reported = diag.rate_reported.load();
    phase.end_hrd_asked = diag.hrd_asked.load();
    phase.end_hrd_reported = diag.hrd_reported.load();
    phase.end_encoder = encoder.GetEncoderInfo().implementation_name;
    // The last third, by when a rate taken through a rebuild is in too.
    const size_t count = callback.sizes.size() - first;
    const size_t late = first + count - count / 3;
    uint64_t late_total = 0;
    uint32_t late_frames = 0;
    for (size_t i = first; i < callback.sizes.size(); ++i) {
      const auto& f = callback.sizes[i];
      ++phase.frames;
      phase.total_bytes += f.bytes;
      if (f.key) {
        ++phase.key_frames;
        phase.key_max_bytes = std::max(phase.key_max_bytes, f.bytes);
        continue;
      }
      phase.delta_max_bytes = std::max(phase.delta_max_bytes, f.bytes);
      if (i >= late) {
        late_total += f.bytes;
        ++late_frames;
      }
    }
    phase.late_avg_bytes =
        late_frames ? static_cast<uint32_t>(late_total / late_frames) : 0;
    r.phases.push_back(std::move(phase));
  }
  encoder.Release();

  livekit::mft_test_first_candidate().store(0);
  livekit::mft_test_unknown_vendor().store(false);
  livekit::d3d_input_failed().store(0);
  livekit::d3d_input_failed_hr().store(0);

  MFShutdown();
  if (SUCCEEDED(com))
    CoUninitialize();
  return r;
}

}  // namespace livekit_ffi
