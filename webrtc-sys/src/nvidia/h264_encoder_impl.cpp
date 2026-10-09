#include <chrono>

#include "livekit/nvenc_timing.h"
#include "h264_encoder_impl.h"


#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/video/video_codec_constants.h"
#include "api/video_codecs/scalability_mode.h"
#include <common_video/h264/h264_common.h>
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/svc/create_scalability_structure.h"
#include "modules/video_coding/utility/simulcast_rate_allocator.h"
#include "modules/video_coding/utility/simulcast_utility.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/metrics.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"
#include "third_party/libyuv/include/libyuv/scale.h"

#if defined(_WIN32)
#include "NvEncoder/NvEncoderD3D11.h"
#endif

namespace webrtc {

namespace {

#if defined(_WIN32)
constexpr UINT kNvidiaVendorId = 0x10DE;
// Shared textures stay open across frames; the capturer's ring is smaller.
constexpr size_t kOpenedTextureCache = 8;
// The capturer holds a texture's mutex only while submitting its render.
// Waiting longer than this means it is starved; drop the frame.
constexpr DWORD kTextureAcquireMs = 10;
#endif

uint32_t FrameSizedVbvBuffer(uint32_t bitrate_bps,
                             uint32_t frame_rate_num,
                             uint32_t frame_rate_den) {
  if (frame_rate_num == 0) {
    return bitrate_bps;
  }
  const uint64_t bits_per_frame =
      (static_cast<uint64_t>(bitrate_bps) * frame_rate_den +
       frame_rate_num - 1) /
      frame_rate_num;
  return static_cast<uint32_t>(std::max<uint64_t>(1, bits_per_frame));
}

// Screen-share QP floor, see InitEncode. The MFT path raises its vendor
// default to the inter value (mft_h264_encoder_impl.cpp).
constexpr uint32_t kScreenMinQpInter = 18;
constexpr uint32_t kScreenMinQpIntra = 16;

}  // namespace

// Used by histograms. Values of entries should not be changed.
enum H264EncoderImplEvent {
  kH264EncoderEventInit = 0,
  kH264EncoderEventError = 1,
  kH264EncoderEventMax = 16,
};


NV_ENC_LEVEL H264LevelToNvEncLevel(webrtc::H264Level level) {
  switch (level) {
    case H264Level::kLevel1_b:
      return NV_ENC_LEVEL_H264_1b;
    case H264Level::kLevel1:
      return NV_ENC_LEVEL_H264_1;
    case H264Level::kLevel1_1:
      return NV_ENC_LEVEL_H264_11;
    case H264Level::kLevel1_2:
      return NV_ENC_LEVEL_H264_12;
    case H264Level::kLevel1_3:
      return NV_ENC_LEVEL_H264_13;
    case H264Level::kLevel2:
      return NV_ENC_LEVEL_H264_2;
    case H264Level::kLevel2_1:
      return NV_ENC_LEVEL_H264_21;
    case H264Level::kLevel2_2:
      return NV_ENC_LEVEL_H264_22;
    case H264Level::kLevel3:
      return NV_ENC_LEVEL_H264_3;
    case H264Level::kLevel3_1:
      return NV_ENC_LEVEL_H264_31;
    case H264Level::kLevel3_2:
      return NV_ENC_LEVEL_H264_32;
    case H264Level::kLevel4:
      return NV_ENC_LEVEL_H264_4;
    case H264Level::kLevel4_1:
      return NV_ENC_LEVEL_H264_41;
    case H264Level::kLevel4_2:
      return NV_ENC_LEVEL_H264_42;
    case H264Level::kLevel5:
      return NV_ENC_LEVEL_H264_5;
    case H264Level::kLevel5_1:
      return NV_ENC_LEVEL_H264_51;
    case H264Level::kLevel5_2:
      return NV_ENC_LEVEL_H264_52;
  }
  return NV_ENC_LEVEL_AUTOSELECT;  // Default value.
}


// Map the parsed SDP H264 profile onto the matching NVENC profile GUID.
// NVENC has no dedicated Constrained Baseline GUID, so both Baseline and
// Constrained Baseline map to NV_ENC_H264_PROFILE_BASELINE_GUID. Anything we
// don't explicitly recognize falls back to AUTOSELECT so NVENC picks a valid
// profile itself.
GUID H264ProfileToNvEncGuid(webrtc::H264Profile profile) {
  switch (profile) {
    case H264Profile::kProfileConstrainedBaseline:
    case H264Profile::kProfileBaseline:
      return NV_ENC_H264_PROFILE_BASELINE_GUID;
    case H264Profile::kProfileMain:
      return NV_ENC_H264_PROFILE_MAIN_GUID;
    case H264Profile::kProfileConstrainedHigh:
      return NV_ENC_H264_PROFILE_CONSTRAINED_HIGH_GUID;
    case H264Profile::kProfileHigh:
      return NV_ENC_H264_PROFILE_HIGH_GUID;
    default:
      return NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
  }
}


NvidiaH264EncoderImpl::NvidiaH264EncoderImpl(
    const webrtc::Environment& env,
    CUcontext context,
    CUmemorytype memory_type,
    NV_ENC_BUFFER_FORMAT nv_format,
    const SdpVideoFormat& format)
    : instance_id_(livekit::d3d_input_next_owner()),
      env_(env),
      encoder_(nullptr),
      cu_context_(context),
      cu_memory_type_(memory_type),
      cu_scaled_array_(nullptr),
      nv_format_(nv_format),
      packetization_mode_(
          H264EncoderSettings::Parse(format).packetization_mode),
      format_(format) {
  // profile-level-id is optional in an H.264 fmtp (RFC 6184 defaults it to
  // Baseline), and at() threw std::out_of_range out of this constructor. When
  // absent, keep the Constrained Baseline default: NVENC has one GUID for both.
  const auto profile_level_it = format_.parameters.find("profile-level-id");
  if (profile_level_it != format_.parameters.end()) {
    std::optional<webrtc::H264ProfileLevelId> profile_level_id =
        webrtc::ParseH264ProfileLevelId(profile_level_it->second.c_str());
    if (profile_level_id.has_value()) {
      profile_ = profile_level_id->profile;
      level_ = profile_level_id->level;
    }
  }

  // Not the negotiated level: 42e01f says 3.1, and forcing that made NVENC
  // signal 3.1 for 2560x1440@60, which needs 5.1. The driver picks the level
  // from the actual resolution, frame rate and bitrate.
  nv_enc_level_ = NV_ENC_LEVEL_AUTOSELECT;

  // Resolve the NVENC profile GUID from the parsed profile. Without this the
  // member stayed uninitialized and later overwrote the encoder's default
  // profileGUID with garbage (see InitEncode).
  nv_profile_guid_ = webrtc::H264ProfileToNvEncGuid(profile_);

  RTC_CHECK_NE(cu_memory_type_, CU_MEMORYTYPE_HOST);
}

NvidiaH264EncoderImpl::~NvidiaH264EncoderImpl() {
  Release();
}

void NvidiaH264EncoderImpl::ReportInit() {
  if (has_reported_init_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
                            kH264EncoderEventInit, kH264EncoderEventMax);
  has_reported_init_ = true;
}

void NvidiaH264EncoderImpl::ReportError() {
  if (has_reported_error_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
                            kH264EncoderEventError, kH264EncoderEventMax);
  has_reported_error_ = true;
}

int32_t NvidiaH264EncoderImpl::InitEncode(
    const VideoCodec* inst,
    const VideoEncoder::Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecH264) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->maxFramerate == 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->width < 1 || inst->height < 1) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  int32_t release_ret = Release();
  if (release_ret != WEBRTC_VIDEO_CODEC_OK) {
    ReportError();
    return release_ret;
  }

  codec_ = *inst;

  // Code expects simulcastStream resolutions to be correct, make sure they are
  // filled even when there are no simulcast layers.
  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  // Initialize encoded image. Default buffer size: size of unencoded data.
  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  configuration_.sending = false;
  configuration_.frame_dropping_on = codec_.GetFrameDropEnabled();
  configuration_.key_frame_interval = codec_.H264()->keyFrameInterval;

  configuration_.width = codec_.width;
  configuration_.height = codec_.height;

  configuration_.max_frame_rate = codec_.maxFramerate;
  configuration_.target_bps = codec_.startBitrate * 1000;
  configuration_.max_bps = codec_.maxBitrate * 1000;

  const CUresult result = cuCtxSetCurrent(cu_context_);
  if (result != CUDA_SUCCESS) {
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  // Read once, here, so the delay is fixed for the life of this encoder and
  // every frame in `pending_frames_` was submitted under the same
  // configuration.
  output_delay_ = livekit::nvenc_output_delay().load(std::memory_order_relaxed);
  pending_frames_.clear();

  // Read once, here, so the preset and the multipass setting
  // ConfigureEncodeParams applies cannot disagree, and so a profile switch
  // mid-session cannot split one measurement window across two
  // configurations.
  screen_profile_ =
      livekit::nvenc_screen_profile().load(std::memory_order_relaxed);

  bool started = false;
#if defined(_WIN32)
  if (codec_.mode == VideoCodecMode::kScreensharing)
    d3d_ = OpenD3DInput();
  if (d3d_) {
    started = StartEncoder(true);
    if (started) {
      texture_input_.store(true, std::memory_order_relaxed);
      livekit::d3d_input_publish(instance_id_, d3d_->luid);
    } else {
      // Not tried on this adapter again until the app restarts, so a driver
      // that refuses it does not cost every later share a failed start.
      livekit::d3d_input_failed().store(d3d_->luid, std::memory_order_relaxed);
      d3d_.reset();
    }
  }
#endif
  if (!started && !StartEncoder(false)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  livekit::nvenc_note_encoder_start(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());

  RTC_LOG(LS_INFO) << "NVIDIA H264 NVENC initialized: "
                   << codec_.width << "x" << codec_.height
                   << " @ " << codec_.maxFramerate << "fps, target_bps="
                   << configuration_.target_bps << ", input="
                   << (texture_input_.load(std::memory_order_relaxed)
                           ? "d3d11"
                           : "cuda");

  SimulcastRateAllocator init_allocator(env_, codec_);
  VideoBitrateAllocation allocation =
      init_allocator.Allocate(VideoBitrateAllocationParameters(
          DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));
  return WEBRTC_VIDEO_CODEC_OK;
}

bool NvidiaH264EncoderImpl::StartEncoder(bool d3d11) {
  // Some NVIDIA GPUs have a limited Encode Session count.
  // We can't get the Session count, so catching NvEncThrow to avoid the crash.
  // refer:
  // https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new
  try {
#if defined(_WIN32)
    if (d3d11) {
      encoder_ = std::make_unique<NvEncoderD3D11>(
          d3d_->device.Get(), codec_.width, codec_.height, nv_format_,
          output_delay_);
    } else
#endif
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      encoder_ = std::make_unique<NvEncoderCuda>(
          cu_context_, codec_.width, codec_.height, nv_format_, output_delay_);
    } else {
      RTC_DCHECK_NOTREACHED();
      return false;
    }
    ConfigureEncodeParams();
    encoder_->CreateEncoder(&nv_initialize_params_);
  } catch (const NVENCException& e) {
    // todo: If Encoder initialization fails, need to notify for Managed side.
    RTC_LOG(LS_ERROR) << "Failed Initialize NvEncoder"
                      << (d3d11 ? " for texture input " : " ") << e.what();
    encoder_ = nullptr;
    return false;
  }
  return true;
}

void NvidiaH264EncoderImpl::ConfigureEncodeParams() {
  nv_initialize_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  nv_encode_config_.version = NV_ENC_CONFIG_VER;
  nv_initialize_params_.encodeConfig = &nv_encode_config_;

  GUID encodeGuid = NV_ENC_CODEC_H264_GUID;
  // P3 drops motion-search effort relative to P5. Under ULTRA_LOW_LATENCY
  // tuning there is no lookahead and there are no B-frames to give up, so the
  // preset is very nearly a pure search-effort dial here.
  GUID presetGuid =
      screen_profile_ >= 2 ? NV_ENC_PRESET_P3_GUID : NV_ENC_PRESET_P5_GUID;

  encoder_->CreateDefaultEncoderParams(&nv_initialize_params_, encodeGuid,
                                       presetGuid,
                                       NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

  nv_initialize_params_.frameRateNum =
      static_cast<uint32_t>(configuration_.max_frame_rate);
  nv_initialize_params_.frameRateDen = 1;
  nv_initialize_params_.bufferFormat = nv_format_;

  nv_encode_config_.profileGUID = nv_profile_guid_;
  nv_encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
  nv_encode_config_.frameIntervalP = 1;  // no B-frames (realtime)
  nv_encode_config_.encodeCodecConfig.h264Config.level = nv_enc_level_;
  nv_encode_config_.encodeCodecConfig.h264Config.idrPeriod =
      NVENC_INFINITE_GOPLENGTH;
  // Emit SPS/PPS on every IDR (not just forced ones). Periodic GOP IDRs
  // otherwise carry no parameter sets, so a subscriber that joins between
  // forced keyframes lands on an undecodable IDR.
  nv_encode_config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

  // --- Colour signalling (VUI) ----------------------------------------------
  // Our capture pipeline converts BGRA -> YUV in BT.709 limited range (GPU
  // compute shaders + the CPU dcv-color-primitives fallback), but the NV12/
  // I420 frames carry no colour metadata. Receivers (Chromium / libwebrtc)
  // assume BT.709 only for unsignaled HD and BT.601 for SD, so without an
  // explicit signal the stream is subtly desaturated / hue-shifted (reds and
  // skin tones most visibly). Write a full VUI colour description so nothing
  // depends on the receiver guessing. limited range (videoFullRangeFlag = 0),
  // primaries / transfer / matrix all BT.709. repeatSPSPPS above ensures every
  // periodic IDR carries the SPS with this VUI, so late joiners get it too.
  NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui =
      nv_encode_config_.encodeCodecConfig.h264Config.h264VUIParameters;
  vui.videoSignalTypePresentFlag = 1;
  vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
  vui.videoFullRangeFlag = 0;
  vui.colourDescriptionPresentFlag = 1;
  vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
  vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
  vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

  nv_encode_config_.rcParams.version = NV_ENC_RC_PARAMS_VER;
  nv_encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  nv_encode_config_.rcParams.averageBitRate = configuration_.target_bps;
  // NVENC ignores maxBitRate in CBR mode (averageBitRate is the hard target),
  // so this assignment is inert here. Kept for parity with the rcParams block
  // and in case the rate-control mode is ever switched to VBR/capped VBR.
  nv_encode_config_.rcParams.maxBitRate =
      configuration_.target_bps + configuration_.target_bps / 4;
  // NVIDIA recommends a very small VBV for interactive game streaming. One
  // frame keeps complex frames from borrowing against a full second of
  // bitrate and landing in WebRTC's pacer as a large burst, which otherwise
  // inflates TWCC delay and can trigger an avoidable BWE backoff.
  nv_encode_config_.rcParams.vbvBufferSize = FrameSizedVbvBuffer(
      configuration_.target_bps, nv_initialize_params_.frameRateNum,
      nv_initialize_params_.frameRateDen);
  nv_encode_config_.rcParams.vbvInitialDelay =
      nv_encode_config_.rcParams.vbvBufferSize;
  // Two-pass (quarter-res first pass) is NVIDIA's recommended low-latency CBR
  // pairing: it tightens rate adherence and reduces per-frame overshoot at
  // negligible cost on NVENC-class GPUs.
  //
  // "Negligible cost on NVENC-class GPUs" holds on an idle GPU. It is exactly
  // what stops holding when a game is saturating the same GPU, which is the
  // case this profile exists to measure: the quarter-resolution first pass is
  // real additional analysis work per frame, and the field data showed plenty
  // of QP headroom to give back instead.
  nv_encode_config_.rcParams.multiPass = screen_profile_ == 0
                                             ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION
                                             : NV_ENC_MULTI_PASS_DISABLED;
  // Bound the IDR size relative to the per-frame P budget so periodic GOP
  // keyframes don't spike the wire rate. Under ULTRA_LOW_LATENCY tuning the
  // driver default is 1; 2 keeps IDRs sharp enough for screenshare text while
  // still capping the burst. Tuning knob - worth an on-hardware A/B.
  nv_encode_config_.rcParams.lowDelayKeyFrameScale = 2;

  // --- Adaptive quantisation -------------------------------------------------
  // Spatial AQ: per-macroblock QP adjustment based on spatial complexity —
  // more bits for flat/detailed regions (skin, text, UI), fewer for noisy
  // regions where the extra bits would be wasted. Visible quality
  // improvement (≈5-10% QP reduction in detail areas). Strength 8/15 is
  // NVIDIA's recommended balance for mixed content.
  //
  // Not free under a game: the NVENC programming guide lists all adaptive
  // quantization modes among the encoder features that run on CUDA, so AQ
  // competes for the 3D engine's time slices while the NVENC engine itself
  // sits mostly idle. Profile 4 is the staff A/B arm that turns it off.
  //
  // Temporal AQ is intentionally NOT enabled here: it requires
  // `numRefFrames >= 2` and `enablePTD = 1`, neither of which is configured
  // on this realtime path, and it is explicitly unsupported with
  // `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`. On some driver versions the
  // encoder accepts init with `enableTemporalAQ = 1` anyway and then hard-
  // crashes (no stack trace, the host process just disappears) on the first
  // encode call. Leave it off until we have a full CAPS-query + ref-buffer
  // reconfig path.
  const bool spatial_aq = screen_profile_ != 4;
  nv_encode_config_.rcParams.enableAQ = spatial_aq ? 1 : 0;
  nv_encode_config_.rcParams.aqStrength = spatial_aq ? 8 : 0;

  // --- Screenshare loss-recovery bound (2 s GOP) ----------------------------
  // Field data (two independent viewers logging the same stream) showed that
  // when an uplink loss burst outruns NACK recovery, the receiver freezes for
  // libwebrtc's ~3 s no-decodable-frame timeout before it escalates to a
  // keyframe request. With an infinite GOP that timeout IS the worst case. A
  // 2-second scheduled IDR caps recovery at min(next IDR, PLI) — mean ~1 s,
  // worst ~2 s — at half the overhead of the old 1 s cadence, whose real sin
  // (per-SetRates GOP reconfigure hammering rate control several times a
  // second) is gone now that SetRates is hysteresis-gated. lowDelayKeyFrameScale
  // above bounds the IDR burst size. Camera streams keep the infinite GOP:
  // there is no perceptual upside to periodic IDRs there and the bitrate
  // saving matters more.
  if (codec_.mode == VideoCodecMode::kScreensharing) {
    const uint32_t gop_seconds = screen_profile_ == 3 ? 10u : 2u;
    const uint32_t idr_period =
        std::max<uint32_t>(1u, static_cast<uint32_t>(
                                   configuration_.max_frame_rate) * gop_seconds);
    nv_encode_config_.gopLength = idr_period;
    nv_encode_config_.encodeCodecConfig.h264Config.idrPeriod = idr_period;

    // --- Screenshare QP floor ------------------------------------------------
    // CBR with no floor spends the whole budget on a still screen. The
    // one-frame VBV holds each periodic IDR to a coarse QP, and the P-frames
    // after it then keep refining an unchanged picture toward QP 0 at the full
    // target rate, until the next IDR starts over. That is uplink voice needed
    // (field report, Sep 2026: voice breaking up only while sharing). Past QP
    // 18 the refinement is not visible, so it stops there and a still screen
    // settles into skip frames a few frames after each IDR. Motion at our
    // bitrates encodes well above the floor; a simple scene that would dip
    // under it loses nothing visible. Persists through Reconfigure, which
    // reuses nv_encode_config_.
    nv_encode_config_.rcParams.enableMinQP = 1;
    nv_encode_config_.rcParams.minQP.qpInterP = kScreenMinQpInter;
    nv_encode_config_.rcParams.minQP.qpInterB = kScreenMinQpInter;
    nv_encode_config_.rcParams.minQP.qpIntra = kScreenMinQpIntra;
  }
}

int32_t NvidiaH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::Release() {
#if defined(_WIN32)
  WithdrawTextureInput();
#endif
  if (encoder_) {
    // Drain before teardown. With a non-zero output delay the encoder is
    // still holding frames we submitted; tearing down without an EOS leaves
    // those output buffers locked on some drivers. The flushed packets are
    // discarded rather than delivered: Release runs on stop, on resize and on
    // encoder recreation, and in all three the callback either is gone or is
    // about to receive a fresh keyframe anyway.
    try {
      std::vector<std::vector<uint8_t>> flushed;
      encoder_->EndEncode(flushed);
    } catch (const NVENCException& e) {
      RTC_LOG(LS_WARNING) << "NvEncoder flush on release failed " << e.what();
    }
    encoder_->DestroyEncoder();
    encoder_ = nullptr;
  }
  pending_frames_.clear();
  livekit::nvenc_note_encoder_released();
  if (cu_scaled_array_) {
    cuArrayDestroy(cu_scaled_array_);
    cu_scaled_array_ = nullptr;
  }
#if defined(_WIN32)
  d3d_.reset();
#endif
  return WEBRTC_VIDEO_CODEC_OK;
}

#if defined(_WIN32)
std::unique_ptr<NvidiaH264EncoderImpl::D3DInput>
NvidiaH264EncoderImpl::OpenD3DInput() {
  const uint64_t luid =
      livekit::d3d_input_requested().load(std::memory_order_relaxed);
  if (luid == 0 ||
      livekit::d3d_input_failed().load(std::memory_order_relaxed) == luid) {
    return nullptr;
  }
  // NVENC opens on NVIDIA adapters only; elsewhere the capturer's adapter is
  // the MFT's to take.
  auto adapter = livekit_ffi::FindAdapterByLuid(luid);
  DXGI_ADAPTER_DESC1 adapter_desc{};
  if (!adapter || FAILED(adapter->GetDesc1(&adapter_desc)) ||
      adapter_desc.VendorId != kNvidiaVendorId) {
    return nullptr;
  }

  auto d3d = std::make_unique<D3DInput>();
  d3d->luid = luid;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, 0, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &d3d->device, nullptr,
                                 &d3d->context);
  if (hr == E_INVALIDARG) {
    // Runtimes without 11.1 reject the whole list.
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                           levels + 1, 1, D3D11_SDK_VERSION, &d3d->device,
                           nullptr, &d3d->context);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "NVENC texture input: D3D11 device failed: 0x"
                        << std::hex << hr << std::dec;
    return nullptr;
  }
  // Opening another device's NV12 texture needs extended resource sharing.
  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  UINT nv12_support = 0;
  if (FAILED(d3d->device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
                                              &options, sizeof(options))) ||
      !options.ExtendedResourceSharing ||
      FAILED(d3d->device->CheckFormatSupport(DXGI_FORMAT_NV12,
                                             &nv12_support)) ||
      !(nv12_support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
    RTC_LOG(LS_WARNING) << "NVENC texture input: no shared NV12 support";
    return nullptr;
  }
  return d3d;
}

void NvidiaH264EncoderImpl::WithdrawTextureInput() {
  texture_input_.store(false, std::memory_order_relaxed);
  // Only this encoder's claim; a newer encoder's stays.
  livekit::d3d_input_withdraw(instance_id_);
  if (d3d_)
    d3d_->opened.clear();
}

int32_t NvidiaH264EncoderImpl::TextureInputFailed(const char* operation,
                                                  long hr) {
  RTC_LOG(LS_WARNING) << "NVENC texture input " << operation << " failed: 0x"
                      << std::hex << hr << std::dec
                      << "; taking frames from memory from here on";
  livekit::d3d_input_failed_hr().store(static_cast<uint32_t>(hr),
                                       std::memory_order_relaxed);
  livekit::d3d_input_failed().store(d3d_->luid, std::memory_order_relaxed);
  WithdrawTextureInput();
  return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
}

int32_t NvidiaH264EncoderImpl::CopyTexture(
    const livekit_ffi::D3D11TextureBuffer& frame,
    ID3D11Texture2D* target) {
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
    if (desc.Format != DXGI_FORMAT_NV12 ||
        desc.Width != static_cast<UINT>(encoder_->GetEncodeWidth()) ||
        desc.Height != static_cast<UINT>(encoder_->GetEncodeHeight()))
      return TextureInputFailed("shared texture shape", E_INVALIDARG);
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    hr = texture.As(&mutex);
    if (FAILED(hr))
      return TextureInputFailed("shared texture mutex", hr);
    if (d3d.opened.size() >= kOpenedTextureCache)
      d3d.opened.pop_front();
    d3d.opened.push_back(
        {frame.texture_id(), std::move(texture), std::move(mutex)});
    opened = std::prev(d3d.opened.end());
  }

  const HRESULT acquired = opened->mutex->AcquireSync(0, kTextureAcquireMs);
  if (acquired == static_cast<HRESULT>(WAIT_TIMEOUT))
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  if (acquired != S_OK) {
    // Abandoned (the capturer's device went away) or a device error.
    d3d.opened.erase(opened);
    return TextureInputFailed("acquire shared texture", acquired);
  }
  d3d.context->CopySubresourceRegion(target, 0, 0, 0, 0,
                                     opened->texture.Get(), 0, nullptr);
  opened->mutex->ReleaseSync(0);
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::UploadFrame(const uint8_t* nv12,
                                           uint32_t stride,
                                           int frame_width,
                                           int frame_height,
                                           ID3D11Texture2D* target) {
  auto& d3d = *d3d_;
  const int width = static_cast<int>(encoder_->GetEncodeWidth());
  const int height = static_cast<int>(encoder_->GetEncodeHeight());
  if (!d3d.upload) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr =
        d3d.device->CreateTexture2D(&desc, nullptr, &d3d.upload);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "NVENC upload texture failed: 0x" << std::hex << hr
                        << std::dec;
      return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    }
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT hr =
      d3d.context->Map(d3d.upload.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "NVENC upload map failed: 0x" << std::hex << hr
                      << std::dec;
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }
  // Both sides are one allocation with the interleaved chroma plane after
  // the luma rows at the same pitch, the layout the CUDA path copies.
  const int copy_width = std::min<int>(frame_width, width);
  const int copy_height = std::min<int>(frame_height, height);
  auto* luma = static_cast<uint8_t*>(mapped.pData);
  libyuv::CopyPlane(nv12, stride, luma, mapped.RowPitch, copy_width,
                    copy_height);
  libyuv::CopyPlane(nv12 + static_cast<size_t>(stride) * frame_height, stride,
                    luma + static_cast<size_t>(mapped.RowPitch) * height,
                    mapped.RowPitch, copy_width, copy_height / 2);
  d3d.context->Unmap(d3d.upload.Get(), 0);
  d3d.context->CopySubresourceRegion(target, 0, 0, 0, 0, d3d.upload.Get(), 0,
                                     nullptr);
  return WEBRTC_VIDEO_CODEC_OK;
}
#endif

int32_t NvidiaH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoder_) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!encoded_image_callback_) {
    RTC_LOG(LS_WARNING)
        << "InitEncode() has been called, but a callback function "
           "has not been set with RegisterEncodeCompleteCallback()";
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  const void* nv12_src = nullptr;
  uint32_t nv12_stride = 0;
  std::vector<uint8_t> nv12_tmp;

  auto* vfb = input_frame.video_frame_buffer().get();
  const int w = input_frame.width();
  const int h = input_frame.height();

#if defined(_WIN32)
  // Copied on the GPU below; nothing here reads its pixels. A texture frame
  // that arrives with texture input off is read back like any other buffer.
  const livekit_ffi::D3D11TextureBuffer* texture_frame =
      d3d_ && texture_input_.load(std::memory_order_relaxed)
          ? livekit_ffi::D3D11TextureBuffer::From(vfb)
          : nullptr;
  const bool on_gpu = texture_frame != nullptr;
#else
  const bool on_gpu = false;
#endif

  if (!on_gpu && vfb->type() == VideoFrameBuffer::Type::kNV12) {
    auto nv12_ref = vfb->GetNV12();
    if (nv12_ref) {
      nv12_src = nv12_ref->DataY();
      nv12_stride = nv12_ref->StrideY();
    }
  }

  if (!on_gpu && !nv12_src) {
    auto i420 = vfb->ToI420();
    if (!i420) {
      RTC_LOG(LS_ERROR) << "Failed to convert "
                        << VideoFrameBufferTypeToString(vfb->type())
                        << " to I420 for NV12 encode.";
      return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    }
    // NV12 needs an even row stride: MergeUVPlane writes 2*((w+1)/2) bytes per
    // chroma row, which is w+1 for odd widths. Sizing the buffer at stride w
    // therefore overran the last row by one byte. Use an even stride for both
    // the Y and interleaved-UV planes (== w for even widths, so no change on
    // the common path). EncoderInfo.requested_resolution_alignment = 2 already
    // asks WebRTC for even dimensions; this keeps the fallback allocation safe
    // even if an odd-width frame slips through.
    const int nv12_line = 2 * ((w + 1) / 2);
    nv12_stride = nv12_line;
    int chroma_h = (h + 1) / 2;
    int y_size = nv12_line * h;
    int uv_size = nv12_line * chroma_h;
    nv12_tmp.resize(y_size + uv_size);

    libyuv::CopyPlane(i420->DataY(), i420->StrideY(),
                       nv12_tmp.data(), nv12_line, w, h);
    libyuv::MergeUVPlane(i420->DataU(), i420->StrideU(),
                          i420->DataV(), i420->StrideV(),
                          nv12_tmp.data() + y_size, nv12_line,
                          (w + 1) / 2, chroma_h);
    nv12_src = nv12_tmp.data();
  }

  bool is_keyframe_needed = false;
  if (configuration_.key_frame_request && configuration_.sending) {
    is_keyframe_needed = true;
  }

  bool send_key_frame =
      is_keyframe_needed ||
      (frame_types && (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  if (send_key_frame) {
    is_keyframe_needed = true;
    configuration_.key_frame_request = false;
  }

  if (!configuration_.sending) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  if (frame_types != nullptr) {
    if ((*frame_types)[0] == VideoFrameType::kEmptyFrame) {
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }
  }

  livekit::NvencActiveEncode active_encode;
  // Set when this frame's metadata is queued, with NVENC's submit count at
  // that point, so the catch below knows whether NVENC ever took the frame.
  bool meta_queued = false;
  int32_t submitted_before = 0;
  try {
    const NvEncInputFrame* nv_enc_input_frame = encoder_->GetNextInputFrame();

    const auto t_copy = std::chrono::steady_clock::now();
#if defined(_WIN32)
    if (d3d_) {
      auto* target =
          static_cast<ID3D11Texture2D*>(nv_enc_input_frame->inputPtr);
      const int32_t copied =
          texture_frame
              ? CopyTexture(*texture_frame, target)
              : UploadFrame(static_cast<const uint8_t*>(nv12_src), nv12_stride,
                            w, h, target);
      if (copied != WEBRTC_VIDEO_CODEC_OK) {
        return copied;
      }
    } else
#endif
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      NvEncoderCuda::CopyToDeviceFrame(
          cu_context_, (void*)nv12_src, nv12_stride,
          reinterpret_cast<CUdeviceptr>(nv_enc_input_frame->inputPtr),
          nv_enc_input_frame->pitch, w, h,
          CU_MEMORYTYPE_HOST, nv_enc_input_frame->bufferFormat,
          nv_enc_input_frame->chromaOffsets, nv_enc_input_frame->numChromaPlanes);
    }
    livekit::nvenc_timing().copy_us.fetch_add(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_copy)
            .count(),
        std::memory_order_relaxed);
    livekit::nvenc_timing().frames.fetch_add(1, std::memory_order_relaxed);

    NV_ENC_PIC_PARAMS pic_params = NV_ENC_PIC_PARAMS();
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = 0;
    if (is_keyframe_needed) {
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA |
                                  NV_ENC_PIC_FLAG_FORCEIDR |
                                  NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
      configuration_.key_frame_request = false;
    }

    PendingFrame pending;
    pending.rtp_timestamp = input_frame.rtp_timestamp();
    pending.ntp_time_ms = input_frame.ntp_time_ms();
    pending.render_time_ms = input_frame.render_time_ms();
    pending.rotation = input_frame.rotation();
    pending.color_space = input_frame.color_space();
    pending.submit_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    pending.frame_us = input_frame.timestamp_us();
    pending_frames_.push_back(pending);
    livekit::nvenc_note_pending_depth(pending_frames_.size());
    meta_queued = true;
    submitted_before = encoder_->GetSubmittedFrameCount();
    // Hard invariant. In steady state this holds exactly `output_delay_`
    // entries. Metadata cannot be dropped independently: NVENC still owns the
    // corresponding encoded frame, so popping only this FIFO would attach its
    // packet to the next frame's RTP timestamp and shift every later frame.
    // Recreate the encoder instead; Release drains/discards its outstanding
    // packets and the replacement starts with a keyframe and an empty FIFO.
    if (pending_frames_.size() > static_cast<size_t>(output_delay_) + 4) {
      RTC_LOG(LS_ERROR) << "NvEncoder pending metadata invariant violated";
      return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    }

    std::vector<std::vector<uint8_t>> bit_stream;
    encoder_->EncodeFrame(bit_stream, &pic_params);
    const uint64_t bitstream_wait_us = encoder_->GetLastBitstreamWaitUs();

    const int64_t out_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    const int64_t out_rtc_us = webrtc::TimeMicros();
    for (std::vector<uint8_t>& packet : bit_stream) {
      if (pending_frames_.empty()) {
        RTC_LOG(LS_WARNING) << "NvEncoder returned more packets than frames";
        break;
      }
      const PendingFrame meta = pending_frames_.front();
      pending_frames_.pop_front();
      livekit::nvenc_note_pending_depth(pending_frames_.size());
      if (out_us > meta.submit_us) {
        livekit::nvenc_note_latency(
            static_cast<uint64_t>(out_us - meta.submit_us));
      }
      if (meta.frame_us > 0 && out_rtc_us > meta.frame_us) {
        livekit::nvenc_note_frame_age(
            static_cast<uint64_t>(out_rtc_us - meta.frame_us));
      }
      int32_t result = ProcessEncodedFrame(packet, meta);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
      // In steady state one EncodeFrame call returns one picture at any
      // output delay (with a delay, an earlier frame's). Classify after
      // parsing the NALs: periodic GOP IDRs are generated by NVENC and are
      // not visible in the input frame flags.
      livekit::nvenc_note_frame_type_wait(
          bitstream_wait_us,
          encoded_image_._frameType == VideoFrameType::kVideoFrameKey);
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed EncodeFrame NvEncoder " << e.what();
    // If MapResources or nvEncEncodePicture threw, no packet will ever come
    // back for this frame, and leaving its entry queued pairs every later
    // packet with the previous frame's metadata (after a few throws it also
    // trips the invariant above on every call). If NVENC accepted the frame
    // and only the bitstream read failed, that packet is still owed: keep it.
    if (meta_queued && !pending_frames_.empty() &&
        encoder_->GetSubmittedFrameCount() == submitted_before) {
      pending_frames_.pop_back();
      livekit::nvenc_note_pending_depth(pending_frames_.size());
    }
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::ProcessEncodedFrame(
    std::vector<uint8_t>& packet,
    const PendingFrame& meta) {
  encoded_image_._encodedWidth = encoder_->GetEncodeWidth();
  encoded_image_._encodedHeight = encoder_->GetEncodeHeight();
  encoded_image_.SetRtpTimestamp(meta.rtp_timestamp);
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = meta.ntp_time_ms;
  encoded_image_.capture_time_ms_ = meta.render_time_ms;
  encoded_image_.rotation_ = meta.rotation;
  // Tag RTP content type from the codec_ mode the caller configured us with.
  // Hardcoding SCREENSHARE here mislabels camera streams, which feeds
  // wrong signals into SFU bandwidth estimation, any network path that
  // inspects content-type hints, and downstream recording / analytics.
  encoded_image_.content_type_ = (codec_.mode == VideoCodecMode::kScreensharing)
                                     ? VideoContentType::SCREENSHARE
                                     : VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
  encoded_image_.SetColorSpace(meta.color_space);
  std::vector<H264::NaluIndex> naluIndices =
      H264::FindNaluIndices(MakeArrayView(packet.data(), packet.size()));
  for (uint32_t i = 0; i < naluIndices.size(); i++) {
    const H264::NaluType naluType =
        H264::ParseNaluType(packet[naluIndices[i].payload_start_offset]);
    if (naluType == H264::kIdr) {
      encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
      break;
    }
  }

  encoded_image_.SetEncodedData(
      EncodedImageBuffer::Create(packet.data(), packet.size()));
  encoded_image_.set_size(packet.size());

  h264_bitstream_parser_.ParseBitstream(encoded_image_);
  encoded_image_.qp_ = h264_bitstream_parser_.GetLastSliceQp().value_or(-1);

  CodecSpecificInfo codecInfo;
  codecInfo.codecType = kVideoCodecH264;
  codecInfo.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;

  const auto result =
      encoded_image_callback_->OnEncodedImage(encoded_image_, &codecInfo);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "Encode m_encodedCompleteCallback failed "
                      << result.error;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo NvidiaH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  // While true WebRTC passes D3D11TextureBuffer frames through untouched;
  // otherwise it reads them back to system memory first.
  info.supports_native_handle =
      texture_input_.load(std::memory_order_relaxed);
  info.implementation_name = "NVIDIA H264 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  // Ask WebRTC to hand us even width/height. The I420->NV12 conversion (and
  // NVENC itself) assumes even chroma dimensions; odd widths otherwise overran
  // the temp NV12 buffer during MergeUVPlane (see Encode()).
  info.requested_resolution_alignment = 2;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNV12, VideoFrameBuffer::Type::kI420};
  return info;
}

void NvidiaH264EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  if (!encoder_) {
    RTC_LOG(LS_WARNING) << "SetRates() while uninitialized.";
    return;
  }

  // isfinite too, not just the range check: NaN compares false against
  // everything, so it sails past `< 1.0` and the cast below is UB. Ported
  // from upstream #1297, which also confirmed our bps->kbps maxBitrate fix.
  if (!std::isfinite(parameters.framerate_fps) ||
      parameters.framerate_fps < 1.0 ||
      parameters.framerate_fps >
          static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  if (parameters.bitrate.get_sum_bps() == 0) {
    configuration_.SetStreamState(false);
    return;
  }

  uint32_t new_target_bps = parameters.bitrate.GetSpatialLayerSum(0);
  uint32_t new_framerate = static_cast<uint32_t>(parameters.framerate_fps);

  // BWE feeds SetRates several times per second and normally changes by tiny
  // amounts each time. Every NVENC Reconfigure re-primes rate control and can
  // visibly hitch the stream. Accumulate small movements and only apply once
  // the target differs by at least 5% (or 100 kbps); large congestion drops
  // still take effect immediately.
  const uint32_t old_target_bps = configuration_.target_bps;
  const uint32_t bitrate_delta =
      new_target_bps > old_target_bps
          ? new_target_bps - old_target_bps
          : old_target_bps - new_target_bps;
  const uint32_t bitrate_threshold =
      std::max<uint32_t>(100000u, old_target_bps / 20u);
  const bool framerate_changed =
      new_framerate != static_cast<uint32_t>(configuration_.max_frame_rate);
  if (bitrate_delta < bitrate_threshold && !framerate_changed) {
    configuration_.SetStreamState(new_target_bps > 0);
    return;
  }

  const auto previous_config = nv_encode_config_;
  const auto previous_initialize = nv_initialize_params_;
  nv_encode_config_.rcParams.averageBitRate = new_target_bps;
  // Inert in CBR mode (NVENC ignores maxBitRate); kept for VBR parity.
  nv_encode_config_.rcParams.maxBitRate =
      new_target_bps + new_target_bps / 4;
  nv_initialize_params_.frameRateNum = new_framerate;
  nv_initialize_params_.frameRateDen = 1;
  nv_encode_config_.rcParams.vbvBufferSize = FrameSizedVbvBuffer(
      new_target_bps, nv_initialize_params_.frameRateNum,
      nv_initialize_params_.frameRateDen);
  nv_encode_config_.rcParams.vbvInitialDelay =
      nv_encode_config_.rcParams.vbvBufferSize;

  // Keep the screenshare GOP at the selected wall-clock duration at the
  // CURRENT framerate. gopLength is
  // frame-based, so a GOP sized at the initial fps drifts to many seconds of
  // wall clock once the framerate drops (a 120-frame GOP is ~18 s at the
  // 6.7 fps idle keep-alive rate), quietly unbounding loss recovery again.
  // This only runs on hysteresis-approved reconfigures, so it adds no extra
  // rate-control churn.
  if (codec_.mode == VideoCodecMode::kScreensharing) {
    const uint32_t gop_seconds = screen_profile_ == 3 ? 10u : 2u;
    const uint32_t idr_period =
        std::max<uint32_t>(1u, new_framerate * gop_seconds);
    nv_encode_config_.gopLength = idr_period;
    nv_encode_config_.encodeCodecConfig.h264Config.idrPeriod = idr_period;
  }

  NV_ENC_RECONFIGURE_PARAMS reconfigure_params = {};
  reconfigure_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  reconfigure_params.reInitEncodeParams = nv_initialize_params_;

  try {
    encoder_->Reconfigure(&reconfigure_params);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "NVENC reconfigure failed: " << e.what();
    nv_encode_config_ = previous_config;
    nv_initialize_params_ = previous_initialize;
    return;
  }

  // Upstream #1297: only record rates the encoder accepted. Preserve Rift's
  // hysteresis, frame-sized VBV, wall-clock GOP and texture input settings.
  codec_.maxFramerate = new_framerate;
  codec_.maxBitrate = new_target_bps / 1000;
  configuration_.target_bps = new_target_bps;
  configuration_.max_frame_rate = parameters.framerate_fps;

  if (configuration_.target_bps) {
    configuration_.SetStreamState(true);
  } else {
    configuration_.SetStreamState(false);
  }
}

void NvidiaH264EncoderImpl::LayerConfig::SetStreamState(bool send_stream) {
  if (send_stream && !sending) {
    // Need a key frame if we have not sent this stream before.
    key_frame_request = true;
  }
  sending = send_stream;
}

}  // namespace webrtc
