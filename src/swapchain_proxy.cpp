#include "swapchain_proxy.h"

#include <d3d11.h>

#include <atomic>

#include "com_ptr.h"
#include "log.h"
#include "upscaler.h"

namespace sp::upscale {
namespace {

UINT BindFlagsForUsage(DXGI_USAGE usage) {
    UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (usage & DXGI_USAGE_UNORDERED_ACCESS) bind |= D3D11_BIND_UNORDERED_ACCESS;
    return bind;
}

class SwapChainProxy final : public IDXGISwapChain4 {
public:
    SwapChainProxy(ID3D11Device* device, HWND hwnd, bool gameFullscreen, const Settings& settings)
        : hwnd_(hwnd), fullscreen_(gameFullscreen ? TRUE : FALSE), settings_(settings) {
        device->AddRef();
        *device_.Put() = device;
        device->GetImmediateContext(context_.Put());
    }

    bool InitUpscaler() { return upscaler_.Init(device_.Get()); }

    bool Attach(IDXGISwapChain* real) {
        real->AddRef();
        *real_.Put() = real;
        real_.As(real1_);
        real_.As(real2_);
        real_.As(real3_);
        real_.As(real4_);

        DXGI_SWAP_CHAIN_DESC desc;
        if (FAILED(real_->GetDesc(&desc))) return false;
        gameW_ = desc.BufferDesc.Width;
        gameH_ = desc.BufferDesc.Height;
        format_ = desc.BufferDesc.Format;
        usage_ = desc.BufferUsage;
        flags_ = desc.Flags;

        // Fullscreen is virtual, so DXGI's own Alt+Enter handling must not switch the real chain.
        ComPtr<IDXGIFactory> factory;
        if (SUCCEEDED(real_->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.Put())))) {
            factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        }

        if (!AcquireRealBuffer() || !CreateGameBuffer()) return false;
        LOG("FSR: swap chain wrapped. Game renders at %ux%u, sharpness %.2f.", gameW_, gameH_, settings_.sharpness);
        return true;
    }

    // IUnknown -----------------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || (riid == __uuidof(IDXGISwapChain1) && real1_) ||
            (riid == __uuidof(IDXGISwapChain2) && real2_) || (riid == __uuidof(IDXGISwapChain3) && real3_) ||
            (riid == __uuidof(IDXGISwapChain4) && real4_)) {
            *out = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        // Anything else (private or media interfaces) is answered by the real swap chain.
        return real_->QueryInterface(riid, out);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (refs == 0) delete this;
        return refs;
    }

    // IDXGIObject / IDXGIDeviceSubObject ---------------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return real_->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override {
        return real_->SetPrivateDataInterface(guid, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return real_->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return real_->GetParent(riid, parent); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override { return real_->GetDevice(riid, device); }

    // IDXGISwapChain -----------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override {
        const bool test = (flags & DXGI_PRESENT_TEST) != 0;
        if (!test) BeforePresent();
        const HRESULT hr = real_->Present(settings_.disableVSync ? 0 : syncInterval, flags);
        if (!test) AfterPresent();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (index == 0 && gameTex_) return gameTex_->QueryInterface(riid, surface);
        return real_->GetBuffer(index, riid, surface);
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* /*target*/) override {
        if (fullscreen != fullscreen_) LOG("FSR: game switched to %s (borderless behind the scenes).", fullscreen ? "fullscreen" : "windowed");
        fullscreen_ = fullscreen;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override {
        if (fullscreen) *fullscreen = fullscreen_;
        if (target) {
            *target = nullptr;
            if (fullscreen_) real_->GetContainingOutput(target);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        const HRESULT hr = real_->GetDesc(desc);
        if (SUCCEEDED(hr)) {
            desc->BufferDesc.Width = gameW_;
            desc->BufferDesc.Height = gameH_;
            desc->Windowed = !fullscreen_;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
        if (width == 0 || height == 0) {
            // "Match the window". Use the size the game last asked for, else the window size if the
            // game sized the window itself. The size we force the window to says nothing about the
            // game's resolution, so it is ignored.
            RECT rc;
            if (pendingW_ && pendingH_) {
                width = pendingW_;
                height = pendingH_;
            } else if (GetClientRect(hwnd_, &rc) && rc.right > 0 && rc.bottom > 0 &&
                       (static_cast<UINT>(rc.right) != enforcedW_ || static_cast<UINT>(rc.bottom) != enforcedH_)) {
                width = static_cast<UINT>(rc.right);
                height = static_cast<UINT>(rc.bottom);
            } else {
                width = gameW_;
                height = gameH_;
            }
        }
        pendingW_ = pendingH_ = 0;

        gameSrv_.Reset();
        gameTex_.Reset();
        ReleaseRealBuffer();
        // The real buffers always follow the window's client area.
        const HRESULT hr = real_->ResizeBuffers(count, 0, 0, format, flags);
        if (FAILED(hr)) {
            LOG("FSR: ResizeBuffers failed (0x%08lX).", static_cast<unsigned long>(hr));
            AcquireRealBuffer();
            CreateGameBuffer();
            return hr;
        }
        gameW_ = width;
        gameH_ = height;
        if (format != DXGI_FORMAT_UNKNOWN) format_ = format;
        flags_ = flags;
        if (!AcquireRealBuffer() || !CreateGameBuffer()) return E_OUTOFMEMORY;
        LOG("FSR: game resolution %ux%u, output %ux%u.", gameW_, gameH_, realW_, realH_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override {
        // The window stays borderless and monitor-sized; remember the size for ResizeBuffers(0, 0).
        if (mode && mode->Width && mode->Height) {
            pendingW_ = mode->Width;
            pendingH_ = mode->Height;
            // A real mode switch would resize the window and send WM_SIZE, which is what prompts
            // games to call ResizeBuffers. Ours doesn't, so send it ourselves.
            if (pendingW_ != gameW_ || pendingH_ != gameH_) {
                RECT rc;
                if (GetClientRect(hwnd_, &rc)) PostMessageW(hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override {
        return real_->GetContainingOutput(output);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override {
        return real_->GetFrameStatistics(stats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override { return real_->GetLastPresentCount(count); }

    // IDXGISwapChain1 ----------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        if (!real1_) return E_NOTIMPL;
        const HRESULT hr = real1_->GetDesc1(desc);
        if (SUCCEEDED(hr)) {
            desc->Width = gameW_;
            desc->Height = gameH_;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override {
        if (!real1_) return E_NOTIMPL;
        const HRESULT hr = real1_->GetFullscreenDesc(desc);
        if (SUCCEEDED(hr)) desc->Windowed = !fullscreen_;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override { return real1_ ? real1_->GetHwnd(hwnd) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unk) override {
        return real1_ ? real1_->GetCoreWindow(riid, unk) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        if (!real1_) return E_NOTIMPL;
        const bool test = (flags & DXGI_PRESENT_TEST) != 0;
        if (!test) BeforePresent();
        // Dirty rects are in game coordinates, which no longer match the back buffer.
        DXGI_PRESENT_PARAMETERS full = {};
        const HRESULT hr = real1_->Present1(settings_.disableVSync ? 0 : syncInterval, flags, &full);
        if (!test) AfterPresent();
        return hr;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {
        return real1_ ? real1_->IsTemporaryMonoSupported() : FALSE;
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override {
        return real1_ ? real1_->GetRestrictToOutput(output) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override {
        return real1_ ? real1_->SetBackgroundColor(color) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override {
        return real1_ ? real1_->GetBackgroundColor(color) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override {
        return real1_ ? real1_->SetRotation(rotation) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override {
        return real1_ ? real1_->GetRotation(rotation) : E_NOTIMPL;
    }

    // IDXGISwapChain2 ----------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT, UINT) override {
        // Would crop the upscaled output; the game's source size is its buffer size here.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override {
        if (width) *width = gameW_;
        if (height) *height = gameH_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override {
        return real2_ ? real2_->SetMaximumFrameLatency(latency) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override {
        return real2_ ? real2_->GetMaximumFrameLatency(latency) : E_NOTIMPL;
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        return real2_ ? real2_->GetFrameLatencyWaitableObject() : nullptr;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override {
        return real2_ ? real2_->SetMatrixTransform(matrix) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override {
        return real2_ ? real2_->GetMatrixTransform(matrix) : E_NOTIMPL;
    }

    // IDXGISwapChain3 ----------------------------------------------------------------------------
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }  // the game only ever sees one buffer
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* support) override {
        return real3_ ? real3_->CheckColorSpaceSupport(space, support) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override {
        return real3_ ? real3_->SetColorSpace1(space) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT*, IUnknown* const*) override {
        return ResizeBuffers(count, width, height, format, flags);
    }

    // IDXGISwapChain4 ----------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override {
        return real4_ ? real4_->SetHDRMetaData(type, size, metadata) : E_NOTIMPL;
    }

private:
    ~SwapChainProxy() {
        // DXGI requires a swap chain to be windowed when it's destroyed.
        BOOL realFullscreen = FALSE;
        if (real_ && SUCCEEDED(real_->GetFullscreenState(&realFullscreen, nullptr)) && realFullscreen) {
            real_->SetFullscreenState(FALSE, nullptr);
        }
    }

    bool CreateGameBuffer() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = gameW_;
        td.Height = gameH_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format_;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = BindFlagsForUsage(usage_);
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, gameTex_.Put());
        if (FAILED(hr) && (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
            td.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;  // not every format supports typed UAVs
            hr = device_->CreateTexture2D(&td, nullptr, gameTex_.Put());
        }
        if (FAILED(hr) || FAILED(device_->CreateShaderResourceView(gameTex_.Get(), nullptr, gameSrv_.Put()))) {
            LOG("FSR: creating the %ux%u game buffer failed (format %u).", gameW_, gameH_, static_cast<unsigned>(format_));
            gameTex_.Reset();
            gameSrv_.Reset();
            return false;
        }
        return true;
    }

    bool AcquireRealBuffer() {
        if (FAILED(real_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(realTex_.Put())))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC td;
        realTex_->GetDesc(&td);
        realW_ = td.Width;
        realH_ = td.Height;
        if (FAILED(device_->CreateRenderTargetView(realTex_.Get(), nullptr, realRtv_.Put()))) {
            realTex_.Reset();
            return false;
        }
        return true;
    }

    void ReleaseRealBuffer() {
        realRtv_.Reset();
        realTex_.Reset();
    }

    // Keep the real buffers the size of the window's client area.
    void SyncOutputSize() {
        RECT rc;
        if (!GetClientRect(hwnd_, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
        const UINT w = static_cast<UINT>(rc.right);
        const UINT h = static_cast<UINT>(rc.bottom);
        if ((w == realW_ && h == realH_ && realRtv_) || (w == failedW_ && h == failedH_)) return;
        ReleaseRealBuffer();
        const HRESULT hr = real_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags_);
        if (FAILED(hr)) {
            LOG("FSR: resizing output to %ux%u failed (0x%08lX).", w, h, static_cast<unsigned long>(hr));
            failedW_ = w;  // don't retry every frame
            failedH_ = h;
        }
        if (AcquireRealBuffer()) {
            LOG("FSR: output %ux%u, game renders %ux%u (%s).", realW_, realH_, gameW_, gameH_,
                realW_ == gameW_ && realH_ == gameH_ ? "same size, no upscaling" : "FSR upscaling");
        }
    }

    // Borderless and covering the monitor, so the output is native resolution.
    void MaintainWindow() {
        if (windowGaveUp_ || IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) return;
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
        const RECT& m = mi.rcMonitor;
        enforcedW_ = static_cast<UINT>(m.right - m.left);
        enforcedH_ = static_cast<UINT>(m.bottom - m.top);

        constexpr LONG_PTR kFrame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        constexpr LONG_PTR kExFrame = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
        const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const bool framed = (style & kFrame) || (exStyle & kExFrame);
        if (!framed && EqualRect(&wr, &m)) return;

        if (++windowFixes_ > 20) {
            windowGaveUp_ = true;
            LOG("FSR: the game keeps resizing its window; no longer forcing borderless.");
            return;
        }
        if (framed) {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, (style & ~kFrame) | WS_POPUP);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle & ~kExFrame);
        }
        // Async so we never block on a window thread that may be waiting on this one.
        SetWindowPos(hwnd_, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
        LOG("FSR: window set to borderless %ldx%ld.", m.right - m.left, m.bottom - m.top);
    }

    void BeforePresent() {
        SyncOutputSize();
        if (!gameTex_ || !realRtv_) return;
        if (gameW_ == realW_ && gameH_ == realH_) {
            context_->CopyResource(realTex_.Get(), gameTex_.Get());
        } else {
            upscaler_.Render(gameSrv_.Get(), gameW_, gameH_, realRtv_.Get(), realW_, realH_, settings_.sharpness);
        }
    }

    void AfterPresent() {
        // After the real Present, so any window messages this triggers re-enter us between frames.
        if (frame_++ % 30 == 0) MaintainWindow();
    }

    std::atomic<ULONG> refs_{1};
    ComPtr<IDXGISwapChain> real_;
    ComPtr<IDXGISwapChain1> real1_;
    ComPtr<IDXGISwapChain2> real2_;
    ComPtr<IDXGISwapChain3> real3_;
    ComPtr<IDXGISwapChain4> real4_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    Upscaler upscaler_;

    HWND hwnd_;
    BOOL fullscreen_;  // what the game believes
    Settings settings_;

    // What the game sees.
    UINT gameW_ = 0;
    UINT gameH_ = 0;
    UINT pendingW_ = 0;  // from ResizeTarget, applied by ResizeBuffers(0, 0)
    UINT pendingH_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_USAGE usage_ = 0;
    UINT flags_ = 0;
    ComPtr<ID3D11Texture2D> gameTex_;
    ComPtr<ID3D11ShaderResourceView> gameSrv_;

    // What actually reaches the screen.
    ComPtr<ID3D11Texture2D> realTex_;
    ComPtr<ID3D11RenderTargetView> realRtv_;
    UINT realW_ = 0;
    UINT realH_ = 0;
    UINT failedW_ = 0;  // last output size ResizeBuffers refused
    UINT failedH_ = 0;
    UINT enforcedW_ = 0;  // window size we impose (the monitor), 0 until we take over the window
    UINT enforcedH_ = 0;

    unsigned frame_ = 0;
    unsigned windowFixes_ = 0;
    bool windowGaveUp_ = false;
};

}  // namespace

IDXGISwapChain4* CreateProxy(IUnknown* device, HWND hwnd, bool gameFullscreen, UINT sampleCount,
                             const Settings& settings, const std::function<HRESULT(IDXGISwapChain**)>& createReal) {
    ComPtr<ID3D11Device> d3d;
    if (!device || FAILED(device->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(d3d.Put())))) {
        LOG("FSR: swap chain is not for a Direct3D 11 device; not upscaling it.");
        return nullptr;
    }
    if (sampleCount != 1 || !IsWindow(hwnd)) {
        LOG("FSR: unsupported swap chain (MSAA back buffer or no window); not upscaling it.");
        return nullptr;
    }

    auto* proxy = new SwapChainProxy(d3d.Get(), hwnd, gameFullscreen, settings);
    if (!proxy->InitUpscaler()) {
        proxy->Release();
        return nullptr;
    }
    ComPtr<IDXGISwapChain> real;
    const HRESULT hr = createReal(real.Put());
    if (FAILED(hr) || !real) {
        LOG("FSR: creating the windowed swap chain failed (0x%08lX).", static_cast<unsigned long>(hr));
        proxy->Release();
        return nullptr;
    }
    if (!proxy->Attach(real.Get())) {
        LOG("FSR: setting up the swap chain proxy failed.");
        proxy->Release();
        return nullptr;
    }
    return proxy;
}

}  // namespace sp::upscale
