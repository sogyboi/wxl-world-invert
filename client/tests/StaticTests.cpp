#include <windows.h>

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#include "RuntimeCompat.hpp"
#include "WorldInvertShared.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/World.hpp"

namespace
{
    int g_failures = 0;

    void Expect(bool value, const char* description)
    {
        if (!value) {
            std::fprintf(stderr, "FAILED: %s\n", description);
            ++g_failures;
        }
    }

    pD3DCompile ResolveD3DCompile()
    {
        HMODULE module = GetModuleHandleA("d3dcompiler_47.dll");
        if (module == nullptr) module = LoadLibraryA("d3dcompiler_47.dll");
        return module != nullptr
            ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"))
            : nullptr;
    }
}

int main()
{
    Expect(world_invert::kPluginVersion == 805 &&
               std::strcmp(world_invert::kPluginVersionText, "0.8.5") == 0,
           "menu version text matches the binary plugin version");
    Expect(world_invert::MirrorHorizontalCoordinate(-1.0f, 1.0f, -0.25f) == 0.25f &&
               world_invert::MirrorHorizontalCoordinate(20.0f, 120.0f, 35.0f) == 105.0f,
           "screen-coordinate mirroring reflects within the native viewport interval");
    Expect(world_invert::MirrorHorizontalU(0.0f) == 1.0f &&
               world_invert::MirrorHorizontalU(0.25f) == 0.75f &&
               world_invert::MirrorHorizontalU(1.0f) == 0.0f,
           "scene compositor reflects texture U exactly once");
    Expect(std::strstr(world_invert::kPixelShaderSource, "tex2D(Source, uv)") != nullptr,
           "scene compositor shader preserves the viewport-bounded source coordinates");
    const pD3DCompile compile = ResolveD3DCompile();
    Expect(compile != nullptr, "D3DCompile is present for runtime pixel-shader creation");
    if (compile != nullptr) {
        ID3DBlob* byteCode = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT result = compile(world_invert::kPixelShaderSource,
                                       std::strlen(world_invert::kPixelShaderSource),
                                       nullptr, nullptr, nullptr, "main", "ps_2_0",
                                       D3DCOMPILE_ENABLE_STRICTNESS, 0, &byteCode, &errors);
        if (errors != nullptr) errors->Release();
        Expect(SUCCEEDED(result) && byteCode != nullptr && byteCode->GetBufferSize() > 0,
               "viewport compositor HLSL compiles as ps_2_0");
        if (byteCode != nullptr) byteCode->Release();
    }
    Expect(world_invert::MirrorHorizontalClipFlags(0x01) == 0x04 &&
               world_invert::MirrorHorizontalClipFlags(0x04) == 0x01 &&
               world_invert::MirrorHorizontalClipFlags(0x0A) == 0x0A,
           "world-label clipping swaps left and right without changing vertical bits");

    // The locally deployed f222923 WXL runtime predates SDK-439's inserted scene event. Subscribe
    // through this narrow compatibility map rather than the newer SDK enum after that insertion.
    Expect(world_invert::runtime::kDeviceLost == 7 &&
               world_invert::runtime::kDeviceReset == 8 &&
               world_invert::runtime::kWorldRenderEnd == 11 &&
               world_invert::runtime::kWorldEnter == 32 &&
               world_invert::runtime::kWorldLeave == 33 &&
               world_invert::runtime::kWorldOverlayRender == 0x007E7490,
           "deployed WXL lifecycle event numbers remain pinned");
    Expect(wxl::offsets::engine::gx::kWorldOnRender == 0x004F8EA0 &&
               wxl::offsets::game::world::kGetScreenCoordinates == 0x004F6D20 &&
               wxl::offsets::game::world::kPickAtScreen == 0x004F9DA0 &&
               wxl::offsets::game::world::kWorldFrame == 0x00B7436C,
           "world-render, world-label, and world-picking landmarks remain pinned to build 12340");
    Expect(wxl::offsets::engine::gx::kGxDevicePtr == 0x00C5DF88 &&
               wxl::offsets::engine::gx::kD3DDeviceField == 0x397C,
           "active D3D9 device landmarks remain pinned to build 12340");

    if (g_failures == 0) {
        std::puts("wxl-world-invert static tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
