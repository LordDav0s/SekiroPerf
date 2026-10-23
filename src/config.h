#pragma once

#include <windows.h>

#include <string>

namespace sp {

struct Config {
    // [FrameRate]
    bool unlockFps = true;
    int targetFps = 0;  // <= 0: match the monitor

    // [Display]
    bool forceHighestRefreshRate = true;
    bool disableVSync = false;

    // [Upscaling]
    bool upscaling = true;
    float sharpness = 0.8f;  // 0..1

    // [System]
    bool highResolutionTimer = true;
    int scanTimeoutSeconds = 120;

    // [Loader]
    std::wstring chainDll;  // resolved to an absolute path, empty if unset
};

// Directory containing `module`, without a trailing backslash.
std::wstring ModuleDirectory(HMODULE module);

Config LoadConfig(const std::wstring& iniPath, const std::wstring& baseDir);

}  // namespace sp
