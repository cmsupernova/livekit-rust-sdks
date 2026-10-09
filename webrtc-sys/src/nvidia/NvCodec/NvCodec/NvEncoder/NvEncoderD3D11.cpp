#include "NvEncoderD3D11.h"

NvEncoderD3D11::NvEncoderD3D11(ID3D11Device* device,
                               uint32_t nWidth,
                               uint32_t nHeight,
                               NV_ENC_BUFFER_FORMAT eBufferFormat,
                               uint32_t nExtraOutputDelay)
    : NvEncoder(NV_ENC_DEVICE_TYPE_DIRECTX,
                device,
                nWidth,
                nHeight,
                eBufferFormat,
                nExtraOutputDelay,
                false),
      m_device(device) {
  if (!m_hEncoder) {
    NVENC_THROW_ERROR("Encoder Initialization failed",
                      NV_ENC_ERR_INVALID_DEVICE);
  }
}

NvEncoderD3D11::~NvEncoderD3D11() {
  ReleaseD3D11Resources();
}

void NvEncoderD3D11::AllocateInputBuffers(int32_t numInputBuffers) {
  if (!IsHWEncoderInitialized()) {
    NVENC_THROW_ERROR("Encoder intialization failed",
                      NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
  }
  if (GetPixelFormat() != NV_ENC_BUFFER_FORMAT_NV12) {
    NVENC_THROW_ERROR("D3D11 input takes NV12 only",
                      NV_ENC_ERR_UNSUPPORTED_PARAM);
  }

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = GetMaxEncodeWidth();
  desc.Height = GetMaxEncodeHeight();
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  // One at a time, so each texture is in m_vInputFrames (and released with
  // it) as soon as it is registered, even if a later one fails.
  for (int32_t i = 0; i < numInputBuffers; i++) {
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &texture))) {
      NVENC_THROW_ERROR("D3D11 input texture creation failed",
                        NV_ENC_ERR_OUT_OF_MEMORY);
    }
    try {
      RegisterInputResources({texture}, NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX,
                             GetMaxEncodeWidth(), GetMaxEncodeHeight(), 0,
                             GetPixelFormat());
    } catch (...) {
      texture->Release();
      throw;
    }
  }
}

void NvEncoderD3D11::ReleaseInputBuffers() {
  ReleaseD3D11Resources();
}

void NvEncoderD3D11::ReleaseD3D11Resources() {
  if (m_hEncoder) {
    UnregisterInputResources();
  }
  for (uint32_t i = 0; i < m_vInputFrames.size(); ++i) {
    if (m_vInputFrames[i].inputPtr) {
      static_cast<ID3D11Texture2D*>(m_vInputFrames[i].inputPtr)->Release();
    }
  }
  m_vInputFrames.clear();
}
