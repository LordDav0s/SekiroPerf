// Tests for the CPU-side FSR constant setup.

#include <cmath>
#include <cstring>

#include "check.h"
#include "fsr_constants.h"

using namespace sp::fsr;

static float F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

static bool Near(float a, float b) { return std::fabs(a - b) <= 1e-6f * (1.0f + std::fabs(b)); }

static void TestEasu() {
    // 1920x1080 -> 2560x1440, the example in ffx_fsr1.h's documentation.
    Constants c = {};
    SetEasu(c, 1920, 1080, 2560, 1440);
    CHECK(Near(F(c.easu0[0]), 0.75f));
    CHECK(Near(F(c.easu0[1]), 0.75f));
    CHECK(Near(F(c.easu0[2]), -0.125f));  // 0.5 * 0.75 - 0.5
    CHECK(Near(F(c.easu0[3]), -0.125f));
    CHECK(Near(F(c.easu1[0]), 1.0f / 1920));
    CHECK(Near(F(c.easu1[1]), 1.0f / 1080));
    CHECK(Near(F(c.easu1[2]), 1.0f / 1920));
    CHECK(Near(F(c.easu1[3]), -1.0f / 1080));
    CHECK(Near(F(c.easu2[0]), -1.0f / 1920));
    CHECK(Near(F(c.easu2[1]), 2.0f / 1080));
    CHECK(Near(F(c.easu2[2]), 1.0f / 1920));
    CHECK(Near(F(c.easu2[3]), 2.0f / 1080));
    CHECK(F(c.easu3[0]) == 0.0f);
    CHECK(Near(F(c.easu3[1]), 4.0f / 1080));
    CHECK(c.easu3[2] == 0 && c.easu3[3] == 0);

    // Output pixel 0 maps to input position -0.125 (pixel centres line up).
    const float pp0 = 0.0f * F(c.easu0[0]) + F(c.easu0[2]);
    CHECK(Near(pp0, -0.125f));
    // Last output pixel centre (2559.5 * 0.75 = 1919.625) lies just past the last texel centre.
    const float ppLast = 2559.0f * F(c.easu0[0]) + F(c.easu0[2]);
    CHECK(Near(ppLast, 1919.125f));
}

static void TestRcas() {
    Constants c = {};
    SetRcas(c, 0.0f, {320, 0, 1920, 1080});
    CHECK(F(c.rcas[0]) == 1.0f);  // 0 stops = full strength
    CHECK(c.rcasRect[0] == 320 && c.rcasRect[1] == 0);
    CHECK(c.rcasRect[2] == 1919 && c.rcasRect[3] == 1079);

    SetRcas(c, 1.0f, {0, 0, 10, 10});
    CHECK(Near(F(c.rcas[0]), 0.5f));  // one stop halves it

    CHECK(SharpnessToStops(1.0f) == 0.0f);
    CHECK(SharpnessToStops(0.0f) == 2.0f);
    CHECK(Near(SharpnessToStops(0.8f), 0.4f));
    CHECK(SharpnessToStops(-3.0f) == 2.0f);
    CHECK(SharpnessToStops(7.0f) == 0.0f);
}

static void TestFitAspect() {
    Rect r = FitAspect(1920, 1080, 2560, 1440);  // same aspect
    CHECK(r.x == 0 && r.y == 0 && r.w == 2560 && r.h == 1440);

    r = FitAspect(1920, 1080, 2560, 1080);  // 16:9 game on 21:9: pillarbox
    CHECK(r.w == 1920 && r.h == 1080 && r.x == 320 && r.y == 0);

    r = FitAspect(1920, 1080, 1920, 1200);  // 16:9 game on 16:10: letterbox
    CHECK(r.w == 1920 && r.h == 1080 && r.x == 0 && r.y == 60);

    r = FitAspect(1366, 768, 1920, 1080);  // "16:9" that is 0.06% off: fill, no 1 px bars
    CHECK(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);

    r = FitAspect(1280, 720, 3840, 2160);
    CHECK(r.x == 0 && r.y == 0 && r.w == 3840 && r.h == 2160);
}

void RunFsrTests() {
    TestEasu();
    TestRcas();
    TestFitAspect();
}
