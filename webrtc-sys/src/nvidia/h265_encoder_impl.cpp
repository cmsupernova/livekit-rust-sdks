#include "h265_encoder_impl.h"

#include <algorithm>
#include <limits>
#include <string>

#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/video/video_codec_constants.h"
#include "api/video_codecs/scalability_mode.h"
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

namespace webrtc {

// Used by histograms. Values of entries should not be changed.
enum H265EncoderImplEvent {
  kH265EncoderEventInit = 0,
  kH265EncoderEventError = 1,
  kH265EncoderEventMax = 16,
};

NvidiaH265EncoderImpl::NvidiaH265EncoderImpl(
    const webrtc::Environment& env,
    CUcontext context,
    CUmemorytype memory_type,
    NV_ENC_BUFFER_FORMAT nv_format,
    const SdpVideoFormat& format)
    : env_(env),
      encoder_(nullptr),
      cu_context_(context),
      cu_memory_type_(memory_type),
      cu_scaled_array_(nullptr),
      nv_format_(nv_format),
      format_(format) {
  RTC_CHECK_NE(cu_memory_type_, CU_MEMORYTYPE_HOST);
}

NvidiaH265EncoderImpl::~NvidiaH265EncoderImpl() {
  Release();
}

void NvidiaH265EncoderImpl::ReportInit() {
  if (has_reported_init_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H265EncoderImpl.Event",
                            kH265EncoderEventInit, kH265EncoderEventMax);
  has_reported_init_ = true;
}

void NvidiaH265EncoderImpl::ReportError() {
  if (has_reported_error_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H265EncoderImpl.Event",
                            kH265EncoderEventError, kH265EncoderEventMax);
  has_reported_error_ = true;
}

int32_t NvidiaH265EncoderImpl::InitEncode(
    const VideoCodec* inst,
    const VideoEncoder::Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecH265) {
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

  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  configuration_.sending = false;
  configuration_.frame_dropping_on = codec_.GetFrameDropEnabled();
  configuration_.key_frame_interval = 0;

  configuration_.width = codec_.width;
  configuration_.height = codec_.height;

  configuration_.max_frame_rate = codec_.maxFramerate;
  configuration_.target_bps = codec_.startBitrate * 1000;
  configuration_.max_bps = codec_.maxBitrate * 1000;

  const CUresult result = cuCtxSetCurrent(cu_context_);
  if (result != CUDA_SUCCESS) {
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  try {
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      encoder_ = std::make_unique<NvEncoderCuda>(cu_context_, codec_.width,
                                                 codec_.height, nv_format_, 0);
    } else {
      RTC_DCHECK_NOTREACHED();
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed Initialize NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  nv_initialize_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  nv_encode_config_.version = NV_ENC_CONFIG_VER;
  nv_initialize_params_.encodeConfig = &nv_encode_config_;

  GUID encodeGuid = NV_ENC_CODEC_HEVC_GUID;
  GUID presetGuid = NV_ENC_PRESET_P5_GUID;

  encoder_->CreateDefaultEncoderParams(&nv_initialize_params_, encodeGuid,
                                       presetGuid,
                                       NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

  nv_initialize_params_.frameRateNum =
      static_cast<uint32_t>(configuration_.max_frame_rate);
  nv_initialize_params_.frameRateDen = 1;
  nv_initialize_params_.bufferFormat = nv_format_;

  nv_encode_config_.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
  nv_encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
  nv_encode_config_.frameIntervalP = 1;
  // Emit VPS/SPS/PPS on every IDR so subscribers that join between forced
  // keyframes always land on a decodable IDR.
  nv_encode_config_.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
  nv_encode_config_.rcParams.version = NV_ENC_RC_PARAMS_VER;
  nv_encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  nv_encode_config_.rcParams.averageBitRate = configuration_.target_bps;
  // NVENC ignores maxBitRate in CBR mode; inert here, kept for VBR parity.
  nv_encode_config_.rcParams.maxBitRate =
      configuration_.target_bps + configuration_.target_bps / 4;
  nv_encode_config_.rcParams.vbvBufferSize = configuration_.target_bps;
  nv_encode_config_.rcParams.vbvInitialDelay =
      nv_encode_config_.rcParams.vbvBufferSize * 9 / 10;
  // Two-pass (quarter-res first pass) tightens low-latency CBR adherence and
  // reduces per-frame overshoot at negligible cost on NVENC-class GPUs.
  nv_encode_config_.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
  // Bound IDR size relative to the per-frame P budget to flatten keyframe
  // spikes. Tuning knob - worth an on-hardware A/B.
  nv_encode_config_.rcParams.lowDelayKeyFrameScale = 2;

  try {
    encoder_->CreateEncoder(&nv_initialize_params_);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed Initialize NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  RTC_LOG(LS_INFO) << "NVIDIA H265/HEVC NVENC initialized: "
                   << codec_.width << "x" << codec_.height
                   << " @ " << codec_.maxFramerate << "fps, target_bps="
                   << configuration_.target_bps;

  SimulcastRateAllocator init_allocator(env_, codec_);
  VideoBitrateAllocation allocation =
      init_allocator.Allocate(VideoBitrateAllocationParameters(
          DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH265EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH265EncoderImpl::Release() {
  if (encoder_) {
    encoder_->DestroyEncoder();
    encoder_ = nullptr;
  }
  if (cu_scaled_array_) {
    cuArrayDestroy(cu_scaled_array_);
    cu_scaled_array_ = nullptr;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH265EncoderImpl::Encode(
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

  if (vfb->type() == VideoFrameBuffer::Type::kNV12) {
    auto nv12_ref = vfb->GetNV12();
    if (nv12_ref) {
      nv12_src = nv12_ref->DataY();
      nv12_stride = nv12_ref->StrideY();
    }
  }

  if (!nv12_src) {
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

  try {
    const NvEncInputFrame* nv_enc_input_frame = encoder_->GetNextInputFrame();

    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      NvEncoderCuda::CopyToDeviceFrame(
          cu_context_, (void*)nv12_src, nv12_stride,
          reinterpret_cast<CUdeviceptr>(nv_enc_input_frame->inputPtr),
          nv_enc_input_frame->pitch, w, h,
          CU_MEMORYTYPE_HOST, nv_enc_input_frame->bufferFormat,
          nv_enc_input_frame->chromaOffsets, nv_enc_input_frame->numChromaPlanes);
    }

    NV_ENC_PIC_PARAMS pic_params = NV_ENC_PIC_PARAMS();
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = 0;
    if (is_keyframe_needed) {
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA |
                                  NV_ENC_PIC_FLAG_FORCEIDR |
                                  NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
      configuration_.key_frame_request = false;
    }

    current_encoding_is_keyframe_ = is_keyframe_needed;

    std::vector<std::vector<uint8_t>> bit_stream;
    encoder_->EncodeFrame(bit_stream, &pic_params);

    for (std::vector<uint8_t>& packet : bit_stream) {
      int32_t result = ProcessEncodedFrame(packet, input_frame);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }
    current_encoding_is_keyframe_ = false;
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed EncodeFrame NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH265EncoderImpl::ProcessEncodedFrame(
    std::vector<uint8_t>& packet,
    const ::webrtc::VideoFrame& inputFrame) {
  encoded_image_._encodedWidth = encoder_->GetEncodeWidth();
  encoded_image_._encodedHeight = encoder_->GetEncodeHeight();
  encoded_image_.SetRtpTimestamp(inputFrame.rtp_timestamp());
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = inputFrame.ntp_time_ms();
  encoded_image_.capture_time_ms_ = inputFrame.render_time_ms();
  encoded_image_.rotation_ = inputFrame.rotation();
  // Tag RTP content type from the codec_ mode the caller configured us with
  // (see nvidia/h264_encoder_impl.cpp for rationale — mislabeling camera
  // streams as screenshare confuses SFU bandwidth estimation and downstream
  // tooling).
  encoded_image_.content_type_ = (codec_.mode == VideoCodecMode::kScreensharing)
                                     ? VideoContentType::SCREENSHARE
                                     : VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType =
      current_encoding_is_keyframe_ ? VideoFrameType::kVideoFrameKey
                                    : VideoFrameType::kVideoFrameDelta;
  encoded_image_.SetColorSpace(inputFrame.color_space());

  encoded_image_.SetEncodedData(
      EncodedImageBuffer::Create(packet.data(), packet.size()));
  encoded_image_.set_size(packet.size());

  encoded_image_.qp_ = -1;

  CodecSpecificInfo codecInfo;
  codecInfo.codecType = kVideoCodecH265;

  const auto result =
      encoded_image_callback_->OnEncodedImage(encoded_image_, &codecInfo);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "Encode m_encodedCompleteCallback failed "
                      << result.error;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo NvidiaH265EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "NVIDIA H265 Encoder";
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

void NvidiaH265EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  if (!encoder_) {
    RTC_LOG(LS_WARNING) << "SetRates() while uninitialized.";
    return;
  }

  if (parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  if (parameters.bitrate.get_sum_bps() == 0) {
    configuration_.SetStreamState(false);
    return;
  }

  uint32_t new_target_bps = parameters.bitrate.GetSpatialLayerSum(0);
  uint32_t new_framerate = static_cast<uint32_t>(parameters.framerate_fps);

  // BWE feeds SetRates several times per second, very often with values
  // identical to the previous call. Each Reconfigure re-primes NVENC's rate
  // control, so skip it when nothing we hand NVENC actually changed.
  if (new_target_bps == configuration_.target_bps &&
      new_framerate == static_cast<uint32_t>(configuration_.max_frame_rate)) {
    configuration_.SetStreamState(new_target_bps > 0);
    return;
  }

  codec_.maxFramerate = new_framerate;
  codec_.maxBitrate = new_target_bps;
  configuration_.target_bps = new_target_bps;
  configuration_.max_frame_rate = parameters.framerate_fps;

  nv_encode_config_.rcParams.averageBitRate = new_target_bps;
  // Inert in CBR mode (NVENC ignores maxBitRate); kept for VBR parity.
  nv_encode_config_.rcParams.maxBitRate =
      new_target_bps + new_target_bps / 4;
  nv_encode_config_.rcParams.vbvBufferSize = new_target_bps;
  nv_encode_config_.rcParams.vbvInitialDelay =
      nv_encode_config_.rcParams.vbvBufferSize * 9 / 10;
  nv_initialize_params_.frameRateNum = new_framerate;
  nv_initialize_params_.frameRateDen = 1;

  NV_ENC_RECONFIGURE_PARAMS reconfigure_params = {};
  reconfigure_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  reconfigure_params.reInitEncodeParams = nv_initialize_params_;

  try {
    encoder_->Reconfigure(&reconfigure_params);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "NVENC H265 reconfigure failed: " << e.what();
  }

  if (configuration_.target_bps) {
    configuration_.SetStreamState(true);
  } else {
    configuration_.SetStreamState(false);
  }
}

void NvidiaH265EncoderImpl::LayerConfig::SetStreamState(bool send_stream) {
  if (send_stream && !sending) {
    key_frame_request = true;
  }
  sending = send_stream;
}

}  // namespace webrtc


