#include "mft_h264_encoder_impl.h"

#include <algorithm>
#include <limits>
#include <string>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

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

MftH264EncoderImpl::MftH264EncoderImpl(const Environment& env) : env_(env) {}

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

bool MftH264EncoderImpl::CreateMftEncoder() {
  MFT_REGISTER_TYPE_INFO input_type = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_type = {MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  HRESULT hr =
      MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                &input_type, &output_type, &activates, &count);
  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_WARNING) << "No hardware H.264 MFT found, trying software.";
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &input_type, &output_type, &activates, &count);
  }

  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_ERROR) << "No H.264 MFT encoder available.";
    return false;
  }

  hr = activates[0]->ActivateObject(IID_PPV_ARGS(transform_.GetAddressOf()));
  for (UINT32 i = 0; i < count; i++)
    activates[i]->Release();
  CoTaskMemFree(activates);

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to activate MFT encoder: 0x" << std::hex
                      << hr;
    return false;
  }

  transform_->QueryInterface(IID_PPV_ARGS(&codec_api_));
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
    return false;
  }
  return true;
}

bool MftH264EncoderImpl::ConfigureInputType() {
  Microsoft::WRL::ComPtr<IMFMediaType> in_type;
  HRESULT hr = MFCreateMediaType(in_type.GetAddressOf());
  if (FAILED(hr)) return false;

  in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, max_framerate_, 1);
  in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr = transform_->SetInputType(input_stream_id_, in_type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT SetInputType failed: 0x" << std::hex << hr;
    return false;
  }
  return true;
}

bool MftH264EncoderImpl::StartStreaming() {
  if (codec_api_) {
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_BOOL;
    val.boolVal = VARIANT_TRUE;
    codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &val);
  }

  HRESULT hr =
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT begin streaming failed: 0x" << std::hex << hr;
    return false;
  }
  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT start of stream failed: 0x" << std::hex << hr;
    return false;
  }
  return true;
}

int32_t MftH264EncoderImpl::InitEncode(const VideoCodec* inst,
                                        const VideoEncoder::Settings&) {
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

  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFStartup failed: 0x" << std::hex << hr;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  mf_started_ = true;

  if (!CreateMftEncoder())
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

  if (!ConfigureOutputType() || !ConfigureInputType() || !StartStreaming())
    return WEBRTC_VIDEO_CODEC_ERROR;

  RTC_LOG(LS_INFO) << "MFT H264 encoder initialized: " << width_ << "x"
                   << height_ << " @ " << max_framerate_
                   << "fps, target_bps=" << target_bps_;

  SimulcastRateAllocator init_allocator(env_, codec_);
  VideoBitrateAllocation allocation =
      init_allocator.Allocate(VideoBitrateAllocationParameters(
          DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::Release() {
  if (transform_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_.Reset();
  }
  if (codec_api_) {
    codec_api_->Release();
    codec_api_ = nullptr;
  }
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
  sending_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!transform_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!callback_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  auto frame_buffer = input_frame.video_frame_buffer()->ToI420();
  if (!frame_buffer)
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;

  bool force_key = key_frame_request_ ||
                   (frame_types && !frame_types->empty() &&
                    (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  if (force_key)
    key_frame_request_ = false;

  if (!sending_)
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;

  if (frame_types && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kEmptyFrame)
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;

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

  const DWORD nv12_size = width_ * height_ * 3 / 2;
  Microsoft::WRL::ComPtr<IMFMediaBuffer> input_buffer;
  HRESULT hr = MFCreateMemoryBuffer(nv12_size, input_buffer.GetAddressOf());
  if (FAILED(hr))
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;

  BYTE* buffer_data = nullptr;
  hr = input_buffer->Lock(&buffer_data, nullptr, nullptr);
  if (FAILED(hr))
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;

  I420ToNV12(frame_buffer.get(), buffer_data, width_);

  input_buffer->Unlock();
  input_buffer->SetCurrentLength(nv12_size);

  Microsoft::WRL::ComPtr<IMFSample> input_sample;
  hr = MFCreateSample(input_sample.GetAddressOf());
  if (FAILED(hr))
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;

  input_sample->AddBuffer(input_buffer.Get());
  input_sample->SetSampleTime(static_cast<LONGLONG>(input_frame.rtp_timestamp()));
  input_sample->SetSampleDuration(10000000LL / max_framerate_);

  hr = transform_->ProcessInput(input_stream_id_, input_sample.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT ProcessInput failed: 0x" << std::hex << hr;
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return ProcessEncodedOutput(input_frame);
}

int32_t MftH264EncoderImpl::ProcessEncodedOutput(
    const VideoFrame& input_frame) {
  MFT_OUTPUT_STREAM_INFO stream_info = {};
  HRESULT hr =
      transform_->GetOutputStreamInfo(output_stream_id_, &stream_info);
  if (FAILED(hr))
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;

  const bool provides_samples =
      (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

  while (true) {
    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = output_stream_id_;

    Microsoft::WRL::ComPtr<IMFSample> our_sample;
    if (!provides_samples) {
      Microsoft::WRL::ComPtr<IMFMediaBuffer> out_buf;
      DWORD buf_size =
          stream_info.cbSize > 0 ? stream_info.cbSize : width_ * height_ * 2;
      hr = MFCreateMemoryBuffer(buf_size, out_buf.GetAddressOf());
      if (FAILED(hr))
        return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;

      hr = MFCreateSample(our_sample.GetAddressOf());
      if (FAILED(hr))
        return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
      our_sample->AddBuffer(out_buf.Get());
      output_data.pSample = our_sample.Get();
    }

    DWORD status = 0;
    hr = transform_->ProcessOutput(0, 1, &output_data, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
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
      bool captured_header = false;
      DWORD type_index = 0;
      while (true) {
        Microsoft::WRL::ComPtr<IMFMediaType> new_output_type;
        HRESULT gt_hr = transform_->GetOutputAvailableType(
            output_stream_id_, type_index, new_output_type.GetAddressOf());
        if (FAILED(gt_hr))
          break;

        GUID subtype = {};
        if (SUCCEEDED(new_output_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_H264) {
          UINT32 header_size = 0;
          if (SUCCEEDED(new_output_type->GetBlobSize(
                  MF_MT_MPEG_SEQUENCE_HEADER, &header_size)) &&
              header_size > 0) {
            sequence_header_.resize(header_size);
            if (SUCCEEDED(new_output_type->GetBlob(
                    MF_MT_MPEG_SEQUENCE_HEADER, sequence_header_.data(),
                    header_size, nullptr))) {
              captured_header = true;
              RTC_LOG(LS_INFO)
                  << "MFT captured SPS+PPS sequence header (" << header_size
                  << " bytes) for IDR prepend.";
            } else {
              sequence_header_.clear();
            }
          }
          if (FAILED(transform_->SetOutputType(output_stream_id_,
                                                new_output_type.Get(), 0))) {
            RTC_LOG(LS_WARNING) << "MFT SetOutputType (post stream change) "
                                   "failed; falling back to ConfigureOutputType.";
            ConfigureOutputType();
          }
          break;
        }
        type_index++;
      }
      if (!captured_header && sequence_header_.empty()) {
        RTC_LOG(LS_WARNING)
            << "MFT stream change did not yield an SPS+PPS header blob; "
               "decoders may be unable to initialize until a subsequent "
               "stream change delivers one.";
      }
      continue;
    }

    if (FAILED(hr)) {
      if (output_data.pEvents)
        output_data.pEvents->Release();
      break;
    }

    IMFSample* result_sample =
        provides_samples ? output_data.pSample : our_sample.Get();
    if (!result_sample) {
      if (output_data.pEvents)
        output_data.pEvents->Release();
      continue;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> result_buffer;
    hr = result_sample->ConvertToContiguousBuffer(result_buffer.GetAddressOf());
    if (provides_samples && output_data.pSample)
      output_data.pSample->Release();
    if (output_data.pEvents)
      output_data.pEvents->Release();
    if (FAILED(hr))
      continue;

    BYTE* data = nullptr;
    DWORD data_length = 0;
    hr = result_buffer->Lock(&data, nullptr, &data_length);
    if (FAILED(hr) || data_length == 0)
      continue;

    encoded_image_._encodedWidth = width_;
    encoded_image_._encodedHeight = height_;
    encoded_image_.SetRtpTimestamp(input_frame.rtp_timestamp());
    encoded_image_.SetSimulcastIndex(0);
    encoded_image_.ntp_time_ms_ = input_frame.ntp_time_ms();
    encoded_image_.capture_time_ms_ = input_frame.render_time_ms();
    encoded_image_.rotation_ = input_frame.rotation();
    encoded_image_.content_type_ = VideoContentType::UNSPECIFIED;
    encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
    encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
    encoded_image_.SetColorSpace(input_frame.color_space());

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

    auto result = callback_->OnEncodedImage(encoded_image_, &codec_info);
    if (result.error != EncodedImageCallback::Result::OK) {
      RTC_LOG(LS_ERROR) << "MFT encode callback failed: " << result.error;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo MftH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "Windows MFT H264 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kI420};
  return info;
}

void MftH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  if (!transform_) {
    RTC_LOG(LS_WARNING) << "MFT SetRates() while uninitialized.";
    return;
  }

  if (parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  if (parameters.bitrate.get_sum_bps() == 0) {
    sending_ = false;
    return;
  }

  target_bps_ = parameters.bitrate.GetSpatialLayerSum(0);
  max_framerate_ = static_cast<uint32_t>(parameters.framerate_fps);

  if (codec_api_) {
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_UI4;
    val.ulVal = target_bps_;
    codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &val);
  }

  if (target_bps_ > 0) {
    if (!sending_)
      key_frame_request_ = true;
    sending_ = true;
  } else {
    sending_ = false;
  }
}

}  // namespace webrtc
