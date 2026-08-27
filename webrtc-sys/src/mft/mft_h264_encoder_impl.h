#ifndef WEBRTC_MFT_H264_ENCODER_IMPL_H_
#define WEBRTC_MFT_H264_ENCODER_IMPL_H_

#include <wrl/client.h>

struct IMFTransform;
struct IMFMediaEventGenerator;
struct ICodecAPI;

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
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
  // Drains every event the async MFT has queued, converting them into
  // input/output credits. Returns false on a fatal event or GetEvent failure.
  bool PumpMftEvents();
  // Sync MFTs pass the frame whose input produced the output; the async model
  // passes nullptr and stamps from `pending_meta_` instead, matched by sample
  // time. `single_shot` = consume exactly one HaveOutput credit.
  int32_t ProcessEncodedOutput(const VideoFrame* input_frame,
                               bool single_shot);

  void I420ToNV12(const I420BufferInterface* i420,
                  uint8_t* nv12_data, int nv12_stride);

  Environment env_;
  VideoCodec codec_;
  EncodedImageCallback* callback_ = nullptr;
  EncodedImage encoded_image_;
  H264BitstreamParser h264_bitstream_parser_;

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_gen_;
  ICodecAPI* codec_api_ = nullptr;

  // Hardware MFTs are asynchronous: they boot locked, must be unlocked via
  // MF_TRANSFORM_ASYNC_UNLOCK, and are then driven by METransformNeedInput /
  // METransformHaveOutput events rather than blind ProcessInput/ProcessOutput
  // calls. `input_credits_`/`output_credits_` count events received but not
  // yet acted on.
  bool is_async_ = false;
  int input_credits_ = 0;
  int output_credits_ = 0;

  // Metadata of frames handed to an async MFT but not yet returned. With a
  // pipelining encoder the output EncodedImage belongs to an EARLIER input
  // than the one just submitted; stamping it from the current frame corrupts
  // RTP timing and A/V sync silently. Matched by MF sample time, which the
  // transform is required to carry through.
  struct FrameMeta {
    int64_t sample_time_100ns = 0;
    uint32_t rtp_timestamp = 0;
    int64_t ntp_time_ms = 0;
    int64_t render_time_ms = 0;
    VideoRotation rotation = kVideoRotation_0;
    std::optional<ColorSpace> color_space;
  };
  std::deque<FrameMeta> pending_meta_;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t target_bps_ = 0;
  uint32_t max_framerate_ = 30;
  bool sending_ = false;
  bool key_frame_request_ = false;
  bool mf_started_ = false;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;

  // Cached SPS+PPS byte-stream captured from MF_MT_MPEG_SEQUENCE_HEADER on
  // the first MF_E_TRANSFORM_STREAM_CHANGE event. Prepended to every IDR
  // frame whose bitstream doesn't already carry parameter sets inline, so
  // receivers can actually initialize their H264 decoders (without this,
  // the entire stream is undecodable and shows as a black frame).
  std::vector<uint8_t> sequence_header_;
};

}  // namespace webrtc

#endif  // WEBRTC_MFT_H264_ENCODER_IMPL_H_
