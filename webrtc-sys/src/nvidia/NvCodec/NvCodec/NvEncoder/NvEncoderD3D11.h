#pragma once

#include <d3d11.h>
#include <stdint.h>
#include <wrl/client.h>

#include "NvEncoder.h"

/**
 *  @brief Encoder for D3D11 textures. Input frames are NV12 textures on the
 *  encoder's own device, registered with NVENC, so a picture already on the
 *  GPU is copied into one instead of uploaded from system memory.
 */
class NvEncoderD3D11 : public NvEncoder {
 public:
  NvEncoderD3D11(ID3D11Device* device,
                 uint32_t nWidth,
                 uint32_t nHeight,
                 NV_ENC_BUFFER_FORMAT eBufferFormat,
                 uint32_t nExtraOutputDelay);
  ~NvEncoderD3D11() override;

 protected:
  void ReleaseInputBuffers() override;

 private:
  void AllocateInputBuffers(int32_t numInputBuffers) override;
  void ReleaseD3D11Resources();

  Microsoft::WRL::ComPtr<ID3D11Device> m_device;
};
