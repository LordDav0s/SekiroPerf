// Tests for the signature scanner and the Sekiro-specific patch math.

#include <cstring>
#include <initializer_list>
#include <vector>

#include "check.h"
#include "pattern.h"
#include "sekiro_sigs.h"

using namespace sp;

static void Append(std::vector<uint8_t>& buf, std::initializer_list<uint8_t> bytes) {
    buf.insert(buf.end(), bytes.begin(), bytes.end());
}

static void AppendF32(std::vector<uint8_t>& buf, float v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    buf.insert(buf.end(), b, b + 4);
}

static void TestPatternBasics() {
    const uint8_t data[] = {0x00, 0xAA, 0xBB, 0xCC, 0xAA, 0x11, 0xCC, 0xAA};
    Pattern p("AA ?? CC");
    CHECK(p.size() == 3);
    CHECK(p.Find(data, sizeof(data)) == 1);
    CHECK(p.Find(data, sizeof(data), 2) == 4);
    CHECK(p.Find(data, sizeof(data), 5) == Pattern::npos);  // trailing AA has no room

    Pattern single("? BB");  // single '?' wildcard, anchor not at index 0
    CHECK(single.Find(data, sizeof(data)) == 1);

    Pattern atEnd("CC AA");
    CHECK(atEnd.Find(data, sizeof(data)) == 3);
    CHECK(atEnd.Find(data, sizeof(data), 4) == 6);  // match ending on the last byte

    Pattern lower("aa bb");
    CHECK(lower.Find(data, sizeof(data)) == 1);

    CHECK(Pattern("AA BZ").Find(data, sizeof(data)) == Pattern::npos);  // malformed
    CHECK(Pattern("").Find(data, sizeof(data)) == Pattern::npos);
    CHECK(Pattern("?? ??").Find(data, sizeof(data)) == Pattern::npos);  // no anchor
    CHECK(Pattern("AA BB CC DD EE FF 00 11 22").Find(data, sizeof(data)) == Pattern::npos);
}

static void TestFrameTick() {
    std::vector<uint8_t> code;
    Append(code, {0x90, 0x90});
    // Decoy: same instruction shape, but the immediate is 1.0f, not 1/60.
    Append(code, {0xC7, 0x43, 0x10, 0x00, 0x00, 0x80, 0x3F, 0x4C, 0x89, 0xAB, 0x00, 0x01, 0x00, 0x00});
    const size_t real = code.size();
    // Real site, bytes from the reference disassembly (mov [rbx+18h], 3C888889h; mov [rbx+270h], r13).
    Append(code, {0xC7, 0x43, 0x18, 0x89, 0x88, 0x88, 0x3C, 0x4C, 0x89, 0xAB, 0x70, 0x02, 0x00, 0x00});
    Append(code, {0xC3});

    Pattern sig(sekiro::kFrameTickSig);
    const size_t hit = sig.FindIf(code.data(), code.size(), [&](size_t off) {
        return sekiro::IsStockFrameTick(sekiro::ReadF32(code.data() + off + sekiro::kFrameTickSigImm));
    });
    CHECK(hit == real);

    Pattern shortSig(sekiro::kFrameTickSigShort);
    const size_t shortHit = shortSig.Find(code.data(), code.size());
    CHECK(shortHit != Pattern::npos);
    CHECK(shortHit + sekiro::kFrameTickSigShortImm == real + sekiro::kFrameTickSigImm);

    // Float rounding variants of 1/60 seen across builds, and things that are not 1/60.
    const uint32_t stock[] = {0x3C888888, 0x3C888889, 0x3C888890};
    for (uint32_t bits : stock) {
        float f;
        std::memcpy(&f, &bits, 4);
        CHECK(sekiro::IsStockFrameTick(f));
    }
    CHECK(!sekiro::IsStockFrameTick(1.0f / 30.0f));
    CHECK(!sekiro::IsStockFrameTick(1.0f / 144.0f));
}

static void TestSpeedFix() {
    std::vector<uint8_t> code;
    Append(code, {0xCC, 0xCC, 0xCC});
    const size_t site = code.size();
    Append(code, {0xF3, 0x0F, 0x58, 0xD0, 0x0F, 0xC6, 0xD2, 0x00, 0x0F, 0x51, 0xC2,
                  0xF3, 0x0F, 0x59, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x2F, 0xF8});
    Append(code, {0xCC, 0xCC, 0xCC, 0xCC, 0xCC});
    const size_t value = code.size();
    AppendF32(code, 30.0f);

    // Point the disp32 at the float, relative to the end of the mulss instruction.
    const size_t disp = site + sekiro::kSpeedFixSigDisp;
    const int32_t rel = static_cast<int32_t>(value - (disp + 4));
    std::memcpy(code.data() + disp, &rel, 4);

    Pattern sig(sekiro::kSpeedFixSig);
    CHECK(sig.Find(code.data(), code.size()) == site);
    CHECK(sekiro::IsRipRelativeModRm(code[site + sekiro::kSpeedFixSigModRm]));

    const uintptr_t target = sekiro::RipTarget(reinterpret_cast<uintptr_t>(code.data() + disp));
    CHECK(target == reinterpret_cast<uintptr_t>(code.data() + value));
    CHECK(sekiro::ReadF32(reinterpret_cast<const uint8_t*>(target)) == 30.0f);

    CHECK(sekiro::IsRipRelativeModRm(0x05));   // xmm0, [rip]
    CHECK(sekiro::IsRipRelativeModRm(0x0D));   // xmm1, [rip]
    CHECK(sekiro::IsRipRelativeModRm(0x3D));   // xmm7, [rip]
    CHECK(!sekiro::IsRipRelativeModRm(0x44));  // SIB + disp8
    CHECK(!sekiro::IsRipRelativeModRm(0xC1));  // register operand

    // Negative displacement (target before the instruction).
    std::vector<uint8_t> back;
    AppendF32(back, 50.0f);
    Append(back, {0x00, 0x00, 0x00, 0x00});
    const int32_t negRel = -8;  // disp at 4, end at 8, target at 0
    std::memcpy(back.data() + 4, &negRel, 4);
    CHECK(sekiro::RipTarget(reinterpret_cast<uintptr_t>(back.data() + 4)) ==
          reinterpret_cast<uintptr_t>(back.data()));
}

static void TestFpsMath() {
    // Matches the reference tool's FindSpeedFixForRefreshRate.
    CHECK(sekiro::SpeedFixForFps(60) == 30.0f);
    CHECK(sekiro::SpeedFixForFps(30) == 15.0f);
    CHECK(sekiro::SpeedFixForFps(100) == 50.0f);
    CHECK(sekiro::SpeedFixForFps(120) == 60.0f);
    CHECK(sekiro::SpeedFixForFps(144) == 67.0f);   // ideal 72: 67 is 5 away, 78.84 is 6.84
    CHECK(sekiro::SpeedFixForFps(165) == 84.0f);   // ideal 82.5
    CHECK(sekiro::SpeedFixForFps(240) == 120.0f);
    CHECK(sekiro::SpeedFixForFps(300) == 150.0f);

    CHECK(sekiro::ClampFps(10) == 30);
    CHECK(sekiro::ClampFps(144) == 144);
    CHECK(sekiro::ClampFps(1000) == 300);

    CHECK(sekiro::IsStockFrameTick(sekiro::FrameTickForFps(60)));
    CHECK(sekiro::FrameTickForFps(144) == 1.0f / 144.0f);

    for (float v : sekiro::kSpeedFixTable) CHECK(sekiro::IsPlausibleSpeedFix(v));
}

void RunSigTests() {
    TestPatternBasics();
    TestFrameTick();
    TestSpeedFix();
    TestFpsMath();
}
