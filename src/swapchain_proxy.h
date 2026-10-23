#pragma once

#include <windows.h>
#include <dxgi1_5.h>

#include <functional>

namespace sp::upscale {

struct Settings {
    float sharpness = 0.8f;
    bool disableVSync = false;
};

// Creates the game's swap chain (always windowed, via `createReal`) behind a proxy. The game
// keeps rendering at its own resolution into a texture the proxy hands out as the back buffer;
// on Present the proxy upscales that with FSR to the window, which it keeps borderless and
// covering the monitor. Fullscreen requests from the game are honoured virtually.
//
// Returns nullptr if upscaling can't be used here; the caller then creates the swap chain normally.
IDXGISwapChain4* CreateProxy(IUnknown* device, HWND hwnd, bool gameFullscreen, UINT sampleCount,
                             const Settings& settings, const std::function<HRESULT(IDXGISwapChain**)>& createReal);

}  // namespace sp::upscale
