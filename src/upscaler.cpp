#include "upscaler.h"

#include <d3dcompiler.h>

#include <cstring>

#include "fsr_shaders.h"
#include "log.h"

namespace sp {
namespace {

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn LoadCompiler() {
    for (const wchar_t* name : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
        if (HMODULE module = LoadLibraryW(name)) {
            if (auto fn = reinterpret_cast<D3DCompileFn>(reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")))) {
                return fn;
            }
        }
    }
    return nullptr;
}

bool Compile(D3DCompileFn compile, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(fsr::kShaderSource, sizeof(fsr::kShaderSource) - 1, "sekiro_perf_fsr.hlsl", nullptr,
                               nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.Put(), errors.Put());
    if (FAILED(hr)) {
        LOG("FSR: compiling %s failed (0x%08lX): %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler output");
        return false;
    }
    return true;
}

}  // namespace

bool Upscaler::Init(ID3D11Device* device) {
    device->AddRef();
    *device_.Put() = device;

    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        LOG("FSR: needs feature level 11_0, device has 0x%X.", static_cast<unsigned>(device->GetFeatureLevel()));
        return false;
    }
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.Put());
    if (FAILED(device_.As(device1)) || FAILED(context.As(context_))) {
        LOG("FSR: Direct3D 11.1 interfaces unavailable.");
        return false;
    }

    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    const UINT stateFlags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                                : 0;
    if (FAILED(device1->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                 nullptr, state_.Put()))) {
        LOG("FSR: could not create a device context state.");
        return false;
    }

    const D3DCompileFn compile = LoadCompiler();
    if (!compile) {
        LOG("FSR: no d3dcompiler DLL found.");
        return false;
    }
    ComPtr<ID3DBlob> vsCode, easuCode, rcasCode;
    if (!Compile(compile, "FullscreenVS", "vs_5_0", vsCode) || !Compile(compile, "EasuPS", "ps_5_0", easuCode) ||
        !Compile(compile, "RcasPS", "ps_5_0", rcasCode)) {
        return false;
    }
    if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs_.Put())) ||
        FAILED(device->CreatePixelShader(easuCode->GetBufferPointer(), easuCode->GetBufferSize(), nullptr, easu_.Put())) ||
        FAILED(device->CreatePixelShader(rcasCode->GetBufferPointer(), rcasCode->GetBufferSize(), nullptr, rcas_.Put()))) {
        LOG("FSR: creating shaders failed.");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(fsr::Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateSamplerState(&sd, sampler_.Put())) ||
        FAILED(device->CreateBuffer(&bd, nullptr, constants_.Put()))) {
        LOG("FSR: creating sampler or constant buffer failed.");
        return false;
    }
    return true;
}

bool Upscaler::EnsureIntermediate(UINT width, UINT height) {
    if (mid_ && midW_ == width && midH_ == height) return true;
    midRtv_.Reset();
    midSrv_.Reset();
    mid_.Reset();
    midW_ = midH_ = 0;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // keeps EASU output precise for RCAS
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, mid_.Put())) ||
        FAILED(device_->CreateShaderResourceView(mid_.Get(), nullptr, midSrv_.Put())) ||
        FAILED(device_->CreateRenderTargetView(mid_.Get(), nullptr, midRtv_.Put()))) {
        LOG("FSR: creating the %ux%u intermediate texture failed.", width, height);
        mid_.Reset();
        return false;
    }
    midW_ = width;
    midH_ = height;
    return true;
}

void Upscaler::Render(ID3D11ShaderResourceView* source, UINT srcW, UINT srcH, ID3D11RenderTargetView* target,
                      UINT dstW, UINT dstH, float sharpness) {
    const fsr::Rect dst = fsr::FitAspect(static_cast<int>(srcW), static_cast<int>(srcH), static_cast<int>(dstW),
                                         static_cast<int>(dstH));
    if (dst.w <= 0 || dst.h <= 0 || !EnsureIntermediate(static_cast<UINT>(dst.w), static_cast<UINT>(dst.h))) return;

    fsr::Constants c = {};
    fsr::SetEasu(c, static_cast<float>(srcW), static_cast<float>(srcH), static_cast<float>(dst.w),
                 static_cast<float>(dst.h));
    fsr::SetRcas(c, fsr::SharpnessToStops(sharpness), dst);

    ID3D11DeviceContext1* ctx = context_.Get();
    ComPtr<ID3DDeviceContextState> gameState;
    ctx->SwapDeviceContextState(state_.Get(), gameState.Put());

    if (!uploadedValid_ || std::memcmp(&c, &uploaded_, sizeof(c)) != 0) {
        ctx->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
        uploaded_ = c;
        uploadedValid_ = true;
    }

    ID3D11Buffer* cb = constants_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    ID3D11ShaderResourceView* const noSrv = nullptr;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);

    // EASU: game image -> intermediate at destination size.
    ID3D11RenderTargetView* midRtv = midRtv_.Get();
    ctx->OMSetRenderTargets(1, &midRtv, nullptr);
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(dst.w), static_cast<float>(dst.h), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(easu_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &source);
    ctx->Draw(3, 0);

    // RCAS: intermediate -> back buffer, centred.
    ctx->PSSetShaderResources(0, 1, &noSrv);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    if (static_cast<UINT>(dst.w) != dstW || static_cast<UINT>(dst.h) != dstH) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(target, black);
    }
    vp = {static_cast<float>(dst.x), static_cast<float>(dst.y), static_cast<float>(dst.w), static_cast<float>(dst.h),
          0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(rcas_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* midSrv = midSrv_.Get();
    ctx->PSSetShaderResources(0, 1, &midSrv);
    ctx->Draw(3, 0);

    // Unbind so nothing we used is still bound when the game's state comes back.
    ctx->PSSetShaderResources(0, 1, &noSrv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->SwapDeviceContextState(gameState.Get(), nullptr);
}

}  // namespace sp
