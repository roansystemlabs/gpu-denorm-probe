// binary32 denormal probe. Synthetic HLSL.
// Compile: dxc -T ps_6_2 -E main [-denorm any|ftz|preserve]
// Every operand comes from the constant buffer, so nothing can fold at compile time.
// Results are raw words on an R32G32B32A32_UINT target (lossless readback).
// SPDX-License-Identifier: MIT
cbuffer Constants : register(b0) { uint4 params[16]; };

uint4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const float least = asfloat(params[0].x);      // 0x00000001: least subnormal
    const float largest = asfloat(params[0].y);    // 0x007fffff: largest subnormal
    const float neg_zero = asfloat(params[0].z);   // 0x80000000
    const float min_normal = asfloat(params[0].w); // 0x00800000: least normal
    const float one_half = asfloat(params[1].x);   // 0x3f000000
    const float add_a = asfloat(params[1].y);      // 0x00800001: normal
    const float add_b = asfloat(params[1].z);      // 0x80800000: normal
    return uint4(asuint(add_a + add_b),             // REQUIRED: normal + normal -> 0x00000001 under preserve
                 asuint(min(least, largest)),       // supplementary: 0x00000001
                 asuint(max(least, neg_zero)),      // supplementary: 0x00000001
                 asuint(min_normal * one_half));    // supplementary: 0x00400000
}
