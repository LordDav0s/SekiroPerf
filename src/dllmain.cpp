// SekiroPerf: loaded by Sekiro Mod Engine through `chainDInput8DLLPath`, or on its own as dinput8.dll.

#include <windows.h>
#include <unknwn.h>

#include "config.h"
#include "dxgi_hooks.h"
#include "fps_unlock.h"
#include "log.h"

namespace {

HMODULE g_chain = nullptr;
sp::Config g_config;

template <typename Fn>
Fn ProcAddress(HMODULE module, const char* name) {
    return module ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}

bool IsSekiroProcess() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') name = p + 1;
    }
    return lstrcmpiW(name, L"sekiro.exe") == 0;
}

void EnableHighResolutionTimer() {
    // Loaded lazily so the DLL itself only imports kernel32/user32.
    using TimeBeginPeriodFn = UINT(WINAPI*)(UINT);
    const auto timeBeginPeriod = ProcAddress<TimeBeginPeriodFn>(LoadLibraryW(L"winmm.dll"), "timeBeginPeriod");
    if (timeBeginPeriod && timeBeginPeriod(1) == 0) {
        LOG("Timer resolution set to 1 ms.");
    } else {
        LOG("Could not set timer resolution.");
    }
}

DWORD WINAPI Worker(LPVOID) {
    // Swap chain hooks first: they have to beat the game's renderer start-up.
    if (g_config.upscaling || g_config.forceHighestRefreshRate || g_config.disableVSync) sp::dxgi::Install(g_config);
    if (g_config.highResolutionTimer) EnableHighResolutionTimer();
    if (g_config.unlockFps) sp::fps::Run(g_config);  // keeps running as a watchdog
    return 0;
}

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);

DirectInput8CreateFn ResolveDirectInput8Create() {
    if (g_chain) {
        if (auto fn = ProcAddress<DirectInput8CreateFn>(g_chain, "DirectInput8Create")) return fn;
        LOG("Chained DLL has no DirectInput8Create export; using the system dinput8.dll.");
    }
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 14 >= MAX_PATH) return nullptr;
    lstrcatW(path, L"\\dinput8.dll");
    return ProcAddress<DirectInput8CreateFn>(LoadLibraryW(path), "DirectInput8Create");
}

}  // namespace

// Mod Engine forwards the game's DirectInput8Create to the chained DLL, so we must provide it.
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out,
                                             IUnknown* outer) {
    static const DirectInput8CreateFn real = ResolveDirectInput8Create();
    if (!real) return E_FAIL;
    return real(instance, version, riid, out, outer);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);

    const std::wstring dir = sp::ModuleDirectory(module);
    sp::log::Init(dir + L"\\sekiro_perf.log");
    g_config = sp::LoadConfig(dir + L"\\sekiro_perf.ini", dir);
    LOG("SekiroPerf loaded from %ls", dir.c_str());

    if (!g_config.chainDll.empty()) {
        // Same thing Mod Engine does for its own chain slot.
        g_chain = LoadLibraryW(g_config.chainDll.c_str());
        if (g_chain == module) g_chain = nullptr;  // pointed at ourselves
        LOG("Chain load %ls: %s", g_config.chainDll.c_str(), g_chain ? "ok" : "FAILED");
    }

    if (!IsSekiroProcess()) {
        LOG("Not sekiro.exe; only forwarding DirectInput.");
        return TRUE;
    }
    LOG("Config: UnlockFps=%d TargetFps=%d ForceHighestRefreshRate=%d DisableVSync=%d Upscaling=%d Sharpness=%.2f "
        "HighResolutionTimer=%d",
        g_config.unlockFps, g_config.targetFps, g_config.forceHighestRefreshRate, g_config.disableVSync,
        g_config.upscaling, g_config.sharpness, g_config.highResolutionTimer);

    // The worker can't start running until the loader lock is released, i.e. after DllMain returns.
    if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
    return TRUE;
}
