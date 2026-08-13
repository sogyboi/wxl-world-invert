#include <windows.h>

#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "RuntimeCompat.hpp"
#include "WorldInvertShared.hpp"
#include "game/Binding.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Lua.hpp"
#include "offsets/game/World.hpp"
#include "wxl/PluginApi.h"

namespace
{
    using world_invert::kPluginName;
    namespace gx = wxl::offsets::engine::gx;
    namespace world = wxl::offsets::game::world;

    // The CWorldFrame rectangle CGWorldFrame::GetScreenCoordinates uses for its output. The native
    // routine compares X to +0x68/+0x70 and Y to +0x64/+0x6C; only the horizontal interval belongs
    // to this effect.
    constexpr size_t kWorldScreenLeft = 0x68;
    constexpr size_t kWorldScreenRight = 0x70;

    class Runtime final
    {
    public:
        explicit Runtime(const WXL_Api* api) : m_api(api)
        {
            s_instance = this;
            SubscribeRuntimeEvents();
        }

        void DrawPanel()
        {
            if (m_api == nullptr) return;
            if (m_api->UiText != nullptr) {
                m_api->UiText(world_invert::kPluginVersionLine);
                m_api->UiText("Mirrors the 3D scene before the native world-overlay draw; UI and label glyphs stay readable.");
            }
            if (m_api->UiCheckbox != nullptr &&
                m_api->UiCheckbox("Mirror 3D world horizontally", &m_enabled) != 0 && m_inWorld) {
                // Turning off is immediate. Turning on waits for one successful scene compositor
                // pass, so the visible world and movement/click controls never disagree.
                if (m_enabled == 0) {
                    SynchronizeCharacterBindingMirror(false);
                }
            }
            if (m_api->UiText == nullptr) return;

            if (!m_inWorld) {
                m_api->UiText("Waiting for a live world; login and character-select scenes are unchanged.");
            } else if (!RendererReady()) {
                m_api->UiText(RendererReadinessReason());
            } else if (m_enabled != 0 && m_sceneMirrorFailed) {
                m_api->UiText("Scene compositor could not copy this render target; the visual mirror is safely held off.");
            } else if (m_enabled != 0 && !m_sceneMirrorAppliedThisFrame) {
                m_api->UiText("Waiting for the native world-overlay pass; the visual mirror is safely held off.");
            } else if (s_worldPickHookAttached) {
                m_api->UiText("3D scene, world labels, A/D, camera yaw, and NPC/world picks stay aligned; UI stays normal.");
            } else {
                m_api->UiText("3D scene and labels stay aligned; the native world-picking hook is unavailable.");
            }
        }

        void OnWorldEnter()
        {
            m_inWorld = IsLiveWorld();
            m_bindingRequestSent = false;
            m_controlsMirrored = false;
            m_sceneMirrorAppliedThisFrame = false;
            m_sceneMirrorPending = false;
            m_pendingWorldFrame = nullptr;
            m_sceneMirrorFailed = false;
            m_compositorFailureLogged = false;
        }

        void OnWorldLeave()
        {
            SynchronizeCharacterBindingMirror(false);
            // The client may have already torn down FrameScript during logout. The companion addon
            // also restores itself on PLAYER_LOGOUT, so leave our internal state fail-closed.
            m_bindingRequestSent = false;
            m_controlsMirrored = false;
            m_inWorld = false;
            m_sceneMirrorAppliedThisFrame = false;
            m_sceneMirrorPending = false;
            m_pendingWorldFrame = nullptr;
            m_sceneMirrorFailed = false;
            ReleaseResources();
        }

        void OnDeviceLost()
        {
            m_sceneMirrorAppliedThisFrame = false;
            m_sceneMirrorPending = false;
            m_pendingWorldFrame = nullptr;
            ReleaseResources();
        }

        void OnDeviceReset()
        {
            // Create default-pool resources lazily in the next eligible world scene.
            m_sceneMirrorAppliedThisFrame = false;
            m_sceneMirrorPending = false;
            m_pendingWorldFrame = nullptr;
            m_sceneMirrorFailed = false;
            m_compositorFailureLogged = false;
            ReleaseResources();
        }

        void OnWorldRenderEnd()
        {
            // The old local WXL runtime publishes this event for glue scenes as well. Require a
            // live CWorldFrame and a loaded map before activating either the compositor or controls.
            const bool liveWorld = IsLiveWorld();
            if (!liveWorld && m_inWorld) {
                SynchronizeCharacterBindingMirror(false);
                m_bindingRequestSent = false;
                m_controlsMirrored = false;
                m_sceneMirrorAppliedThisFrame = false;
                m_sceneMirrorPending = false;
                m_pendingWorldFrame = nullptr;
                ReleaseResources();
            }
            m_inWorld = liveWorld;
            if (!m_inWorld) return;

            // This runs after the scene and its labels. It applies control changes for the next input
            // pass only after the just-rendered visual scene proved it could be mirrored.
            SynchronizeCharacterBindingMirror(
                m_enabled != 0 && m_sceneMirrorAppliedThisFrame);
        }

        static void __cdecl DrawPanelThunk(void* user)
        {
            if (user != nullptr) {
                static_cast<Runtime*>(user)->DrawPanel();
            }
        }

        static bool AttachHooks(const WXL_Api* api)
        {
            const bool worldRender = AttachWorldRenderHook(api);
            const bool worldOverlay = AttachWorldOverlayHook(api);
            const bool worldCoordinates = AttachWorldScreenCoordinatesHook(api);
            return worldRender && worldOverlay && worldCoordinates;
        }

        static bool AttachWorldPickHook(const WXL_Api* api)
        {
            if (api == nullptr || api->HookAttach == nullptr || s_worldPickHookAttached) return false;

            // Fail closed if this is not the build-12340 HitTestPoint prologue the manifest claims.
            constexpr unsigned char kExpectedPrologue[] = {
                0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x98, 0x00, 0x00,
                0x00, 0x56, 0x8B, 0xF1,
            };
            if (!HasExpectedPrologue(world::kPickAtScreen, kExpectedPrologue)) return false;

            s_worldPickHookAttached = api->HookAttach(
                "WorldMirror.PickAtScreen", world::kPickAtScreen,
                reinterpret_cast<void*>(&Runtime::PickAtScreenDetour),
                reinterpret_cast<void**>(&s_originalPickAtScreen), WXL_HOOK_DEFAULT_PRIORITY) != 0;
            return s_worldPickHookAttached;
        }

    private:
        using WorldOnRenderHookFn = void(__fastcall*)(void* worldFrame, void* unusedEdx);
        using WorldOverlayHookFn = void(__cdecl*)();
        using GetScreenCoordinatesHookFn = int(__fastcall*)(void* worldFrame, void* unusedEdx,
                                                              const float* worldPos, float* outScreen,
                                                              uint32_t* clipFlags);
        using PickAtScreenHookFn = int(__fastcall*)(void* worldFrame, void* unusedEdx,
                                                     float ddcX, float ddcY, int mode, void* result12);
        using FrameScriptExecute3Fn = void(__cdecl*)(const char* source, const char* chunkName,
                                                      void* parentContext);

        template <size_t N>
        static bool HasExpectedPrologue(uintptr_t target, const unsigned char (&expected)[N])
        {
            return std::memcmp(reinterpret_cast<const void*>(target), expected, N) == 0;
        }

        static bool IsLiveWorld()
        {
            const void* const worldFrame = *reinterpret_cast<void* const*>(world::kWorldFrame);
            const int32_t mapId = *reinterpret_cast<const int32_t*>(world::kCurrentMapId);
            return worldFrame != nullptr && mapId >= 0;
        }

        static void* ActiveWorldFrame()
        {
            return *reinterpret_cast<void**>(world::kWorldFrame);
        }

        static IDirect3DDevice9* ActiveD3DDevice()
        {
            void* const gxDevice = *reinterpret_cast<void**>(gx::kGxDevicePtr);
            if (gxDevice == nullptr) return nullptr;
            return *reinterpret_cast<IDirect3DDevice9**>(
                static_cast<unsigned char*>(gxDevice) + gx::kD3DDeviceField);
        }

        bool RendererReady() const
        {
            return s_worldRenderHookAttached && s_worldOverlayHookAttached &&
                s_worldScreenCoordinatesHookAttached;
        }

        static const char* RendererReadinessReason()
        {
            if (!s_worldRenderHookAttached)
                return "Renderer hook unavailable: native world-scene chain did not attach.";
            if (!s_worldOverlayHookAttached)
                return "Renderer hook unavailable: native world-overlay chain did not attach.";
            if (!s_worldScreenCoordinatesHookAttached)
                return "Renderer hook unavailable: world-label coordinate chain did not attach.";
            return "Renderer hooks are ready.";
        }

        bool ShouldMirrorWorldScene(void* worldFrame) const
        {
            return m_enabled != 0 && m_inWorld && RendererReady() &&
                worldFrame != nullptr && worldFrame == ActiveWorldFrame();
        }

        bool ShouldMirrorWorldCoordinates(void* worldFrame) const
        {
            return m_enabled != 0 && m_inWorld && m_sceneMirrorAppliedThisFrame &&
                worldFrame != nullptr && worldFrame == ActiveWorldFrame();
        }

        bool ShouldMirrorWorldOverlay() const
        {
            return m_sceneMirrorPending && m_enabled != 0 && m_inWorld && RendererReady() &&
                m_pendingWorldFrame != nullptr && m_pendingWorldFrame == ActiveWorldFrame();
        }

        bool MirrorWorldScreenX(void* worldFrame, float* outScreen, uint32_t* clipFlags) const
        {
            if (worldFrame == nullptr || outScreen == nullptr || !std::isfinite(outScreen[0])) return false;
            const auto* const frame = static_cast<const unsigned char*>(worldFrame);
            const float left = *reinterpret_cast<const float*>(frame + kWorldScreenLeft);
            const float right = *reinterpret_cast<const float*>(frame + kWorldScreenRight);
            if (!std::isfinite(left) || !std::isfinite(right) || right <= left) return false;

            outScreen[0] = world_invert::MirrorHorizontalCoordinate(left, right, outScreen[0]);
            if (clipFlags != nullptr) {
                *clipFlags = world_invert::MirrorHorizontalClipFlags(*clipFlags);
            }
            return true;
        }

        bool ShouldMirrorWorldPick(void* worldFrame, float ddcX, int mode) const
        {
            if (m_enabled == 0 || !m_inWorld || !m_sceneMirrorAppliedThisFrame ||
                mode != world::kPickModeCursor || !std::isfinite(ddcX)) {
                return false;
            }
            if (worldFrame == nullptr || worldFrame != ActiveWorldFrame()) return false;

            // SetupDefaultAction reaches the native world hit test only when this active input belongs
            // to the world frame. FrameXML input follows its own path and never has this ownership.
            void* const input = *reinterpret_cast<void**>(
                static_cast<unsigned char*>(worldFrame) + world::kWorldFrameInput);
            return input != nullptr &&
                *reinterpret_cast<void**>(static_cast<unsigned char*>(input) + 0x78) == worldFrame;
        }

        static float MirrorWorldPickX(float ddcX)
        {
            const float width = *reinterpret_cast<float*>(world::kDdcWidth);
            return std::isfinite(width) && width > 0.0f ? width - ddcX : ddcX;
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
            m_targetWidth = 0;
            m_targetHeight = 0;
            m_targetFormat = D3DFMT_UNKNOWN;
        }

        bool EnsureResources(IDirect3DDevice9* device, const D3DSURFACE_DESC& target)
        {
            const bool targetChanged = m_targetWidth != target.Width ||
                m_targetHeight != target.Height || m_targetFormat != target.Format;
            if (targetChanged) ReleaseResources();

            if (m_scratchTexture == nullptr) {
                if (FAILED(device->CreateTexture(target.Width, target.Height, 1,
                                                 D3DUSAGE_RENDERTARGET, target.Format,
                                                 D3DPOOL_DEFAULT, &m_scratchTexture, nullptr)) ||
                    m_scratchTexture == nullptr ||
                    FAILED(m_scratchTexture->GetSurfaceLevel(0, &m_scratchSurface)) ||
                    m_scratchSurface == nullptr) {
                    ReleaseResources();
                    return false;
                }
                m_targetWidth = target.Width;
                m_targetHeight = target.Height;
                m_targetFormat = target.Format;
            }

            if (m_pixelShader != nullptr) return true;

            const pD3DCompile compile = ResolveD3DCompile();
            if (compile == nullptr) return false;

            ID3DBlob* byteCode = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT compileResult = compile(world_invert::kPixelShaderSource,
                                                  std::strlen(world_invert::kPixelShaderSource),
                                                  nullptr, nullptr, nullptr, "main", "ps_2_0",
                                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, &byteCode, &errors);
            if (errors != nullptr) errors->Release();
            if (FAILED(compileResult) || byteCode == nullptr) {
                if (byteCode != nullptr) byteCode->Release();
                return false;
            }

            const HRESULT createResult = device->CreatePixelShader(
                static_cast<const DWORD*>(byteCode->GetBufferPointer()), &m_pixelShader);
            byteCode->Release();
            if (FAILED(createResult) || m_pixelShader == nullptr) {
                ReleaseResources();
                return false;
            }
            return true;
        }

        bool ApplySceneMirror(IDirect3DDevice9* device)
        {
            if (device == nullptr) return false;

            IDirect3DSurface9* target = nullptr;
            if (FAILED(device->GetRenderTarget(0, &target)) || target == nullptr) return false;

            D3DSURFACE_DESC targetDescription{};
            D3DVIEWPORT9 viewport{};
            const bool validTarget = SUCCEEDED(target->GetDesc(&targetDescription)) &&
                SUCCEEDED(device->GetViewport(&viewport)) &&
                targetDescription.Format != D3DFMT_UNKNOWN &&
                targetDescription.MultiSampleType == D3DMULTISAMPLE_NONE &&
                viewport.Width != 0 && viewport.Height != 0 &&
                viewport.X <= targetDescription.Width && viewport.Y <= targetDescription.Height &&
                viewport.Width <= targetDescription.Width - viewport.X &&
                viewport.Height <= targetDescription.Height - viewport.Y;
            if (!validTarget || !EnsureResources(device, targetDescription)) {
                target->Release();
                return false;
            }

            IDirect3DStateBlock9* savedState = nullptr;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &savedState)) || savedState == nullptr ||
                FAILED(savedState->Capture())) {
                if (savedState != nullptr) savedState->Release();
                target->Release();
                return false;
            }

            const HRESULT copyResult = device->StretchRect(target, nullptr, m_scratchSurface, nullptr,
                                                            D3DTEXF_NONE);
            HRESULT drawResult = E_FAIL;
            if (SUCCEEDED(copyResult)) {
                const float left = static_cast<float>(viewport.X) - 0.5f;
                const float right = static_cast<float>(viewport.X + viewport.Width) - 0.5f;
                const float top = static_cast<float>(viewport.Y) - 0.5f;
                const float bottom = static_cast<float>(viewport.Y + viewport.Height) - 0.5f;
                const float uLeft = static_cast<float>(viewport.X) /
                    static_cast<float>(targetDescription.Width);
                const float uRight = static_cast<float>(viewport.X + viewport.Width) /
                    static_cast<float>(targetDescription.Width);
                const float vTop = static_cast<float>(viewport.Y) /
                    static_cast<float>(targetDescription.Height);
                const float vBottom = static_cast<float>(viewport.Y + viewport.Height) /
                    static_cast<float>(targetDescription.Height);
                struct Vertex { float x, y, z, rhw, u, v; };
                // Reverse U inside the active viewport, not across the entire render target.
                // This keeps a letterboxed/non-zero viewport as one contiguous reflected scene.
                const Vertex quad[4] = {
                    { left, top, 0.0f, 1.0f, uRight, vTop },
                    { right, top, 0.0f, 1.0f, uLeft, vTop },
                    { left, bottom, 0.0f, 1.0f, uRight, vBottom },
                    { right, bottom, 0.0f, 1.0f, uLeft, vBottom },
                };

                device->SetRenderState(D3DRS_ZENABLE, FALSE);
                device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
                device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
                device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
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

            savedState->Apply();
            savedState->Release();
            target->Release();
            return SUCCEEDED(drawResult);
        }

        void LogCompositorFailureOnce()
        {
            if (m_compositorFailureLogged || m_api == nullptr || m_api->Log == nullptr) return;
            m_compositorFailureLogged = true;
            m_api->Log(WXL_LOG_WARN, kPluginName,
                       "scene compositor skipped: active D3D9 render target could not be copied; frame left unchanged");
        }

        bool RequestCharacterBindingMirror(bool enabled)
        {
            // Fixed local FrameScript snippets declare desired state for the companion addon. The
            // deployed build's executor takes source, chunk name, and parent context; the SDK's
            // two-argument declaration is not safe for this exact f222923 runtime/client pairing.
            const char* source = enabled
                ? "WorldMirrorRequested=true; if WorldMirrorControls then WorldMirrorControls:ApplyRequested() end"
                : "WorldMirrorRequested=false; if WorldMirrorControls then WorldMirrorControls:ApplyRequested() end";
            using namespace wxl::offsets::engine::lua;
            if (wxl::game::Native<FrameScriptGetContextFn>(kFrameScriptGetContext)() == nullptr) {
                return false;
            }
            wxl::game::Native<FrameScriptExecute3Fn>(kFrameScriptExecute)(
                source, "wxl-world-invert", nullptr);
            m_bindingRequestSent = true;
            return true;
        }

        void SynchronizeCharacterBindingMirror(bool enabled)
        {
            if (m_bindingRequestSent && m_controlsMirrored == enabled) return;
            if (RequestCharacterBindingMirror(enabled)) {
                m_controlsMirrored = enabled;
            }
        }

        void SubscribeRuntimeEvents()
        {
            if (m_api == nullptr || m_api->Subscribe == nullptr) return;
            // f222923 predates SDK-439's event-table insertion. These values are deliberately
            // pinned in RuntimeCompat.hpp rather than using the newer SDK enum at/after that point.
            m_api->Subscribe(world_invert::runtime::kDeviceLost, &Runtime::OnDeviceLostThunk, this);
            m_api->Subscribe(world_invert::runtime::kDeviceReset, &Runtime::OnDeviceResetThunk, this);
            m_api->Subscribe(world_invert::runtime::kWorldRenderEnd, &Runtime::OnWorldRenderEndThunk, this);
            m_api->Subscribe(world_invert::runtime::kWorldEnter, &Runtime::OnWorldEnterThunk, this);
            m_api->Subscribe(world_invert::runtime::kWorldLeave, &Runtime::OnWorldLeaveThunk, this);
        }

        static void __cdecl OnDeviceLostThunk(void* user, const void*)
        {
            if (user != nullptr) static_cast<Runtime*>(user)->OnDeviceLost();
        }

        static void __cdecl OnDeviceResetThunk(void* user, const void*)
        {
            if (user != nullptr) static_cast<Runtime*>(user)->OnDeviceReset();
        }

        static void __cdecl OnWorldRenderEndThunk(void* user, const void*)
        {
            if (user != nullptr) static_cast<Runtime*>(user)->OnWorldRenderEnd();
        }

        static void __cdecl OnWorldEnterThunk(void* user, const void*)
        {
            if (user != nullptr) static_cast<Runtime*>(user)->OnWorldEnter();
        }

        static void __cdecl OnWorldLeaveThunk(void* user, const void*)
        {
            if (user != nullptr) static_cast<Runtime*>(user)->OnWorldLeave();
        }

        static void __fastcall WorldOnRenderDetour(void* worldFrame, void* unusedEdx)
        {
            const WorldOnRenderHookFn original = s_originalWorldOnRender;
            if (original == nullptr) return;

            Runtime* const runtime = s_instance;
            const bool currentWorld = runtime != nullptr && worldFrame != nullptr &&
                worldFrame == ActiveWorldFrame();
            const bool outermostWorldPass = currentWorld && s_worldRenderScopeDepth++ == 0;
            if (outermostWorldPass) {
                runtime->m_sceneMirrorAppliedThisFrame = false;
                runtime->m_sceneMirrorPending = false;
                runtime->m_pendingWorldFrame = nullptr;
                if (runtime->m_enabled == 0) runtime->m_sceneMirrorFailed = false;
            }
            const bool mirrorThisPass = currentWorld && runtime->ShouldMirrorWorldScene(worldFrame);

            // This calls the WXL core's WorldScenePass link and then the native 3D scene renderer.
            // The next native call is the build-bound world-overlay batch. Arm a compositor for
            // that boundary instead of copying here: its labels then draw after the reflected scene
            // at reflected coordinates, so their glyphs remain normal-facing.
            original(worldFrame, unusedEdx);

            if (currentWorld && --s_worldRenderScopeDepth == 0) {
                runtime->m_sceneMirrorPending = mirrorThisPass;
                runtime->m_pendingWorldFrame = mirrorThisPass ? worldFrame : nullptr;
            }
        }

        static void __cdecl WorldOverlayDetour()
        {
            const WorldOverlayHookFn original = s_originalWorldOverlay;
            if (original == nullptr) return;

            Runtime* const runtime = s_instance;
            if (runtime != nullptr && runtime->m_sceneMirrorPending) {
                const bool shouldMirror = runtime->ShouldMirrorWorldOverlay();
                // Clear first so an unexpected nested path cannot reflect the same scene twice,
                // including after a world-frame replacement between the two native passes.
                runtime->m_sceneMirrorPending = false;
                runtime->m_pendingWorldFrame = nullptr;
                if (shouldMirror) {
                    const bool applied = runtime->ApplySceneMirror(ActiveD3DDevice());
                    runtime->m_sceneMirrorAppliedThisFrame = applied;
                    runtime->m_sceneMirrorFailed = !applied;
                    if (!applied) runtime->LogCompositorFailureOnce();
                }
            }
            original();
        }

        static int __fastcall GetScreenCoordinatesDetour(void* worldFrame, void* unusedEdx,
                                                           const float* worldPos, float* outScreen,
                                                           uint32_t* clipFlags)
        {
            const GetScreenCoordinatesHookFn original = s_originalGetScreenCoordinates;
            if (original == nullptr) return 0;
            const int result = original(worldFrame, unusedEdx, worldPos, outScreen, clipFlags);

            Runtime* const runtime = s_instance;
            if (runtime != nullptr && runtime->ShouldMirrorWorldCoordinates(worldFrame)) {
                runtime->MirrorWorldScreenX(worldFrame, outScreen, clipFlags);
            }
            return result;
        }

        static int __fastcall PickAtScreenDetour(void* worldFrame, void* unusedEdx,
                                                  float ddcX, float ddcY, int mode, void* result12)
        {
            if (s_originalPickAtScreen == nullptr) return 0;
            Runtime* const runtime = s_instance;
            if (runtime != nullptr && runtime->ShouldMirrorWorldPick(worldFrame, ddcX, mode)) {
                ddcX = MirrorWorldPickX(ddcX);
            }
            return s_originalPickAtScreen(worldFrame, unusedEdx, ddcX, ddcY, mode, result12);
        }

        static bool AttachWorldRenderHook(const WXL_Api* api)
        {
            if (api == nullptr || api->HookAttach == nullptr || s_worldRenderHookAttached) return false;

            // WarcraftXL owns a chain at this exact entry for WorldScenePass. Depending on startup
            // timing that chain may already have replaced the raw client prologue when extensions
            // load. HookAttach joins the core-managed chain safely, so a raw-byte check here would
            // reject the supported, already-hooked state.
            s_worldRenderHookAttached = api->HookAttach(
                "WorldMirror.WorldOnRender", gx::kWorldOnRender,
                reinterpret_cast<void*>(&Runtime::WorldOnRenderDetour),
                reinterpret_cast<void**>(&s_originalWorldOnRender), WXL_HOOK_DEFAULT_PRIORITY) != 0;
            return s_worldRenderHookAttached;
        }

        static bool AttachWorldOverlayHook(const WXL_Api* api)
        {
            if (api == nullptr || api->HookAttach == nullptr || s_worldOverlayHookAttached) return false;

            // `CGWorldFrame::RenderWorld` calls this build-12340 no-argument routine immediately
            // after WorldOnRender. It is deliberately guarded because attaching at the wrong text
            // batch would either leave labels reversed or risk mirroring a non-world overlay.
            constexpr unsigned char kExpectedPrologue[] = {
                0x55, 0x8B, 0xEC, 0xA1, 0xC8, 0x80, 0xD3, 0x00,
                0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00,
            };
            if (!HasExpectedPrologue(world_invert::runtime::kWorldOverlayRender,
                                     kExpectedPrologue)) return false;

            s_worldOverlayHookAttached = api->HookAttach(
                "WorldMirror.WorldOverlay", world_invert::runtime::kWorldOverlayRender,
                reinterpret_cast<void*>(&Runtime::WorldOverlayDetour),
                reinterpret_cast<void**>(&s_originalWorldOverlay), WXL_HOOK_DEFAULT_PRIORITY) != 0;
            return s_worldOverlayHookAttached;
        }

        static bool AttachWorldScreenCoordinatesHook(const WXL_Api* api)
        {
            if (api == nullptr || api->HookAttach == nullptr || s_worldScreenCoordinatesHookAttached) return false;
            constexpr unsigned char kExpectedPrologue[] = {
                0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x8B, 0x45,
                0x08, 0xD9, 0x00, 0x56, 0xD9, 0x55, 0xDC, 0x8B, 0xF1,
            };
            if (!HasExpectedPrologue(world::kGetScreenCoordinates, kExpectedPrologue)) return false;

            s_worldScreenCoordinatesHookAttached = api->HookAttach(
                "WorldMirror.GetScreenCoordinates", world::kGetScreenCoordinates,
                reinterpret_cast<void*>(&Runtime::GetScreenCoordinatesDetour),
                reinterpret_cast<void**>(&s_originalGetScreenCoordinates), WXL_HOOK_DEFAULT_PRIORITY) != 0;
            return s_worldScreenCoordinatesHookAttached;
        }

        inline static Runtime* s_instance = nullptr;
        inline static WorldOnRenderHookFn s_originalWorldOnRender = nullptr;
        inline static WorldOverlayHookFn s_originalWorldOverlay = nullptr;
        inline static GetScreenCoordinatesHookFn s_originalGetScreenCoordinates = nullptr;
        inline static PickAtScreenHookFn s_originalPickAtScreen = nullptr;
        inline static bool s_worldRenderHookAttached = false;
        inline static bool s_worldOverlayHookAttached = false;
        inline static bool s_worldScreenCoordinatesHookAttached = false;
        inline static bool s_worldPickHookAttached = false;
        inline static thread_local unsigned s_worldRenderScopeDepth = 0;

        const WXL_Api* m_api = nullptr;
        int m_enabled = 0;
        bool m_inWorld = false;
        bool m_bindingRequestSent = false;
        bool m_controlsMirrored = false;
        bool m_sceneMirrorAppliedThisFrame = false;
        bool m_sceneMirrorPending = false;
        void* m_pendingWorldFrame = nullptr;
        bool m_sceneMirrorFailed = false;
        bool m_compositorFailureLogged = false;
        IDirect3DTexture9* m_scratchTexture = nullptr;
        IDirect3DSurface9* m_scratchSurface = nullptr;
        IDirect3DPixelShader9* m_pixelShader = nullptr;
        UINT m_targetWidth = 0;
        UINT m_targetHeight = 0;
        D3DFORMAT m_targetFormat = D3DFMT_UNKNOWN;
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

    g_runtime = new (std::nothrow) Runtime(api);
    if (g_runtime == nullptr) {
        if (api->Log != nullptr) {
            api->Log(WXL_LOG_ERROR, kPluginName, "disabled: unable to allocate resident runtime state");
        }
        return 0;
    }
    if (!Runtime::AttachHooks(api) && api->Log != nullptr) {
        api->Log(WXL_LOG_WARN, kPluginName,
                 "3D-world renderer hooks unavailable: visual mirror will remain safely disabled");
    }
    if (!Runtime::AttachWorldPickHook(api) && api->Log != nullptr) {
        api->Log(WXL_LOG_WARN, kPluginName,
                 "world picking unavailable: build guard or native hook attachment failed");
    }
    api->UiAddPanel("World Mirror", &Runtime::DrawPanelThunk, g_runtime);
    if (api->Log != nullptr) {
        api->Log(WXL_LOG_INFO, kPluginName,
                 "loaded: default-off world-only mirror with readable world labels; open World Mirror to enable it");
    }
    return 1;
}
