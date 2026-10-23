#include "dxgi_hooks.h"

#include <windows.h>
#include <dxgi1_2.h>

#include <atomic>
#include <vector>

#include "com_ptr.h"
#include "log.h"
#include "memory.h"
#include "swapchain_proxy.h"

namespace sp::dxgi {
namespace {

// COM vtable slots. These are fixed by the interface definitions, not by any implementation.
constexpr size_t kFactoryCreateSwapChain = 10;          // IDXGIFactory
constexpr size_t kFactory2CreateSwapChainForHwnd = 15;  // IDXGIFactory2
constexpr size_t kSwapChainPresent = 8;                 // IDXGISwapChain
constexpr size_t kSwapChainResizeTarget = 14;           // IDXGISwapChain

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);

bool g_forceRefresh = false;
bool g_disableVSync = false;
bool g_upscaling = false;
upscale::Settings g_upscaleSettings;

std::atomic<CreateSwapChainFn> g_createSwapChain{nullptr};
std::atomic<CreateSwapChainForHwndFn> g_createSwapChainForHwnd{nullptr};
std::atomic<PresentFn> g_present{nullptr};
std::atomic<ResizeTargetFn> g_resizeTarget{nullptr};

// Replaces one slot of `object`'s vtable. Vtables are shared by every object of the class,
// so this also catches objects the game creates later.
template <typename Fn>
bool HookSlot(void* object, size_t slot, Fn hook, std::atomic<Fn>& original, const char* name) {
    void** entry = *reinterpret_cast<void***>(object) + slot;
    void* current = *entry;
    if (current == reinterpret_cast<void*>(hook)) return true;
    if (original.load()) {
        // A second, different vtable (another DXGI implementation). One original per slot.
        LOG("DXGI: %s belongs to an unexpected second implementation; left alone.", name);
        return false;
    }
    // Publish the original before the hook so a concurrent call never sees a null original.
    original.store(reinterpret_cast<Fn>(current));
    if (!mem::Write(reinterpret_cast<uintptr_t>(entry), &hook, sizeof(hook))) {
        original.store(nullptr);
        LOG("DXGI: failed to hook %s.", name);
        return false;
    }
    LOG("DXGI: hooked %s.", name);
    return true;
}

// The output whose monitor shows `hwnd`. Caller releases.
IDXGIOutput* OutputForWindow(IDXGIFactory* factory, HWND hwnd) {
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(a, adapter.Put()))) return nullptr;
        for (UINT o = 0;; ++o) {
            IDXGIOutput* output = nullptr;
            if (FAILED(adapter->EnumOutputs(o, &output))) break;
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) return output;
            output->Release();
        }
    }
}

double Hz(const DXGI_RATIONAL& r) {
    return r.Denominator ? static_cast<double>(r.Numerator) / r.Denominator : 0.0;
}

// Highest refresh rate `output` natively supports at width x height, or {0, 0}.
DXGI_RATIONAL BestRefresh(IDXGIOutput* output, UINT width, UINT height, DXGI_FORMAT format) {
    DXGI_RATIONAL best = {0, 0};
    const DXGI_FORMAT formats[] = {format, DXGI_FORMAT_R8G8B8A8_UNORM};
    for (DXGI_FORMAT f : formats) {
        if (f == DXGI_FORMAT_UNKNOWN) continue;
        UINT count = 0;
        if (FAILED(output->GetDisplayModeList(f, 0, &count, nullptr)) || count == 0) continue;
        std::vector<DXGI_MODE_DESC> modes(count);
        if (FAILED(output->GetDisplayModeList(f, 0, &count, modes.data()))) continue;
        for (UINT i = 0; i < count; ++i) {
            const DXGI_MODE_DESC& m = modes[i];
            if (m.Width == width && m.Height == height && Hz(m.RefreshRate) > Hz(best)) best = m.RefreshRate;
        }
        if (best.Denominator) break;
    }
    return best;
}

// Raises mode.RefreshRate to the highest the output supports at that resolution.
bool RaiseRefresh(IDXGIOutput* output, DXGI_MODE_DESC& mode) {
    if (!output || mode.Width == 0 || mode.Height == 0) return false;
    const DXGI_RATIONAL best = BestRefresh(output, mode.Width, mode.Height, mode.Format);
    // The margin keeps us from "upgrading" 59.94 Hz to 60 Hz and similar.
    if (!best.Denominator || Hz(best) <= Hz(mode.RefreshRate) + 0.5) return false;
    LOG("DXGI: %ux%u display mode %.2f Hz -> %.2f Hz", mode.Width, mode.Height, Hz(mode.RefreshRate), Hz(best));
    mode.RefreshRate = best;
    return true;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT /*syncInterval*/, UINT flags) {
    return g_present.load()(self, 0, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* self, const DXGI_MODE_DESC* params) {
    const ResizeTargetFn original = g_resizeTarget.load();
    if (params) {
        DXGI_MODE_DESC mode = *params;
        if (mode.Format == DXGI_FORMAT_UNKNOWN) {
            DXGI_SWAP_CHAIN_DESC desc;
            if (SUCCEEDED(self->GetDesc(&desc))) mode.Format = desc.BufferDesc.Format;
        }
        ComPtr<IDXGIOutput> output;
        if (SUCCEEDED(self->GetContainingOutput(output.Put())) && RaiseRefresh(output.Get(), mode)) {
            mode.Format = params->Format;
            if (SUCCEEDED(original(self, &mode))) return S_OK;
            LOG("DXGI: ResizeTarget rejected the raised refresh rate; using the game's request.");
        }
    }
    return original(self, params);
}

// Hooks for a swap chain the game uses directly (no upscaling proxy).
void HookSwapChain(IDXGISwapChain* swapChain) {
    if (g_disableVSync) HookSlot(swapChain, kSwapChainPresent, &HookPresent, g_present, "IDXGISwapChain::Present");
    if (g_forceRefresh) {
        HookSlot(swapChain, kSwapChainResizeTarget, &HookResizeTarget, g_resizeTarget, "IDXGISwapChain::ResizeTarget");
    }
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    const CreateSwapChainFn original = g_createSwapChain.load();

    if (g_upscaling && desc && out) {
        const auto createReal = [&](IDXGISwapChain** real) {
            DXGI_SWAP_CHAIN_DESC windowed = *desc;
            windowed.Windowed = TRUE;
            return original(self, device, &windowed, real);
        };
        if (IDXGISwapChain4* proxy = upscale::CreateProxy(device, desc->OutputWindow, !desc->Windowed,
                                                          desc->SampleDesc.Count, g_upscaleSettings, createReal)) {
            *out = proxy;
            return S_OK;
        }
    }

    HRESULT hr = E_FAIL;
    bool created = false;
    if (desc && g_forceRefresh) {
        DXGI_SWAP_CHAIN_DESC patched = *desc;
        ComPtr<IDXGIOutput> output(OutputForWindow(self, patched.OutputWindow));
        if (RaiseRefresh(output.Get(), patched.BufferDesc)) {
            hr = original(self, device, &patched, out);
            created = SUCCEEDED(hr);
            if (!created) {
                LOG("DXGI: swap chain creation failed with the raised refresh rate (0x%08lX); retrying as-is.",
                    static_cast<unsigned long>(hr));
            }
        }
    }
    if (!created) hr = original(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                     IDXGIOutput* restrictToOutput, IDXGISwapChain1** out) {
    const CreateSwapChainForHwndFn original = g_createSwapChainForHwnd.load();

    if (g_upscaling && desc && out) {
        const auto createReal = [&](IDXGISwapChain** real) {
            IDXGISwapChain1* chain = nullptr;
            const HRESULT hr = original(self, device, hwnd, desc, nullptr, restrictToOutput, &chain);  // windowed
            *real = chain;
            return hr;
        };
        const bool gameFullscreen = fullscreen && !fullscreen->Windowed;
        if (IDXGISwapChain4* proxy = upscale::CreateProxy(device, hwnd, gameFullscreen, desc->SampleDesc.Count,
                                                          g_upscaleSettings, createReal)) {
            *out = proxy;
            return S_OK;
        }
    }

    HRESULT hr = E_FAIL;
    bool created = false;
    if (desc && fullscreen && g_forceRefresh) {
        DXGI_MODE_DESC mode = {};
        mode.Width = desc->Width;
        mode.Height = desc->Height;
        mode.Format = desc->Format;
        mode.RefreshRate = fullscreen->RefreshRate;
        ComPtr<IDXGIOutput> output(OutputForWindow(self, hwnd));
        if (RaiseRefresh(output.Get(), mode)) {
            DXGI_SWAP_CHAIN_FULLSCREEN_DESC patched = *fullscreen;
            patched.RefreshRate = mode.RefreshRate;
            hr = original(self, device, hwnd, desc, &patched, restrictToOutput, out);
            created = SUCCEEDED(hr);
            if (!created) {
                LOG("DXGI: swap chain creation failed with the raised refresh rate (0x%08lX); retrying as-is.",
                    static_cast<unsigned long>(hr));
            }
        }
    }
    if (!created) hr = original(self, device, hwnd, desc, fullscreen, restrictToOutput, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

}  // namespace

void Install(const Config& config) {
    g_forceRefresh = config.forceHighestRefreshRate;
    g_disableVSync = config.disableVSync;
    g_upscaling = config.upscaling;
    g_upscaleSettings.sharpness = config.sharpness;
    g_upscaleSettings.disableVSync = config.disableVSync;

    // Loaded the same way the game loads it, so we patch the implementation the game will use
    // (system DXGI, DXVK under Proton, or a wrapper in the game folder). Never unloaded.
    HMODULE module = LoadLibraryW(L"dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = module ? reinterpret_cast<CreateFactoryFn>(
                                      reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory1")))
                                : nullptr;
    if (!createFactory) {
        LOG("DXGI: CreateDXGIFactory1 unavailable; display hooks disabled.");
        return;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.Put())))) {
        LOG("DXGI: could not create a factory; display hooks disabled.");
        return;
    }
    HookSlot(factory.Get(), kFactoryCreateSwapChain, &HookCreateSwapChain, g_createSwapChain,
             "IDXGIFactory::CreateSwapChain");

    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(factory.As(factory2))) {
        HookSlot(factory2.Get(), kFactory2CreateSwapChainForHwnd, &HookCreateSwapChainForHwnd, g_createSwapChainForHwnd,
                 "IDXGIFactory2::CreateSwapChainForHwnd");
    }
}

}  // namespace sp::dxgi
