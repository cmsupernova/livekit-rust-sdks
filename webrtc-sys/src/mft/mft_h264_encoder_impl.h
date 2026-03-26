#ifndef WEBRTC_MFT_H264_ENCODER_IMPL_H_
#define WEBRTC_MFT_H264_ENCODER_IMPL_H_

#include <wrl/client.h>

struct IMFTransform;
struct ICodecAPI;

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/include/video_codec_interface.h"

namespace webrtc {

class MftH264EncoderImpl : public VideoEncoder {
 public:
  explicit MftH264EncoderImpl(const Environment& env);
  ~MftH264EncoderImpl() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const VideoEncoder::Settings& settings) override;

  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;

  int32_t Release() override;

  int32_t Encode(const VideoFrame& input_frame,
                 const std::vector<VideoFrameType>* frame_types) override;

  void SetRates(const RateControlParameters& parameters) override;

  EncoderInfo GetEncoderInfo() const override;

 private:
  bool CreateMftEncoder();
  bool ConfigureInputType();
  bool ConfigureOutputType();
  bool StartStreaming();
  int32_t ProcessEncodedOutput(const VideoFrame& input_frame);

  void I420ToNV12(const I420BufferInterface* i420,
                  uint8_t* nv12_data, int nv12_stride);

  Environment env_;
  VideoCodec codec_;
  EncodedImageCallback* callback_ = nullptr;
  EncodedImage encoded_image_;
  H264BitstreamParser h264_bitstream_parser_;

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  ICodecAPI* codec_api_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t target_bps_ = 0;
  uint32_t max_framerate_ = 30;
  bool sending_ = false;
  bool key_frame_request_ = false;
  bool mf_started_ = false;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
};

}  // namespace webrtc

#endif  // WEBRTC_MFT_H264_ENCODER_IMPL_H_
