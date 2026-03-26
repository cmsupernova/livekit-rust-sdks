#include "mft_encoder_factory.h"

#include <memory>

#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>

#include "mft_h264_encoder_impl.h"
#include "rtc_base/logging.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace webrtc {

static bool HasHardwareH264Mft() {
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  if (FAILED(hr))
    return false;

  MFT_REGISTER_TYPE_INFO input_type = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_type = {MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                 &input_type, &output_type, &activates, &count);

  if (SUCCEEDED(hr)) {
    for (UINT32 i = 0; i < count; i++) {
      activates[i]->Release();
    }
    CoTaskMemFree(activates);
  }

  MFShutdown();
  return SUCCEEDED(hr) && count > 0;
}

MftVideoEncoderFactory::MftVideoEncoderFactory() {
  std::map<std::string, std::string> baselineParameters = {
      {"profile-level-id", "42e01f"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };
  supported_formats_.push_back(SdpVideoFormat("H264", baselineParameters));
}

MftVideoEncoderFactory::~MftVideoEncoderFactory() {}

bool MftVideoEncoderFactory::IsSupported() {
  bool supported = HasHardwareH264Mft();
  if (supported) {
    RTC_LOG(LS_INFO) << "MFT hardware H.264 encoder is available.";
  }
  return supported;
}

std::unique_ptr<VideoEncoder> MftVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      RTC_LOG(LS_INFO) << "Using Windows MFT hardware encoder for H264";
      return std::make_unique<MftH264EncoderImpl>(env);
    }
  }
  return nullptr;
}

std::vector<SdpVideoFormat> MftVideoEncoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::vector<SdpVideoFormat> MftVideoEncoderFactory::GetImplementations()
    const {
  return supported_formats_;
}

}  // namespace webrtc
