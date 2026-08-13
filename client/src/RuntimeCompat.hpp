#pragma once

#include <cstdint>

// The locally deployed WarcraftXL f222923 runtime predates the SDK-439
// OnWorldSceneEnd insertion. Its ABI value is still 1.1, but events after
// OnWorldRenderEnd have shifted by one. Keep the compatibility surface narrow:
// these are the only runtime event IDs this extension consumes.
namespace world_invert::runtime
{
    inline constexpr uint32_t kDeviceLost    = 7;
    inline constexpr uint32_t kDeviceReset   = 8;
    inline constexpr uint32_t kWorldRenderEnd = 11;
    inline constexpr uint32_t kWorldEnter    = 32;
    inline constexpr uint32_t kWorldLeave    = 33;

    // Build-12340's CWorldFrame render-finalizer calls this no-argument world-overlay batch
    // immediately after the native 3D scene pass and before it restores the camera matrices.
    // The v0.8.4 compositor uses that narrow seam so label glyphs can draw after the scene image
    // has been reflected. This address is guarded by an exact prologue before it is hooked.
    inline constexpr uintptr_t kWorldOverlayRender = 0x007E7490;
}
