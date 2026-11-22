// binary16 denormal probe. Synthetic HLSL.
// Compile: dxc -enable-16bit-types -T ps_6_2 -E main [-denorm any|ftz|preserve]
// Every operand comes from the constant buffer, so nothing can fold at compile time.
// Results are raw words on an R32G32B32A32_UINT target (lossless readback).
// SPDX-License-Identifier: MIT
cbuffer Constants : register(b0) { uint4 params[16]; };

half from_bits(uint word) { return asfloat16((uint16_t)word); }
uint to_bits(half value) { return (uint)asuint16(value); }

uint4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    const half least = from_bits(params[0].x);      // 0x0001: least subnormal
    const half largest = from_bits(params[0].y);    // 0x03ff: largest subnormal
    const half neg_zero = from_bits(params[0].z);   // 0x8000
    const half min_normal = from_bits(params[0].w); // 0x0400: least normal
    const half one_half = from_bits(params[1].x);   // 0x3800
    const float f32_tiny = asfloat(params[1].y);    // 0x33800000 = 2^-24 (normal in f32)
    const half add_a = from_bits(params[1].z);      // 0x0401: normal
    const half add_b = from_bits(params[1].w);      // 0x8400: normal

    const uint added = to_bits(add_a + add_b);            // REQUIRED: normal + normal -> 0x0001 under preserve
    const uint minimum = to_bits(min(least, largest));    // supplementary: 0x0001
    const uint maximum = to_bits(max(least, neg_zero));   // supplementary: 0x0001
    const uint produced = to_bits(min_normal * one_half); // supplementary: 0x0200
    const uint widened = asuint((float)least);            // f16 -> f32: 0x33800000 if kept
    const uint narrowed = to_bits((half)f32_tiny);        // f32 -> f16: 0x0001 if kept
    const uint moved = to_bits(least);                    // move-only control: 0x0001 in every mode

    return uint4(added | minimum << 16, maximum | produced << 16, widened, narrowed | moved << 16);
}
