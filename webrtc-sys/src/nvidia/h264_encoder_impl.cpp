#include "h264_encoder_impl.h"


#include <algorithm>
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

namespace webrtc {

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


NvidiaH264EncoderImpl::NvidiaH264EncoderImpl(
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
      packetization_mode_(
          H264EncoderSettings::Parse(format).packetization_mode),
      format_(format) {
  std::string hexString = format_.parameters.at("profile-level-id");
  std::optional<webrtc::H264ProfileLevelId> profile_level_id =
      webrtc::ParseH264ProfileLevelId(hexString.c_str());
  if (profile_level_id.has_value()) {
    profile_ = profile_level_id->profile;
    level_ = profile_level_id->level;
  }

  nv_enc_level_ = NV_ENC_LEVEL_AUTOSELECT;
  if (level_ != H264Level::kLevel1_b) {
    // Convert H264Level to NV_ENC_LEVEL.
    nv_enc_level_ = webrtc::H264LevelToNvEncLevel(level_);
  }

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

  // Some NVIDIA GPUs have a limited Encode Session count.
  // We can't get the Session count, so catching NvEncThrow to avoid the crash.
  // refer:
  // https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new
  try {
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      encoder_ = std::make_unique<NvEncoderCuda>(cu_context_, codec_.width,
                                                 codec_.height, nv_format_, 0);
    } else {
      RTC_DCHECK_NOTREACHED();
    }
  } catch (const NVENCException& e) {
    // todo: If Encoder initialization fails, need to notify for Managed side.
    RTC_LOG(LS_ERROR) << "Failed Initialize NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  nv_initialize_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  nv_encode_config_.version = NV_ENC_CONFIG_VER;
  nv_initialize_params_.encodeConfig = &nv_encode_config_;

  GUID encodeGuid = NV_ENC_CODEC_H264_GUID;
  GUID presetGuid = NV_ENC_PRESET_P4_GUID;

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
  nv_encode_config_.rcParams.version = NV_ENC_RC_PARAMS_VER;
  nv_encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  nv_encode_config_.rcParams.averageBitRate = configuration_.target_bps;
  nv_encode_config_.rcParams.maxBitRate =
      configuration_.target_bps + configuration_.target_bps / 4;
  nv_encode_config_.rcParams.vbvBufferSize = configuration_.target_bps;
  nv_encode_config_.rcParams.vbvInitialDelay =
      nv_encode_config_.rcParams.vbvBufferSize * 9 / 10;

  // --- Adaptive quantisation -------------------------------------------------
  // Spatial AQ: per-macroblock QP adjustment based on spatial complexity —
  // more bits for flat/detailed regions (skin, text, UI), fewer for noisy
  // regions where the extra bits would be wasted. ~0% perf cost on Turing+
  // NVENC, visible quality improvement (≈5-10% QP reduction in detail areas).
  // Strength 8/15 is NVIDIA's recommended balance for mixed content.
  //
  // Temporal AQ is intentionally NOT enabled here: it requires
  // `numRefFrames >= 2` and `enablePTD = 1`, neither of which is configured
  // on this realtime path, and it is explicitly unsupported with
  // `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`. On some driver versions the
  // encoder accepts init with `enableTemporalAQ = 1` anyway and then hard-
  // crashes (no stack trace, the host process just disappears) on the first
  // encode call. Leave it off until we have a full CAPS-query + ref-buffer
  // reconfig path.
  nv_encode_config_.rcParams.enableAQ = 1;
  nv_encode_config_.rcParams.aqStrength = 8;

  // --- Keyframe interval (screenshare) --------------------------------------
  // WebRTC's default is infinite GOP + PLI-on-request — bitrate-efficient
  // but a new subscriber stares at green frames for ~1 RTT while the PLI
  // round-trips. For screenshare we'd rather "waste" ~5-10% of the bitrate
  // budget on a periodic IDR (once per second at target framerate) so that:
  //   * new viewers see the screen within ≤1 second of joining
  //   * packet-loss recovery doesn't need a PLI round-trip
  //   * scrub/seek in saved recordings lands on a keyframe quickly
  // Realtime camera streams keep infinite GOP (no perceptual upside to
  // periodic IDRs there; the bitrate saving matters more).
  if (codec_.mode == VideoCodecMode::kScreensharing) {
    const uint32_t idr_period =
        std::max<uint32_t>(1u, configuration_.max_frame_rate);
    nv_encode_config_.gopLength = idr_period;
    nv_encode_config_.encodeCodecConfig.h264Config.idrPeriod = idr_period;
  }

  try {
    encoder_->CreateEncoder(&nv_initialize_params_);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed Initialize NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  RTC_LOG(LS_INFO) << "NVIDIA H264 NVENC initialized: "
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

int32_t NvidiaH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::Release() {
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
    nv12_stride = w;
    int y_size = w * h;
    int chroma_h = (h + 1) / 2;
    int uv_size = w * chroma_h;
    nv12_tmp.resize(y_size + uv_size);

    libyuv::CopyPlane(i420->DataY(), i420->StrideY(),
                       nv12_tmp.data(), w, w, h);
    libyuv::MergeUVPlane(i420->DataU(), i420->StrideU(),
                          i420->DataV(), i420->StrideV(),
                          nv12_tmp.data() + y_size, w,
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

    std::vector<std::vector<uint8_t>> bit_stream;
    encoder_->EncodeFrame(bit_stream, &pic_params);

    for (std::vector<uint8_t>& packet : bit_stream) {
      int32_t result = ProcessEncodedFrame(packet, input_frame);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed EncodeFrame NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::ProcessEncodedFrame(
    std::vector<uint8_t>& packet,
    const ::webrtc::VideoFrame& inputFrame) {
  encoded_image_._encodedWidth = encoder_->GetEncodeWidth();
  encoded_image_._encodedHeight = encoder_->GetEncodeHeight();
  encoded_image_.SetRtpTimestamp(inputFrame.rtp_timestamp());
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = inputFrame.ntp_time_ms();
  encoded_image_.capture_time_ms_ = inputFrame.render_time_ms();
  encoded_image_.rotation_ = inputFrame.rotation();
  // Tag RTP content type from the codec_ mode the caller configured us with.
  // Hardcoding SCREENSHARE here mislabels camera streams, which feeds
  // wrong signals into SFU bandwidth estimation, any network path that
  // inspects content-type hints, and downstream recording / analytics.
  encoded_image_.content_type_ = (codec_.mode == VideoCodecMode::kScreensharing)
                                     ? VideoContentType::SCREENSHARE
                                     : VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
  encoded_image_.SetColorSpace(inputFrame.color_space());
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
  info.supports_native_handle = false;
  info.implementation_name = "NVIDIA H264 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNV12, VideoFrameBuffer::Type::kI420};
  return info;
}

void NvidiaH264EncoderImpl::SetRates(
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

  codec_.maxFramerate = new_framerate;
  codec_.maxBitrate = new_target_bps;
  configuration_.target_bps = new_target_bps;
  configuration_.max_frame_rate = parameters.framerate_fps;

  nv_encode_config_.rcParams.averageBitRate = new_target_bps;
  nv_encode_config_.rcParams.maxBitRate =
      new_target_bps + new_target_bps / 4;  // 125% headroom for transients
  nv_encode_config_.rcParams.vbvBufferSize = new_target_bps;  // 1 second of buffering
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
    RTC_LOG(LS_ERROR) << "NVENC reconfigure failed: " << e.what();
  }

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
