#pragma once

#include <cstdint>

namespace world_invert
{
    constexpr uint32_t kPluginVersion = 701; // 0.7.1
    constexpr char kPluginName[] = "wxl-world-invert";

    constexpr float MirrorHorizontalU(float value)
    {
        return 1.0f - value;
    }

    // Compiled at runtime for ps_2_0; keep this separate so the non-client test can validate it.
    constexpr char kPixelShaderSource[] = R"HLSL(
sampler Source : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(Source, float2(1.0 - uv.x, uv.y));
}
)HLSL";
}
