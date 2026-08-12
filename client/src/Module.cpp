#include <windows.h>

#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <new>

#include "WorldInvertShared.hpp"
#include "game/Binding.hpp"
#include "offsets/engine/Lua.hpp"
#include "offsets/game/World.hpp"
#include "wxl/EventScript.hpp"
#include "wxl/PluginApi.h"

namespace
{
    using world_invert::kPluginName;

    class Runtime final : public wxl::ext::EventScript
    {
    public:
        explicit Runtime(const WXL_Api* api) : m_api(api)
        {
            s_instance = this;
            on<&Runtime::OnWorldEnter>(wxl::events::Event::OnWorldEnter);
            on<&Runtime::OnWorldLeave>(wxl::events::Event::OnWorldLeave);
            on<&Runtime::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
            on<&Runtime::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
            on<&Runtime::OnWorldRenderEnd>(wxl::events::Event::OnWorldRenderEnd);
        }

        void DrawPanel()
        {
            if (m_api == nullptr) return;
            if (m_api->UiText != nullptr) {
                m_api->UiText("Mirrors the completed 3D world left-to-right before FrameXML draws.");
            }
            if (m_api->UiCheckbox != nullptr) {
                if (m_api->UiCheckbox("Mirror 3D world horizontally", &m_enabled) != 0) {
                    RequestCharacterBindingMirror(m_enabled != 0);
                }
            }
            if (m_api->UiText != nullptr) {
                m_api->UiText(m_inWorld ? "Scope: current world only; UI is unchanged."
                                     : "Waiting for an in-world render pass; login scenes are unchanged.");
                if (m_inWorld) {
                    m_api->UiText(s_worldPickHookAttached
                                      ? "A/D, camera yaw, and NPC/world picking are mirrored; UI clicks stay normal."
                                      : "World picking hook unavailable; visual mirror is still safe.");
                }
            }
        }

        void OnWorldEnter(const wxl::events::WorldEnterArgs&)
        {
            m_inWorld = true;
            m_bindingRequestSent = false;
            m_failureLogged = false;
        }

        void OnWorldLeave(const wxl::events::WorldLeaveArgs&)
        {
            RequestCharacterBindingMirror(false);
            m_bindingRequestSent = false;
            m_inWorld = false;
            ReleaseResources();
        }

        void OnDeviceLost(const wxl::events::DeviceResetArgs&)
        {
            ReleaseResources();
        }

        void OnDeviceReset(const wxl::events::DeviceResetArgs&)
        {
            // Create resources lazily on the first eligible world frame after reset.
            m_failureLogged = false;
            m_resourcesUnavailable = false;
        }

        void OnWorldRenderEnd(const wxl::events::WorldRenderEndArgs& args)
        {
            // The core's initial login path can reach an active world without emitting
            // OnWorldEnter after extensions have subscribed.  This callback is the
            // authoritative world-to-UI boundary, so seeing it is sufficient proof that
            // the client is rendering a live world and is safe to use as the gate.
            if (args.device == nullptr) return;
            m_inWorld = true;
            if (!m_bindingRequestSent) {
                RequestCharacterBindingMirror(m_enabled != 0);
            }
            if (m_enabled == 0) return;
            if (!Apply(static_cast<IDirect3DDevice9*>(args.device))) {
                LogFailureOnce();
            }
        }

        static void __cdecl DrawPanelThunk(void* user)
        {
            if (user != nullptr) {
                static_cast<Runtime*>(user)->DrawPanel();
            }
        }

    private:
        using PickAtScreenHookFn = int(__fastcall*)(void* worldFrame, void* unusedEdx,
                                                     float ddcX, float ddcY, int mode, void* result12);

        static int __fastcall PickAtScreenDetour(void* worldFrame, void* unusedEdx,
                                                  float ddcX, float ddcY, int mode, void* result12)
        {
            if (s_originalPickAtScreen == nullptr) return 0;
            Runtime* const runtime = s_instance;
            if (runtime != nullptr && runtime->ShouldMirrorWorldPick(worldFrame, ddcX, mode)) {
                ddcX = runtime->MirrorWorldPickX(ddcX);
            }
            return s_originalPickAtScreen(worldFrame, unusedEdx, ddcX, ddcY, mode, result12);
        }

        bool ShouldMirrorWorldPick(void* worldFrame, float ddcX, int mode) const
        {
            using namespace wxl::offsets::game::world;
            if (m_enabled == 0 || !m_inWorld || mode != kPickModeCursor || !std::isfinite(ddcX)) {
                return false;
            }
            void* const activeWorldFrame = *reinterpret_cast<void**>(kWorldFrame);
            if (worldFrame == nullptr || worldFrame != activeWorldFrame) return false;

            // SetupDefaultAction reaches the native world hit test only when this active input belongs
            // to the world frame. FrameXML input follows its own path and never has this ownership.
            void* const input = *reinterpret_cast<void**>(reinterpret_cast<char*>(worldFrame) + kWorldFrameInput);
            return input != nullptr &&
                *reinterpret_cast<void**>(reinterpret_cast<char*>(input) + 0x78) == worldFrame;
        }

        static float MirrorWorldPickX(float ddcX)
        {
            const float width = *reinterpret_cast<float*>(wxl::offsets::game::world::kDdcWidth);
            return std::isfinite(width) && width > 0.0f ? width - ddcX : ddcX;
        }

    public:
        static bool AttachWorldPickHook(const WXL_Api* api)
        {
            using namespace wxl::offsets::game::world;
            if (api == nullptr || api->HookAttach == nullptr || s_worldPickHookAttached) return false;

            // Fail closed if this is not the build-12340 HitTestPoint prologue the manifest claims.
            // The WXL core also rejects client-build mismatches before WXL_Load can run.
            constexpr unsigned char kExpectedPrologue[] = {
                0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x98, 0x00, 0x00,
                0x00, 0x56, 0x8B, 0xF1,
            };
            const auto* const target = reinterpret_cast<const unsigned char*>(kPickAtScreen);
            if (std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) return false;

            s_worldPickHookAttached = api->HookAttach(
                "WorldMirror.PickAtScreen", kPickAtScreen,
                reinterpret_cast<void*>(&Runtime::PickAtScreenDetour),
                reinterpret_cast<void**>(&s_originalPickAtScreen), WXL_HOOK_DEFAULT_PRIORITY) != 0;
            return s_worldPickHookAttached;
        }

    private:
        bool RequestCharacterBindingMirror(bool enabled)
        {
            // These are fixed, local FrameScript snippets.  They declare the desired state for the
            // companion addon and ask it to apply that state if it has already loaded.  The addon
            // also observes the declaration at PLAYER_LOGIN, so startup timing cannot drop it.
            const char* source = enabled
                ? "WorldMirrorRequested=true; if WorldMirrorControls then WorldMirrorControls:ApplyRequested() end"
                : "WorldMirrorRequested=false; if WorldMirrorControls then WorldMirrorControls:ApplyRequested() end";
            using namespace wxl::offsets::engine::lua;
            void* state = wxl::game::Native<FrameScriptGetContextFn>(kFrameScriptGetContext)();
            if (state == nullptr) return false;
            wxl::game::Native<FrameScriptExecuteFn>(kFrameScriptExecute)(source, state);
            m_bindingRequestSent = true;
            return true;
        }

        static pD3DCompile ResolveD3DCompile()
        {
            static const pD3DCompile compile = [] {
                HMODULE module = GetModuleHandleA("d3dcompiler_47.dll");
                if (module == nullptr) module = LoadLibraryA("d3dcompiler_47.dll");
                return module != nullptr
                    ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"))
                    : nullptr;
            }();
            return compile;
        }

        void LogFailureOnce()
        {
            if (m_failureLogged || m_api == nullptr || m_api->Log == nullptr) return;
            m_failureLogged = true;
            m_api->Log(WXL_LOG_WARN, kPluginName,
                       "effect skipped: D3D9 copy, shader, or state capture was unavailable; frame left unchanged");
        }

        void ReleaseResources()
        {
            if (m_scratchSurface != nullptr) {
                m_scratchSurface->Release();
                m_scratchSurface = nullptr;
            }
            if (m_scratchTexture != nullptr) {
                m_scratchTexture->Release();
                m_scratchTexture = nullptr;
            }
            if (m_pixelShader != nullptr) {
                m_pixelShader->Release();
                m_pixelShader = nullptr;
            }
            m_width = 0;
            m_height = 0;
            m_format = D3DFMT_UNKNOWN;
        }

        bool EnsureResources(IDirect3DDevice9* device, const D3DSURFACE_DESC& backBuffer)
        {
            if (m_resourcesUnavailable) return false;
            const bool targetChanged = m_width != backBuffer.Width || m_height != backBuffer.Height ||
                m_format != backBuffer.Format;
            if (targetChanged) ReleaseResources();

            if (m_scratchTexture == nullptr) {
                if (FAILED(device->CreateTexture(backBuffer.Width, backBuffer.Height, 1,
                                                 D3DUSAGE_RENDERTARGET, backBuffer.Format,
                                                 D3DPOOL_DEFAULT, &m_scratchTexture, nullptr)) ||
                    m_scratchTexture == nullptr ||
                    FAILED(m_scratchTexture->GetSurfaceLevel(0, &m_scratchSurface)) ||
                    m_scratchSurface == nullptr) {
                    ReleaseResources();
                    m_resourcesUnavailable = true;
                    return false;
                }
                m_width = backBuffer.Width;
                m_height = backBuffer.Height;
                m_format = backBuffer.Format;
            }

            if (m_pixelShader != nullptr) return true;

            const pD3DCompile compile = ResolveD3DCompile();
            if (compile == nullptr) {
                m_resourcesUnavailable = true;
                return false;
            }

            ID3DBlob* byteCode = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT compileResult = compile(world_invert::kPixelShaderSource,
                                                  std::strlen(world_invert::kPixelShaderSource),
                                                  nullptr, nullptr, nullptr, "main", "ps_2_0",
                                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, &byteCode, &errors);
            if (errors != nullptr) errors->Release();
            if (FAILED(compileResult) || byteCode == nullptr) {
                m_resourcesUnavailable = true;
                return false;
            }

            const HRESULT createResult = device->CreatePixelShader(
                static_cast<const DWORD*>(byteCode->GetBufferPointer()), &m_pixelShader);
            byteCode->Release();
            if (FAILED(createResult) || m_pixelShader == nullptr) {
                ReleaseResources();
                m_resourcesUnavailable = true;
                return false;
            }
            return true;
        }

        bool Apply(IDirect3DDevice9* device)
        {
            IDirect3DSurface9* backBuffer = nullptr;
            if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) ||
                backBuffer == nullptr) {
                return false;
            }

            D3DSURFACE_DESC description{};
            const HRESULT descriptionResult = backBuffer->GetDesc(&description);
            if (FAILED(descriptionResult) || !EnsureResources(device, description)) {
                backBuffer->Release();
                return false;
            }

            IDirect3DStateBlock9* savedState = nullptr;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &savedState)) || savedState == nullptr ||
                FAILED(savedState->Capture())) {
                if (savedState != nullptr) savedState->Release();
                backBuffer->Release();
                return false;
            }

            const HRESULT copyResult = device->StretchRect(backBuffer, nullptr, m_scratchSurface, nullptr,
                                                            D3DTEXF_NONE);
            HRESULT drawResult = E_FAIL;
            if (SUCCEEDED(copyResult)) {
                D3DVIEWPORT9 viewport{};
                if (SUCCEEDED(device->GetViewport(&viewport))) {
                    const float width = static_cast<float>(viewport.Width);
                    const float height = static_cast<float>(viewport.Height);
                    struct Vertex { float x, y, z, rhw, u, v; };
                    const Vertex quad[4] = {
                        { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
                        { width - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
                        { -0.5f, height - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
                        { width - 0.5f, height - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
                    };

                    device->SetRenderState(D3DRS_ZENABLE, FALSE);
                    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA |
                                                                   D3DCOLORWRITEENABLE_RED |
                                                                   D3DCOLORWRITEENABLE_GREEN |
                                                                   D3DCOLORWRITEENABLE_BLUE);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                    device->SetVertexShader(nullptr);
                    device->SetPixelShader(m_pixelShader);
                    device->SetTexture(0, m_scratchTexture);
                    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
                    drawResult = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
                }
            }

            savedState->Apply();
            savedState->Release();
            backBuffer->Release();
            return SUCCEEDED(drawResult);
        }

        inline static Runtime* s_instance = nullptr;
        inline static PickAtScreenHookFn s_originalPickAtScreen = nullptr;
        inline static bool s_worldPickHookAttached = false;

        const WXL_Api* m_api = nullptr;
        int m_enabled = 0;
        bool m_inWorld = false;
        bool m_bindingRequestSent = false;
        bool m_failureLogged = false;
        bool m_resourcesUnavailable = false;
        IDirect3DTexture9* m_scratchTexture = nullptr;
        IDirect3DSurface9* m_scratchSurface = nullptr;
        IDirect3DPixelShader9* m_pixelShader = nullptr;
        UINT m_width = 0;
        UINT m_height = 0;
        D3DFORMAT m_format = D3DFMT_UNKNOWN;
    };

    Runtime* g_runtime = nullptr;

    const WXL_PluginInfo kPluginInfo{
        sizeof(WXL_PluginInfo), WXL_API_VERSION, kPluginName, world_invert::kPluginVersion,
        WXL_CLIENT_BUILD,
    };
}

const WXL_PluginInfo* __cdecl WXL_Query()
{
    return &kPluginInfo;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (api == nullptr || api->structSize < sizeof(WXL_Api) || api->apiVersion != WXL_API_VERSION ||
        api->Subscribe == nullptr || api->UiAddPanel == nullptr || api->UiCheckbox == nullptr ||
        api->UiText == nullptr || api->HookAttach == nullptr) {
        return 0;
    }

    wxl::ext::EventScript::Bind(api);
    g_runtime = new (std::nothrow) Runtime(api);
    if (g_runtime == nullptr) {
        if (api->Log != nullptr) {
            api->Log(WXL_LOG_ERROR, kPluginName, "disabled: unable to allocate resident runtime state");
        }
        return 0;
    }
    if (!Runtime::AttachWorldPickHook(api) && api->Log != nullptr) {
        api->Log(WXL_LOG_WARN, kPluginName,
                 "world picking unavailable: build guard or native hook attachment failed");
    }
    api->UiAddPanel("World Mirror", &Runtime::DrawPanelThunk, g_runtime);
    if (api->Log != nullptr) {
        api->Log(WXL_LOG_INFO, kPluginName,
                 "loaded: default-off world-only horizontal mirror; open the World Mirror panel to enable it");
    }
    return 1;
}
