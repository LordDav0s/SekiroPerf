#pragma once

// AMD FidelityFX Super Resolution 1.0 (EASU upscale + RCAS sharpen), 32-bit float path,
// ported from ffx_fsr1.h / ffx_a.h into standalone Shader Model 5 HLSL.
// Copyright (c) 2021 Advanced Micro Devices, Inc. MIT license, see THIRD_PARTY_NOTICES.md.
//
// Kept to plain SM5 features so both Microsoft's d3dcompiler and Wine's (vkd3d) accept it.

namespace sp::fsr {

inline constexpr const char kShaderSource[] = R"hlsl(
cbuffer FsrConstants : register(b0)
{
    uint4 Easu0;
    uint4 Easu1;
    uint4 Easu2;
    uint4 Easu3;
    uint4 Rcas;      // x: sharpness (float bits)
    int4 RcasRect;   // xy: destination offset, zw: destination size - 1
};

Texture2D<float4> Source : register(t0);
SamplerState PointClamp : register(s0);

float4 FullscreenVS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// ffx_a.h approximations. These are bit tricks, so they must stay exactly as AMD wrote them.
float PrxLoRcp(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }
float PrxMedRcp(float a) { float b = asfloat(0x7ef19fffu - asuint(a)); return b * (-b * a + 2.0); }
float PrxLoRsq(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }
float Max3F(float x, float y, float z) { return max(x, max(y, z)); }
float3 Min3F3(float3 x, float3 y, float3 z) { return min(x, min(y, z)); }
float3 Max3F3(float3 x, float3 y, float3 z) { return max(x, max(y, z)); }

//------------------------------------------------------------------------------------------------
// EASU: edge adaptive spatial upsampling
//------------------------------------------------------------------------------------------------
void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    // Rotate offset by direction, apply anisotropy.
    float2 v;
    v.x = (off.x * ( dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    // Lanczos-2 approximation: (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

// Accumulate direction and edge length for one bilinear corner. Taps form a '+':
//     a
//   b c d
//     e
void EasuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = PrxLoRcp(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = PrxLoRcp(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float4 EasuPS(float4 position : SV_Position) : SV_Target
{
    // Position of 'f' in the input.
    float2 pp = floor(position.xy) * asfloat(Easu0.xy) + asfloat(Easu0.zw);
    float2 fp = floor(pp);
    pp -= fp;

    // 12-tap kernel, fetched as four gathers:
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float2 p0 = fp * asfloat(Easu1.xy) + asfloat(Easu1.zw);
    float2 p1 = p0 + asfloat(Easu2.xy);
    float2 p2 = p0 + asfloat(Easu2.zw);
    float2 p3 = p0 + asfloat(Easu3.xy);
    float4 bczzR = Source.GatherRed(PointClamp, p0);
    float4 bczzG = Source.GatherGreen(PointClamp, p0);
    float4 bczzB = Source.GatherBlue(PointClamp, p0);
    float4 ijfeR = Source.GatherRed(PointClamp, p1);
    float4 ijfeG = Source.GatherGreen(PointClamp, p1);
    float4 ijfeB = Source.GatherBlue(PointClamp, p1);
    float4 klhgR = Source.GatherRed(PointClamp, p2);
    float4 klhgG = Source.GatherGreen(PointClamp, p2);
    float4 klhgB = Source.GatherBlue(PointClamp, p2);
    float4 zzonR = Source.GatherRed(PointClamp, p3);
    float4 zzonG = Source.GatherGreen(PointClamp, p3);
    float4 zzonB = Source.GatherBlue(PointClamp, p3);

    // Approximate luma times 2.
    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;

    // Direction and length, bilinearly weighted over the four nearest texels f g j k.
    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    EasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    // Normalize with approximation, and clean up close to zero.
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = PrxLoRsq(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    // {0 to 2} -> {0 to 1}, shaped with a square.
    len = len * 0.5;
    len *= len;
    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
    float stretch = (dir.x * dir.x + dir.y * dir.y) * PrxLoRcp(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    // Window shifts from +/-{sqrt(2.0) to slightly beyond 2.0} with the amount of edge.
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = PrxLoRcp(lob);

    // Min/max of the 4 nearest, for deringing.
    float3 fC = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    float3 gC = float3(klhgR.w, klhgG.w, klhgB.w);
    float3 jC = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    float3 kC = float3(klhgR.x, klhgG.x, klhgB.x);
    float3 min4 = min(Min3F3(fC, gC, jC), kC);
    float3 max4 = max(Max3F3(fC, gC, jC), kC);

    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    EasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x)); // b
    EasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y)); // c
    EasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x)); // i
    EasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y)); // j
    EasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z)); // f
    EasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w)); // e
    EasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x)); // k
    EasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y)); // l
    EasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z)); // h
    EasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w)); // g
    EasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z)); // o
    EasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w)); // n

    // Normalize and dering.
    float3 color = min(max4, max(min4, aC * (1.0 / aW)));
    return float4(color, 1.0);
}

//------------------------------------------------------------------------------------------------
// RCAS: robust contrast adaptive sharpening
//------------------------------------------------------------------------------------------------
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float3 RcasLoad(int2 p)
{
    return Source.Load(int3(clamp(p, int2(0, 0), RcasRect.zw), 0)).rgb;
}

float4 RcasPS(float4 position : SV_Position) : SV_Target
{
    // 3x3 cross:
    //    b
    //  d e f
    //    h
    int2 sp = int2(position.xy) - RcasRect.xy;
    float3 b = RcasLoad(sp + int2( 0, -1));
    float3 d = RcasLoad(sp + int2(-1,  0));
    float3 e = RcasLoad(sp);
    float3 f = RcasLoad(sp + int2( 1,  0));
    float3 h = RcasLoad(sp + int2( 0,  1));

    // Min and max of ring, per channel.
    float3 mn4 = min(Min3F3(b, d, f), h);
    float3 mx4 = max(Max3F3(b, d, f), h);
    // Limiters (high precision reciprocals).
    float2 peakC = float2(1.0, -1.0 * 4.0);
    float3 hitMin = min(mn4, e) * (1.0 / (4.0 * mx4));
    float3 hitMax = (peakC.x - max(mx4, e)) * (1.0 / (4.0 * mn4 + peakC.y));
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(Max3F(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * asfloat(Rcas.x);
    // Resolve with the medium precision reciprocal to avoid tonality shifts.
    float rcpL = PrxMedRcp(4.0 * lobe + 1.0);
    float3 color = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    return float4(color, 1.0);
}
)hlsl";

}  // namespace sp::fsr
