# WXL World Mirror

`wxl-world-invert` is a visual-only WarcraftXL extension for WoW
3.3.5a build 12340. It mirrors the native 3D world pass left-to-right, so the map,
units, sky, particles, and world models are horizontally reflected while the game's UI,
cursor, addon frames, and world-overlay text remain normal-facing.

It does not alter MPQs, maps, collision, server state, or gameplay.

## Safety defaults

- The effect starts **off** every time the extension is loaded.
- The in-game **World Mirror** panel displays the exact extension version and the
  client build it targets, so a screenshot immediately shows whether a local client
  is running the expected release.
- It activates only for a verified live `CWorldFrame` and loaded map; a glue-screen
  `OnWorldRenderEnd` event cannot turn it on. Login and character-select 3D scenes
  are deliberately excluded.
- The native 3D renderer finishes normally, then one opaque screen-space compositor
  pass reflects its active scene target immediately before the build-bound native
  world-overlay batch. This
  avoids altering WoW's projection, frustum, visibility, culling, or terrain state.
  It also prevents a partially reflected scene from mixing with a normal pass.
- Native world-to-screen coordinates are reflected separately while that overlay draws, so world labels move
  with the mirrored unit but their glyphs are never rendered backwards. Horizontal
  clip flags are swapped with them.
- If the renderer landmarks, native hooks, or the active render target are unavailable,
  the visual effect is held off and a single warning is logged; the game frame is left
  untouched. Labels, world picking, and controls are held normal in the same frame.
- The included `WorldMirrorControls` addon temporarily swaps the player's current
  `A` and `D` character-movement commands and flips WoW's native `mouseInvertYaw`
  camera preference while the mirror is on. It applies to both left- and
  right-button camera drags. The addon restores the original session bindings and
  yaw preference when the mirror is off, on world leave, or at logout; it never
  calls `SaveBindings`, so the player's keybinding profile is untouched.
- The extension also reflects the native **world** hit-test ray, so hover, target,
  left-click, and right-click interaction agree with the mirrored NPCs and objects.
  FrameXML and overlay controls do not use that world hit-test path, so UI clicks
  remain in their normal physical locations.

## Build from source

The build is pinned to WXL ABI-1.1 commit `439d5a90235a8f38ea3ca3d91541f0cf0a626fe1`.
Point `WXL_SDK_ROOT` at that exact SDK checkout. The extension also pins the
locally deployed f222923 runtime's lifecycle-event IDs, because it predates the
SDK's later scene-event insertion. It produces files only under `client/.build`
and does not touch a WoW client profile.

```powershell
$env:WXL_SDK_ROOT = 'C:\path\to\wxl-sdk-v1.1'
.\client\build.ps1
```

The build runs static renderer-binding tests and an ABI probe.

## Package and install

After a successful build, package the extension:

```powershell
.\tools\Package-WorldInvert.ps1
```

With WoW closed, copy both packaged directories into the desired WXL client:

- `Extensions\wxl-world-invert` into `Extensions`
- `Interface\AddOns\WorldMirrorControls` into `Interface\AddOns`

Do not overwrite an existing extension or addon folder without first making a
backup. Start the client, make sure **World Mirror Controls** is enabled on the
character-selection AddOns screen, open the WXL overlay, select **World Mirror**,
and check **Mirror 3D world horizontally**.

The mirror toggle is the only control: it activates the visual mirror and the
temporary A/D binding swap together.

## Hub and release installation

The release ZIP contains both required client folders. A normal WXL Hub install
can deploy the native extension, but the Hub's extension deployer does not place
WoW addons under `Interface\AddOns`. Install `WorldMirrorControls` from the
same release ZIP manually so mirrored A/D movement, camera yaw, and world
picking stay aligned.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
