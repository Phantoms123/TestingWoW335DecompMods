# CoAVolFog built into the client

Upstream CoAVolFog is a `version.dll` proxy that loads `CoAVolFog.dll` into the 32-bit 3.3.5a `Wow.exe`, wraps
Direct3D 9 through the client's `GetProcAddress` slot, rewrites call sites and vtable slots, and reads client state
at fixed addresses. In this tree the client calls it directly, so none of that is needed and it also runs in the
64-bit build. The renderers, shaders, data files and `CoAVolFog.ini` are upstream's, unchanged.

## What changed from upstream

| Upstream | Here |
|---|---|
| `proxy/` and `dllmain.cpp` (loader, `DllMain`) | Removed. `native_main.cpp` runs the same start-up from `VolFog_CreateDirect3D9`, reading the ini and data files from the client folder |
| `GetProcAddress` slot filter for `Direct3DCreate9` | `CGxDeviceD3d::ILoadD3dLib` passes its `Direct3DCreate9` to `VolFog_CreateDirect3D9` |
| Call-site patches and asm thunks in `hooks.cpp` | Calls from the client's source (table below); the `vf_on_*` handlers are upstream's |
| Water material vtable slots | `VolFogWaterDrawScope` at the top of `CMaterialWater::Render` and `CMaterialWaterNoSpec::Render` |
| Client addresses and 32-bit offsets in `engine*.cpp` | `client.h` / `client_native.cpp` read the same values through the client's own globals and types |
| Code-byte guards before patching | Removed; there is nothing to patch |
| `nvapi.dll` | `nvapi64.dll` in the 64-bit build |

## Where the client calls it

| Upstream patch site | Client source |
|---|---|
| World render call `0x4FB03D` | `CGWorldFrame::RenderWorld`, around `OnWorldRender` (`vf_on_frame_begin`/`end`) |
| After the opaque M2 pass `0x4F911D` | `CGWorldFrame::OnWorldRender`, after `scene->Draw(M2PASS_0)` |
| Liquid surface pass `0x4F9170` | `CGWorldFrame::OnWorldRender`, around the above-water `RenderLiquidPass` |
| Glare pass `0x4F9213` | `CGWorldFrame::OnWorldRender`, `vf_on_glare_pass` in place of `DayNight::RenderGlares` |
| FFX end `0x4F9281` | `CGWorldFrame::OnWorldRender`, before `FFX::Finish` |
| World name text `0x7E5818` | `PLAYERNAMEDESC::Render`, around `GxuFontRenderBatch(s_playerNameBatch)` |
| M2 batch fog `0x81FD15` | `CM2SceneRender::SetupLighting`, before `CShaderEffect::SetFogParams` |
| Water pass `0x790AA2` | `CWorldScene::RenderLiquidPass`, around `Flush(..., 1)` |
| Far clip clamp `0x780810`, `0x781444` | `World::SetFarClip`, `CWorld::LoadMap` |

## Client state it reads

| Upstream address | Client source |
|---|---|
| Gx view, projection, viewport depth | `GxXformView`, `CGxDevice::m_projection`, `CGxDevice::m_viewport.m_z` |
| `0xADF5E8`, `0xADF628` | `CWorldScene::viewMatrix`, `CWorldScene::projMatrix` |
| `0xCD8F5C`, `0xCD8F68` | `CWorldScene::s_activeWorldView`, `CWorldScene::camTarget` |
| `0xCD8794` | `s_liquidTypeId` |
| `0xAB63BC` (the loading screen's map) | `s_mapId` |
| World frame `+0xB14` | `World::s_farClip` |
| `0xD38B00` block (day/night) | `DayNight::g_dnInfo`: `m_dayProgression`, `m_cameraPos`, `m_stormBlend`, `m_overrideLightParamsId`, `m_fog`, `m_fogInterior`, `m_interiorFogBlend`, `m_bands` |
| `0xD38E28`, `0xD38E48` | `DayNight::g_planets.m_sun`, `.m_moon` |
| `0xA41CA4`, `0xA41CA0` | 0.2013889 and 0.9236111, as `DayNight::DNClouds::SetupBumpMap` chooses sun or moon |
| `0xD45774`, `0xD45780`, `0xB74364`, `0xB74368` | `FFX::s_cvarFFX`, `FFX::s_effect`, `CGWorldFrame::ffxEffectGlow`, `CGWorldFrame::ffxEffectDeath` |
| Glow composite pass colour | `EffectGlow::m_passes` / `m_altPasses`, `PassGlowBase::m_color` |
| `0xD43B18` settings bank | `Liquid::CSettingsBank::s_settings` |
| `0xAD4070` LiquidType rows | `g_liquidTypeDB` |
| Liquid renderer bucket | `Liquid::CRenderer::m_instances[1]` |
| `0xCD754C` point lights | `World::s_m2Scene->m_lightGrid`, `CM2Light` |
| Camera WMO groups | `CWorldScene::s_viewerMapObjDef`, `s_viewerMapObjGroups`, `CMapObj`, `CMapObjGroup` |
| Visible units | `ClntObjMgrEnumVisibleObjects`, `CGUnit_C::m_movementData`, `CGObject_C::m_worldObject` |
| `0xADF7F0` | `CWorld::s_waterRipples` |

## Building

On by default on Windows (`WOWDECOMP_VOLFOG`). It needs `fxc.exe` from the Windows SDK, found automatically or
given with `-DFXC=<path>`, and downloads Dear ImGui at configure time. `CoAVolFog.ini`, `fogdata.bin` and
`waterdata.bin` are copied next to the client executable. Configure with `-DWOWDECOMP_VOLFOG=OFF` to leave it out;
the client then renders exactly as before.

Not available on Linux yet: the shaders would have to be compiled with `fxc` on Windows and copied over, and the
NVIDIA depth copy and settings window use Win32. Only the Direct3D 9 backend is wrapped (`gxApi d3d9`, the default),
not D3D9Ex, OpenGL or Vulkan, as upstream.

## Not verified

Compiled and checked here, but not run in the client. Upstream's harness (`tests/`) is kept for reference and is not
built.
