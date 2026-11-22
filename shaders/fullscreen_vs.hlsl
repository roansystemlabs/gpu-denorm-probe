// Full-screen triangle; the probes only need one pixel shader invocation per pixel.
// Compile: dxc -T vs_6_0 -E main
// SPDX-License-Identifier: MIT

struct Output {
    float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};

Output main(uint id : SV_VertexID) {
    Output output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
