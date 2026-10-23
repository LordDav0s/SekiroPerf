#pragma once

// CPU-side setup for the FSR 1.0 shaders. Portable so it can be unit tested.
// FsrEasuCon / FsrRcasCon are ported from AMD's ffx_fsr1.h (MIT, see THIRD_PARTY_NOTICES.md).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace sp::fsr {

// Must match the cbuffer in fsr_shaders.h.
struct Constants {
    uint32_t easu0[4];
    uint32_t easu1[4];
    uint32_t easu2[4];
    uint32_t easu3[4];
    uint32_t rcas[4];     // x: sharpness as float bits
    int32_t rcasRect[4];  // xy: destination offset in the back buffer, zw: destination size - 1
};
static_assert(sizeof(Constants) == 96, "constant buffer layout");

struct Rect {
    int x, y, w, h;
};

inline uint32_t FloatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// FsrEasuCon for an input viewport that covers the whole input texture.
inline void SetEasu(Constants& c, float inW, float inH, float outW, float outH) {
    c.easu0[0] = FloatBits(inW * (1.0f / outW));
    c.easu0[1] = FloatBits(inH * (1.0f / outH));
    c.easu0[2] = FloatBits(0.5f * inW * (1.0f / outW) - 0.5f);
    c.easu0[3] = FloatBits(0.5f * inH * (1.0f / outH) - 0.5f);
    c.easu1[0] = FloatBits(1.0f / inW);
    c.easu1[1] = FloatBits(1.0f / inH);
    c.easu1[2] = FloatBits(1.0f * (1.0f / inW));
    c.easu1[3] = FloatBits(-1.0f * (1.0f / inH));
    c.easu2[0] = FloatBits(-1.0f * (1.0f / inW));
    c.easu2[1] = FloatBits(2.0f * (1.0f / inH));
    c.easu2[2] = FloatBits(1.0f * (1.0f / inW));
    c.easu2[3] = FloatBits(2.0f * (1.0f / inH));
    c.easu3[0] = FloatBits(0.0f);
    c.easu3[1] = FloatBits(4.0f * (1.0f / inH));
    c.easu3[2] = 0;
    c.easu3[3] = 0;
}

// FsrRcasCon. `stops` halves the sharpening per stop; 0 is the strongest.
inline void SetRcas(Constants& c, float stops, const Rect& dst) {
    c.rcas[0] = FloatBits(std::exp2(-stops));
    c.rcas[1] = c.rcas[2] = c.rcas[3] = 0;
    c.rcasRect[0] = dst.x;
    c.rcasRect[1] = dst.y;
    c.rcasRect[2] = dst.w - 1;
    c.rcasRect[3] = dst.h - 1;
}

// User-facing sharpness 0..1 to FSR stops: 1 -> 0 stops (max), 0 -> 2 stops (subtle).
inline float SharpnessToStops(float sharpness) {
    if (sharpness < 0.0f) sharpness = 0.0f;
    if (sharpness > 1.0f) sharpness = 1.0f;
    return (1.0f - sharpness) * 2.0f;
}

// Largest rect with the input's aspect ratio, centred in the output. Aspect ratios within 1%
// (e.g. 1366x768 vs 16:9) fill the output instead of leaving 1-2 px bars.
inline Rect FitAspect(int inW, int inH, int outW, int outH) {
    const long long a = static_cast<long long>(inW) * outH;  // input aspect, cross-multiplied
    const long long b = static_cast<long long>(outW) * inH;  // output aspect
    if (std::llabs(a - b) * 100 <= b) return {0, 0, outW, outH};
    if (a > b) {  // input is wider: full width, bars top and bottom
        const int h = static_cast<int>((static_cast<long long>(outW) * inH * 2 + inW) / (2LL * inW));
        return {0, (outH - h) / 2, outW, h};
    }
    const int w = static_cast<int>((static_cast<long long>(outH) * inW * 2 + inH) / (2LL * inH));
    return {(outW - w) / 2, 0, w, outH};
}

}  // namespace sp::fsr
