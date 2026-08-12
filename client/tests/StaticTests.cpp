#include <windows.h>

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#include "WorldInvertShared.hpp"
#include "engine/events/Event.hpp"
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
    Expect(world_invert::MirrorHorizontalU(0.0f) == 1.0f &&
               world_invert::MirrorHorizontalU(0.25f) == 0.75f &&
               world_invert::MirrorHorizontalU(1.0f) == 0.0f,
           "horizontal mirror complements only the U coordinate");
    // These IDs are the ABI-1.1 values verified against the active local WXL core.  The plugin
    // never relies on events appended after WorldLeave.
    Expect(static_cast<uint32_t>(wxl::events::Event::OnDeviceLost) == 7,
           "OnDeviceLost ABI event number remains pinned");
    Expect(static_cast<uint32_t>(wxl::events::Event::OnDeviceReset) == 8,
           "OnDeviceReset ABI event number remains pinned");
    Expect(static_cast<uint32_t>(wxl::events::Event::OnWorldRenderEnd) == 11,
           "OnWorldRenderEnd ABI event number remains pinned");
    Expect(static_cast<uint32_t>(wxl::events::Event::OnInput) == 17,
           "OnInput ABI event number remains pinned");
    Expect(static_cast<uint32_t>(wxl::events::Event::OnWorldEnter) == 32 &&
               static_cast<uint32_t>(wxl::events::Event::OnWorldLeave) == 33,
           "world lifecycle ABI event numbers remain pinned");
    Expect(wxl::offsets::game::world::kPickAtScreen == 0x004F9DA0 &&
               wxl::offsets::game::world::kWorldFrame == 0x00B7436C,
           "world-picking ABI landmarks remain pinned to client build 12340");

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
               "world-mirror HLSL compiles as ps_2_0");
        if (byteCode != nullptr) byteCode->Release();
    }

    if (g_failures == 0) {
        std::puts("wxl-world-invert static tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
