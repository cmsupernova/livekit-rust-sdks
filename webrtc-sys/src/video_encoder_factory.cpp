/*
 * Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/video_encoder_factory.h"

#include "api/environment/environment_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "livekit/objc_video_factory.h"
#include "livekit/nvenc_timing.h"
#include "media/base/media_constants.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "rtc_base/logging.h"
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#endif
#if defined(WEBRTC_USE_H264)
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#endif
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"

#ifdef WEBRTC_ANDROID
#include "livekit/android.h"
#endif

#if defined(USE_NVIDIA_VIDEO_CODEC)
#include "nvidia/nvidia_encoder_factory.h"
#endif

#if defined(USE_VAAPI_VIDEO_CODEC)
#include "vaapi/vaapi_encoder_factory.h"
#endif

#if defined(USE_MFT_VIDEO_CODEC)
#include "mft/mft_encoder_factory.h"
#endif

namespace livekit_ffi {

using Factory = webrtc::VideoEncoderFactoryTemplate<
    webrtc::LibvpxVp8EncoderTemplateAdapter,
#if defined(WEBRTC_USE_H264)
    webrtc::OpenH264EncoderTemplateAdapter,
#endif
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
    webrtc::LibaomAv1EncoderTemplateAdapter,
#endif
    webrtc::LibvpxVp9EncoderTemplateAdapter>;

namespace {

// Staff isolation arm: the stock software H.264 encoder with the codec mode
// rewritten from kScreensharing to kRealtimeVideo at InitEncode.
//
// Why: an AMD field run whose shares all fell back to OpenH264 encoded
// entire 2s windows as ALL keyframes (56 of 62 frames in one window) with
// ~0 PLI/FIR from viewers, so no one asked for them. OpenH264's
// SCREEN_CONTENT_REAL_TIME usage runs a scene-change detector that promotes
// frames to IDR on its own, and a fullscreen game changes most of the frame
// most of the time. Every IDR costs several P-frames' worth of CPU, which is
// the observed 90%+ CPU and 3-18fps output. The detector lives inside the
// prebuilt libwebrtc binary where it cannot be flipped directly; the camera
// usage mode does not run it. This wrapper is the controlled experiment for
// that hypothesis, and if it holds it is also the production shape for the
// software fallback.
class CameraModeH264Encoder : public webrtc::VideoEncoder {
 public:
  explicit CameraModeH264Encoder(std::unique_ptr<webrtc::VideoEncoder> inner)
      : inner_(std::move(inner)) {}

  int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                     const Settings& settings) override {
    webrtc::VideoCodec camera = *codec_settings;
    camera.mode = webrtc::VideoCodecMode::kRealtimeVideo;
    return inner_->InitEncode(&camera, settings);
  }
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override {
    return inner_->RegisterEncodeCompleteCallback(callback);
  }
  int32_t Release() override { return inner_->Release(); }
  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override {
    return inner_->Encode(frame, frame_types);
  }
  void SetRates(const RateControlParameters& parameters) override {
    inner_->SetRates(parameters);
  }
  void OnPacketLossRateUpdate(float packet_loss_rate) override {
    inner_->OnPacketLossRateUpdate(packet_loss_rate);
  }
  void OnRttUpdate(int64_t rtt_ms) override { inner_->OnRttUpdate(rtt_ms); }
  void OnLossNotification(const LossNotification& loss_notification) override {
    inner_->OnLossNotification(loss_notification);
  }
  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info = inner_->GetEncoderInfo();
    // Visible in the publisher stats line's `encoder=` field, so a log can
    // never mistake this arm for the plain software one.
    info.implementation_name += " (camera-mode)";
    return info;
  }

 private:
  std::unique_ptr<webrtc::VideoEncoder> inner_;
};

// The production software fallback: the stock template factory, with every
// H.264 encoder it produces wrapped in CameraModeH264Encoder.
//
// Evidence, same machine, same 1080p60 scene, same ~35% GPU load, vsync on:
// plain OpenH264 in screen-content mode averaged 53.7fps with dips to 30,
// 14.3ms per frame, and a 4.6% keyframe ratio with windows of 24 IDRs in 2s;
// camera mode held 60fps flat at 9.2ms per frame with a 0.2% keyframe ratio.
// The scene-change detector is the whole difference, and it costs a third
// of the encode budget while feeding viewers a stream of I-frames nobody
// asked for. Camera tracks already run in camera mode, so wrapping them is a
// no-op; only screen shares change. Isolation arm 3 (`SW 60`) keeps the
// unwrapped encoder on purpose so the A/B stays meaningful.
class CameraModeSoftwareFactory : public webrtc::VideoEncoderFactory {
 public:
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return inner_.GetSupportedFormats();
  }
  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override {
    return inner_.QueryCodecSupport(format, scalability_mode);
  }
  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    auto encoder = inner_.Create(env, format);
    if (encoder && format.name == "H264") {
      return std::make_unique<CameraModeH264Encoder>(std::move(encoder));
    }
    return encoder;
  }

 private:
  Factory inner_;
};

}  // namespace

VideoEncoderFactory::InternalFactory::InternalFactory() {
#ifdef __APPLE__
  factories_.push_back(livekit_ffi::CreateObjCVideoEncoderFactory());
#endif

#ifdef WEBRTC_ANDROID
  factories_.push_back(CreateAndroidVideoEncoderFactory());
#endif

#if defined(USE_NVIDIA_VIDEO_CODEC)
  if (webrtc::NvidiaVideoEncoderFactory::IsSupported()) {
    factories_.push_back(std::make_unique<webrtc::NvidiaVideoEncoderFactory>());
    livekit::mft_diag_flag(1);
  } else {
#endif

#if defined(USE_MFT_VIDEO_CODEC)
    if (webrtc::MftVideoEncoderFactory::IsSupported()) {
      factories_.push_back(std::make_unique<webrtc::MftVideoEncoderFactory>());
      livekit::mft_diag_flag(2);
    }
#endif

#if defined(USE_VAAPI_VIDEO_CODEC)
    if (webrtc::VAAPIVideoEncoderFactory::IsSupported()) {
      factories_.push_back(std::make_unique<webrtc::VAAPIVideoEncoderFactory>());
    }
#endif

#if defined(USE_NVIDIA_VIDEO_CODEC)
  }
#endif
}

std::vector<webrtc::SdpVideoFormat>
VideoEncoderFactory::InternalFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> formats = Factory().GetSupportedFormats();

  for (const auto& factory : factories_) {
    auto supported_formats = factory->GetSupportedFormats();
    formats.insert(formats.end(), supported_formats.begin(),
                   supported_formats.end());
  }
  return formats;
}

VideoEncoderFactory::CodecSupport
VideoEncoderFactory::InternalFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
  auto original_format =
      webrtc::FuzzyMatchSdpVideoFormat(Factory().GetSupportedFormats(), format);
  return original_format
             ? Factory().QueryCodecSupport(*original_format, scalability_mode)
             : webrtc::VideoEncoderFactory::CodecSupport{.is_supported = false};
}

std::unique_ptr<webrtc::VideoEncoder>
VideoEncoderFactory::InternalFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  const uint32_t isolation_mode =
      livekit::screen_encoder_mode().load(std::memory_order_relaxed);

  if (isolation_mode == 3) {
    auto original_format =
        webrtc::FuzzyMatchSdpVideoFormat(Factory().GetSupportedFormats(), format);
    if (original_format) {
      RTC_LOG(LS_INFO) << "Screen encoder isolation: forcing software for "
                       << format.name;
      return Factory().Create(env, *original_format);
    }
    RTC_LOG(LS_ERROR) << "Software isolation arm does not support " << format.name;
    return nullptr;
  }

  if (isolation_mode == 4) {
    auto original_format =
        webrtc::FuzzyMatchSdpVideoFormat(Factory().GetSupportedFormats(), format);
    if (original_format) {
      auto inner = Factory().Create(env, *original_format);
      if (inner) {
        RTC_LOG(LS_INFO)
            << "Screen encoder isolation: software with camera-mode override";
        return std::make_unique<CameraModeH264Encoder>(std::move(inner));
      }
    }
    RTC_LOG(LS_ERROR) << "Camera-mode software arm does not support "
                      << format.name;
    return nullptr;
  }

#if defined(USE_MFT_VIDEO_CODEC)
  if (isolation_mode == 2) {
    webrtc::MftVideoEncoderFactory factory;
    if (webrtc::MftVideoEncoderFactory::IsSupported()) {
      for (const auto& supported_format : factory.GetSupportedFormats()) {
        if (supported_format.IsSameCodec(format)) {
          RTC_LOG(LS_INFO) << "Screen encoder isolation: forcing MFT for "
                           << format.name;
          return factory.Create(env, format);
        }
      }
    }
    RTC_LOG(LS_ERROR) << "MFT isolation arm unavailable for " << format.name;
    return nullptr;
  }
#endif

#if defined(USE_NVIDIA_VIDEO_CODEC)
  if (isolation_mode == 1) {
    webrtc::NvidiaVideoEncoderFactory factory;
    if (webrtc::NvidiaVideoEncoderFactory::IsSupported()) {
      for (const auto& supported_format : factory.GetSupportedFormats()) {
        if (supported_format.IsSameCodec(format)) {
          RTC_LOG(LS_INFO) << "Screen encoder isolation: forcing NVIDIA for "
                           << format.name;
          return factory.Create(env, format);
        }
      }
    }
    RTC_LOG(LS_ERROR) << "NVIDIA isolation arm unavailable for " << format.name;
    return nullptr;
  }
#endif

  for (const auto& factory : factories_) {
    for (const auto& supported_format : factory->GetSupportedFormats()) {
      if (supported_format.IsSameCodec(format))
        return factory->Create(env, format);
    }
  }

  auto original_format =
      webrtc::FuzzyMatchSdpVideoFormat(Factory().GetSupportedFormats(), format);

  if (original_format) {
    // No matching hardware factory is also a production software path, not
    // only a failed hardware InitEncode. Keep screen-content mode confined
    // to the explicit software isolation arm above.
    return CameraModeSoftwareFactory().Create(env, *original_format);
  }

  RTC_LOG(LS_ERROR) << "No VideoEncoder found for " << format.name;
  return nullptr;
}

VideoEncoderFactory::VideoEncoderFactory() {
  internal_factory_ = std::make_unique<InternalFactory>();
  // Software-only factory (VP8/VP9/AV1 and, where built, OpenH264) used as the
  // SimulcastEncoderAdapter fallback below.
  software_factory_ = std::make_unique<CameraModeSoftwareFactory>();
}

std::vector<webrtc::SdpVideoFormat> VideoEncoderFactory::GetSupportedFormats()
    const {
  return internal_factory_->GetSupportedFormats();
}

VideoEncoderFactory::CodecSupport VideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
  return internal_factory_->QueryCodecSupport(format, scalability_mode);
}

std::unique_ptr<webrtc::VideoEncoder> VideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  std::unique_ptr<webrtc::VideoEncoder> encoder;
  if (format.IsCodecInList(internal_factory_->GetSupportedFormats())) {
    // Primary = internal factory (hardware first), fallback = software factory.
    // Passing nullptr here meant a hardware encoder failing mid-session left a
    // dead track; the software fallback keeps the stream alive.
    //
    // EXCEPT while a staff isolation arm is forced. The isolation branches in
    // InternalFactory::Create fail closed on purpose - and this fallback was
    // silently re-opening them: an AMD test ran arms forcing NVENC and MFT,
    // both correctly returned nullptr, and the adapter handed every one of
    // them OpenH264. Seven sessions of "A/B data" measured the same encoder
    // seven times and nothing in the logs said so. A forced arm that cannot
    // build must produce a visibly dead track, because for a diagnostic run
    // a plausible wrong measurement is strictly worse than no measurement.
    const uint32_t isolation_mode =
        livekit::screen_encoder_mode().load(std::memory_order_relaxed);
    encoder = std::make_unique<webrtc::SimulcastEncoderAdapter>(
        env, internal_factory_.get(),
        isolation_mode != 0 ? nullptr : software_factory_.get(), format);
  }

  return encoder;
}

}  // namespace livekit_ffi
