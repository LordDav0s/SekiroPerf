#pragma once

// Sekiro-specific signatures and the math around them. Portable so it can be unit tested.
//
// The byte signatures and the speed-fix table come from uberhalit's SekiroFpsUnlockAndMore
// (MIT, see THIRD_PARTY_NOTICES.md), which tracked them across sekiro.exe 1.2.0.0 - 1.5.0.0.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sp::sekiro {

// fFrameTick, the frame time limit, is stored as an instruction immediate:
//   C7 43 18 89 88 88 3C    mov dword ptr [rbx+18h], 3C888889h   ; 1/60 s
//   4C 89 AB 70 02 00 00    mov [rbx+270h], r13
inline constexpr const char* kFrameTickSig = "C7 43 ?? ?? ?? ?? ?? 4C 89 AB";
inline constexpr ptrdiff_t kFrameTickSigImm = 3;
// Fallback: the immediate's upper bytes plus the following instruction. The immediate's
// lowest byte varies between builds (88/89/90) due to float rounding.
inline constexpr const char* kFrameTickSigShort = "88 88 3C 4C 89 AB";
inline constexpr ptrdiff_t kFrameTickSigShortImm = -1;

// Movement speed math reads a frame-rate-dependent constant through a RIP-relative operand.
// Left alone, running speed is wrong above 60 FPS.
//   F3 0F 58 D0             addss  xmm2, xmm0
//   0F C6 D2 00             shufps xmm2, xmm2, 0
//   0F 51 C2                sqrtps xmm0, xmm2
//   F3 0F 59 05 xx xx xx xx mulss  xmm0, [rip+disp32]   ; 30.0 at 60 FPS
//   0F 2F F8                comiss xmm7, xmm0
inline constexpr const char* kSpeedFixSig =
    "F3 0F 58 ?? 0F C6 ?? 00 0F 51 ?? F3 0F 59 ?? ?? ?? ?? ?? 0F 2F";
inline constexpr ptrdiff_t kSpeedFixSigModRm = 14;
inline constexpr ptrdiff_t kSpeedFixSigDisp = 15;

// Same bounds as the reference tool.
inline constexpr int kMinFps = 30;
inline constexpr int kMaxFps = 300;

// Values the game's speed table holds; the right one is closest to half the frame rate.
inline constexpr float kSpeedFixTable[] = {
    15.0f,  16.0f,  16.6667f, 18.0f,  18.6875f, 18.8516f, 20.0f,    24.0f,  25.0f,
    27.5f,  30.0f,  32.0f,    38.5f,  40.0f,    48.0f,    49.5f,    50.0f,  57.2958f,
    60.0f,  64.0f,  66.75f,   67.0f,  78.8438f, 80.0f,    84.0f,    90.0f,  93.8f,
    100.0f, 120.0f, 127.0f,   128.0f, 130.0f,   140.0f,   144.0f,   150.0f,
};
inline constexpr float kSpeedFixDefault = 30.0f;

inline float ReadF32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The stock immediate is 1/60 s; builds differ by a few ULPs.
inline bool IsStockFrameTick(float v) { return v > 0.01666f && v < 0.01667f; }

inline bool IsPlausibleSpeedFix(float v) { return v >= 15.0f && v <= 150.0f; }

// ModRM with mod=00, rm=101 encodes [rip+disp32].
inline bool IsRipRelativeModRm(uint8_t modrm) { return (modrm & 0xC7) == 0x05; }

// Absolute address referenced by a disp32 that ends its instruction (no immediate follows).
inline uintptr_t RipTarget(uintptr_t dispAddress) {
    int32_t rel;
    std::memcpy(&rel, reinterpret_cast<const void*>(dispAddress), sizeof(rel));
    return dispAddress + sizeof(rel) + static_cast<intptr_t>(rel);
}

inline int ClampFps(int fps) { return fps < kMinFps ? kMinFps : fps > kMaxFps ? kMaxFps : fps; }

inline float FrameTickForFps(int fps) { return 1.0f / static_cast<float>(fps); }

inline float SpeedFixForFps(int fps) {
    const float ideal = static_cast<float>(fps) / 2.0f;
    float best = kSpeedFixDefault;
    for (float v : kSpeedFixTable) {
        if (std::fabs(ideal - v) < std::fabs(ideal - best)) best = v;
    }
    return best;
}

}  // namespace sp::sekiro
