#pragma once

#include <cstdint>

namespace world_invert
{
    constexpr uint32_t kPluginVersion = 805; // 0.8.5
    constexpr char kPluginName[] = "wxl-world-invert";
    constexpr char kPluginVersionText[] = "0.8.5";
    constexpr char kPluginVersionLine[] = "Version: 0.8.5 | WoW 3.3.5 build 12340";

    // A world-space screen coordinate lives in an arbitrary viewport interval rather than a fixed
    // [0, 1] range. Reflect it about that interval's centre without touching the glyph geometry
    // subsequently drawn at the returned position.
    constexpr float MirrorHorizontalCoordinate(float left, float right, float value)
    {
        return left + right - value;
    }

    // Texture-space reflection used by the single pre-overlay compositor. It is deliberately
    // separate from the world-label coordinate reflection above: glyph geometry remains normal.
    constexpr float MirrorHorizontalU(float value)
    {
        return 1.0f - value;
    }

    // Compiled at runtime for ps_2_0. The scene compositor is deliberately a single opaque
    // screen-space pass: the native 3D renderer remains responsible for every world draw and its
    // CPU-side visibility/culling state. The vertex quad supplies reversed U coordinates, which
    // preserves a correctly bounded reflection even when the active viewport is letterboxed.
    constexpr char kPixelShaderSource[] = R"HLSL(
sampler Source : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(Source, uv);
}
)HLSL";

    // Native CGWorldFrame clip bits: left=1, top=2, right=4, bottom=8. Reflection swaps only the
    // horizontal edges, which keeps off-screen labels and indicator culling aligned with the mirror.
    constexpr uint32_t MirrorHorizontalClipFlags(uint32_t flags)
    {
        constexpr uint32_t kLeft = 0x01;
        constexpr uint32_t kRight = 0x04;
        return (flags & ~(kLeft | kRight)) |
            ((flags & kLeft) << 2) |
            ((flags & kRight) >> 2);
    }
}
