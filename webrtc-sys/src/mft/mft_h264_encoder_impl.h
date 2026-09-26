#ifndef WEBRTC_MFT_H264_ENCODER_IMPL_H_
#define WEBRTC_MFT_H264_ENCODER_IMPL_H_

#include <wrl/client.h>

struct IMFTransform;
struct IMFMediaEventGenerator;
struct IMFMediaEvent;
struct IMFMediaType;
struct IMFSample;
struct ID3D11Texture2D;
struct ICodecAPI;

namespace livekit_ffi {
class D3D11TextureBuffer;
}

#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "mft_progress_watchdog.h"

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
  // Owns the callback lifetime fence. Stop() quiesces callbacks before any
  // encoder state is released. Only the staff event-driven arm creates it.
  class EventPump;
  Microsoft::WRL::ComPtr<EventPump> event_pump_;
  std::atomic<bool> event_key_frame_request_{false};
  int32_t event_result_ = 0;
  bool HandleMftEvent(IMFMediaEvent* event);
  // `adapter_luid` 0 enumerates every hardware MFT the classic way;
  // otherwise only the MFTs on that adapter (MFTEnum2), for texture input.
  bool CreateMftEncoder(UINT32 candidate_index, UINT32* candidate_count,
                        uint64_t adapter_luid);
  // A non-zero `texture_adapter` is a texture-input attempt: the candidate
  // counts only if texture input actually comes on.
  int32_t InitEncodeCandidate(const VideoCodec* inst, UINT32 candidate_index,
                              UINT32* candidate_count,
                              uint64_t texture_adapter);
  bool ConfigureCodecBeforeMediaType();
  bool ReadBackRateControl();
  long CreateInputType(IMFMediaType** type);
  bool ConfigureInputType();

  // Texture input (see d3d_input_requested in nvenc_timing.h). The MFT gets
  // its own device on its own adapter; frames arrive as keyed-mutex NV12
  // textures from the capturer and are copied GPU to GPU into samples from a
  // D3D11 sample allocator. CPU frames that still arrive are uploaded into
  // the same samples, since an MFT holding a D3D manager may not take
  // system memory.
  struct D3DInput;
  std::unique_ptr<D3DInput> d3d_;
  // The active candidate's adapter, 0 if unknown. MFTEnumEx activates often
  // carry no MFT_ENUM_ADAPTER_LUID, so only a filtered enumeration knows it.
  uint64_t adapter_luid_ = 0;
  // The MFT accepted our D3D manager during this candidate's init.
  bool d3d_attempted_ = false;
  // Published copy of "texture frames are copied in" for GetEncoderInfo,
  // which must not race the event pump withdrawing texture input.
  std::atomic<bool> texture_input_on_{false};
  bool EnableD3DInput();
  int32_t AllocateTextureSample(Microsoft::WRL::ComPtr<IMFSample>* sample,
                                Microsoft::WRL::ComPtr<ID3D11Texture2D>* texture,
                                UINT* subresource);
  int32_t TextureSample(const livekit_ffi::D3D11TextureBuffer& frame,
                        Microsoft::WRL::ComPtr<IMFSample>* sample);
  int32_t UploadSample(const NV12BufferInterface* nv12,
                       const I420BufferInterface* i420,
                       Microsoft::WRL::ComPtr<IMFSample>* sample);
  int32_t TextureInputFailed(const char* operation, long hr);
  void WithdrawTextureInput();
  bool ConfigureOutputType();
  bool StartStreaming();
  // Drains every event the async MFT has queued, converting them into
  // input/output credits. Returns false on a fatal event or GetEvent failure.
  bool PumpMftEvents();
  int32_t DrainReadyOutput();
  int32_t FinishEncodeAttempt();
  int32_t RuntimeFailure(const char* operation, HRESULT hr);
  // Sync MFTs pass the frame whose input produced the output; the async model
  // passes nullptr and stamps from `pending_meta_` instead, matched by sample
  // time. `single_shot` = consume exactly one HaveOutput credit.
  int32_t ProcessEncodedOutput(const VideoFrame* input_frame,
                               bool single_shot, uint64_t submitted_us = 0);

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
    uint64_t submitted_us = 0;
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
  // mft_now_us() of the last keyframe output (or periodic keyframe request),
  // for the screen-share IDR bound in Encode(). The event-driven arm writes
  // it from the pump callback, so like the state above it is only touched
  // under the pump's lock.
  uint64_t last_key_frame_us_ = 0;
  bool mf_started_ = false;
  bool bitrate_failure_logged_ = false;
  bool runtime_failed_ = false;
  MftProgressWatchdog progress_;
  std::string encoder_name_ = "Windows MFT H264 Encoder";
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
