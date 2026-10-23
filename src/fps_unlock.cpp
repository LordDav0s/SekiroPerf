#include "fps_unlock.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "memory.h"
#include "sekiro_sigs.h"

namespace sp::fps {
namespace {

using sekiro::ReadF32;

uintptr_t FindFrameTick(const std::vector<mem::Range>& code, const mem::Range& image) {
    static const Pattern sig(sekiro::kFrameTickSig);
    static const Pattern shortSig(sekiro::kFrameTickSigShort);

    const auto isStock = [&](uintptr_t imm) {
        return image.Contains(imm, 4) && mem::IsReadable(imm, 4) &&
               sekiro::IsStockFrameTick(ReadF32(reinterpret_cast<const uint8_t*>(imm)));
    };
    if (uintptr_t hit = mem::FindIf(sig, code, [&](uintptr_t a) { return isStock(a + sekiro::kFrameTickSigImm); })) {
        return hit + sekiro::kFrameTickSigImm;
    }
    if (uintptr_t hit = mem::FindIf(shortSig, code, [&](uintptr_t a) { return isStock(a + sekiro::kFrameTickSigShortImm); })) {
        return hit + sekiro::kFrameTickSigShortImm;
    }
    return 0;
}

uintptr_t FindSpeedFixDisp(const std::vector<mem::Range>& code, const mem::Range& image) {
    static const Pattern sig(sekiro::kSpeedFixSig);

    const uintptr_t hit = mem::FindIf(sig, code, [&](uintptr_t a) {
        const auto* p = reinterpret_cast<const uint8_t*>(a);
        if (!sekiro::IsRipRelativeModRm(p[sekiro::kSpeedFixSigModRm])) return false;
        const uintptr_t target = sekiro::RipTarget(a + sekiro::kSpeedFixSigDisp);
        return image.Contains(target, 4) && mem::IsReadable(target, 4) &&
               sekiro::IsPlausibleSpeedFix(ReadF32(reinterpret_cast<const uint8_t*>(target)));
    });
    return hit ? hit + sekiro::kSpeedFixSigDisp : 0;
}

// Highest refresh rate the primary monitor supports at its current resolution.
DWORD HighestRefreshAtDesktopResolution(const DEVMODEW& current) {
    DWORD best = current.dmDisplayFrequency;
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
        if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight &&
            mode.dmDisplayFrequency > best) {
            best = mode.dmDisplayFrequency;
        }
    }
    return best;
}

}  // namespace

int ResolveTargetFps(const Config& config) {
    if (config.targetFps > 0) return sekiro::ClampFps(config.targetFps);

    DEVMODEW current = {};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) return 60;
    // 0 and 1 mean "hardware default".
    DWORD hz = current.dmDisplayFrequency > 1 ? current.dmDisplayFrequency : 60;
    // Upscaling keeps the game borderless, which always runs at the desktop's refresh rate.
    if (config.forceHighestRefreshRate && !config.upscaling) hz = HighestRefreshAtDesktopResolution(current);
    return sekiro::ClampFps(static_cast<int>(hz));
}

void Run(const Config& config) {
    const int fps = ResolveTargetFps(config);
    if (fps == 60) {
        LOG("Frame rate: target resolves to 60 FPS (60 Hz monitor?), nothing to unlock. "
            "Set TargetFps together with DisableVSync=1 to go above your refresh rate.");
        return;
    }

    const float frameTick = sekiro::FrameTickForFps(fps);
    const float speedFix = sekiro::SpeedFixForFps(fps);
    const mem::Range image = mem::ModuleRange(GetModuleHandleW(nullptr));
    LOG("Frame rate: target %d FPS (frame tick %.6f s, speed fix %.4f)", fps, frameTick, speedFix);

    // The exe is DRM-wrapped and only decrypted after startup, so keep scanning until the
    // code shows up.
    uintptr_t frameTickImm = 0;
    uintptr_t speedDisp = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(config.scanTimeoutSeconds) * 1000;
    for (;;) {
        const std::vector<mem::Range> code = mem::CodeRegions(image);
        if (!frameTickImm) frameTickImm = FindFrameTick(code, image);
        if (!speedDisp) speedDisp = FindSpeedFixDisp(code, image);
        if (frameTickImm && speedDisp) break;
        if (GetTickCount64() > deadline) {
            LOG("Frame rate: gave up after %d s (frame tick %s, speed fix %s). Unsupported game version, or "
                "another tool already patched these.",
                config.scanTimeoutSeconds, frameTickImm ? "found" : "NOT found", speedDisp ? "found" : "NOT found");
            return;
        }
        Sleep(250);
    }
    LOG("Frame rate: frame tick at %p, speed fix operand at %p (stock value %.4f)",
        reinterpret_cast<void*>(frameTickImm), reinterpret_cast<void*>(speedDisp),
        ReadF32(reinterpret_cast<const uint8_t*>(sekiro::RipTarget(speedDisp))));

    // The speed fix operand is shared table data, so point this one instruction at our own copy
    // instead of editing the table.
    auto* cave = static_cast<float*>(mem::AllocNear(speedDisp, sizeof(float)));
    if (!cave) {
        LOG("Frame rate: no free memory within reach of the speed fix operand; not unlocking.");
        return;
    }
    *cave = speedFix;
    const intptr_t rel = reinterpret_cast<intptr_t>(cave) - static_cast<intptr_t>(speedDisp + 4);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LOG("Frame rate: speed fix memory out of rel32 range; not unlocking.");
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    const int32_t newDisp = static_cast<int32_t>(rel);
    int32_t oldDisp;
    std::memcpy(&oldDisp, reinterpret_cast<const void*>(speedDisp), sizeof(oldDisp));

    // Speed fix first: never run above 60 FPS with the 60 FPS movement constant.
    if (!mem::Write(speedDisp, &newDisp, sizeof(newDisp))) {
        LOG("Frame rate: failed to write speed fix; not unlocking.");
        return;
    }
    if (!mem::Write(frameTickImm, &frameTick, sizeof(frameTick))) {
        mem::Write(speedDisp, &oldDisp, sizeof(oldDisp));
        LOG("Frame rate: failed to write frame tick; reverted.");
        return;
    }
    LOG("Frame rate: 60 FPS cap removed, now capped at %d FPS.", fps);

    // Re-apply if anything (DRM unpacking, integrity checks) restores the original bytes.
    unsigned restores = 0;
    for (;;) {
        Sleep(2000);
        const bool tickOk = std::memcmp(reinterpret_cast<const void*>(frameTickImm), &frameTick, 4) == 0;
        const bool speedOk = std::memcmp(reinterpret_cast<const void*>(speedDisp), &newDisp, 4) == 0;
        if (tickOk && speedOk) continue;
        if (!speedOk) mem::Write(speedDisp, &newDisp, sizeof(newDisp));
        if (!tickOk) mem::Write(frameTickImm, &frameTick, sizeof(frameTick));
        if (++restores <= 5) LOG("Frame rate: patch was reverted by the game, re-applied (%u).", restores);
    }
}

}  // namespace sp::fps
