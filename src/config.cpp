#include "config.h"

#include <cwchar>

namespace sp {
namespace {

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback, const std::wstring& ini) {
    return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, ini.c_str()) != 0;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback, const std::wstring& ini) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback, const std::wstring& ini) {
    wchar_t text[32] = {};
    GetPrivateProfileStringW(section, key, L"", text, 32, ini.c_str());
    wchar_t* end = nullptr;
    const float value = std::wcstof(text, &end);
    return end != text ? value : fallback;
}

// Drive-letter or UNC paths are absolute. Anything else, including Mod Engine's "\file.dll"
// style, is taken relative to `baseDir`.
std::wstring ResolvePath(std::wstring path, const std::wstring& baseDir) {
    if (path.empty()) return path;
    const bool absolute = (path.size() >= 2 && path[1] == L':') ||
                          (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
    if (absolute) return path;
    while (!path.empty() && (path[0] == L'\\' || path[0] == L'/')) path.erase(0, 1);
    return baseDir + L"\\" + path;
}

}  // namespace

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

Config LoadConfig(const std::wstring& ini, const std::wstring& baseDir) {
    Config c;
    c.unlockFps = ReadBool(L"FrameRate", L"UnlockFps", c.unlockFps, ini);
    c.targetFps = ReadInt(L"FrameRate", L"TargetFps", c.targetFps, ini);
    c.forceHighestRefreshRate = ReadBool(L"Display", L"ForceHighestRefreshRate", c.forceHighestRefreshRate, ini);
    c.disableVSync = ReadBool(L"Display", L"DisableVSync", c.disableVSync, ini);
    c.upscaling = ReadBool(L"Upscaling", L"Enable", c.upscaling, ini);
    c.sharpness = ReadFloat(L"Upscaling", L"Sharpness", c.sharpness, ini);
    if (!(c.sharpness >= 0.0f)) c.sharpness = 0.0f;  // also catches NaN
    if (c.sharpness > 1.0f) c.sharpness = 1.0f;
    c.highResolutionTimer = ReadBool(L"System", L"HighResolutionTimer", c.highResolutionTimer, ini);
    c.scanTimeoutSeconds = ReadInt(L"System", L"ScanTimeoutSeconds", c.scanTimeoutSeconds, ini);
    if (c.scanTimeoutSeconds < 5) c.scanTimeoutSeconds = 5;

    wchar_t chain[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Loader", L"ChainDLL", L"", chain, MAX_PATH, ini.c_str());
    c.chainDll = ResolvePath(chain, baseDir);
    return c;
}

}  // namespace sp
