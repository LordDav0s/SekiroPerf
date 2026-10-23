#pragma once

#include <windows.h>
#include <d3d11_1.h>

#include "com_ptr.h"
#include "fsr_constants.h"

namespace sp {

// FSR 1.0 (EASU + RCAS) on the game's D3D11 device. Runs inside its own device context state,
// so the game's pipeline state is untouched.
class Upscaler {
public:
    // False if the device or system can't run it (shader compiler missing, feature level < 11_0...).
    bool Init(ID3D11Device* device);

    // Scales `source` (srcW x srcH) into `target` (dstW x dstH), keeping the aspect ratio.
    void Render(ID3D11ShaderResourceView* source, UINT srcW, UINT srcH, ID3D11RenderTargetView* target, UINT dstW,
                UINT dstH, float sharpness);

private:
    bool EnsureIntermediate(UINT width, UINT height);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<ID3DDeviceContextState> state_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> easu_;
    ComPtr<ID3D11PixelShader> rcas_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> constants_;
    fsr::Constants uploaded_ = {};
    bool uploadedValid_ = false;

    // EASU output at destination size, input to RCAS.
    ComPtr<ID3D11Texture2D> mid_;
    ComPtr<ID3D11ShaderResourceView> midSrv_;
    ComPtr<ID3D11RenderTargetView> midRtv_;
    UINT midW_ = 0;
    UINT midH_ = 0;
};

}  // namespace sp
