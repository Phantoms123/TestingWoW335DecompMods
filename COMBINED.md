# Combined client: what is in this tree

This is SatyPardus/wowdecomp (3.3.5a, build 12340) with other branches merged in and 4.x (Cataclysm)
content support added. It is meant for a 3.3.5a TrinityCore server.

## Sources

| Source | What it adds |
|---|---|
| [SatyPardus/wowdecomp](https://github.com/SatyPardus/wowdecomp) `main` | The base: the decompiled 3.3.5a client |
| [arlyon/wowdecomp](https://github.com/arlyon/wowdecomp) `vulkan-next` | Linux/POSIX/SDL platform layer, native Vulkan (Slang) and DXVK-native backends |
| [SatyPardus/wowdecomp15595](https://github.com/SatyPardus/wowdecomp15595) | Reference only: the 4.3.4 record layouts the converters follow |
| [WarcraftXL](https://github.com/WarcraftXL) modules (GPL-3.0-or-later) | Reference for the model and terrain converters, rewritten against this tree's types |
| [jealous-sound/coa-vfog](https://github.com/jealous-sound/coa-vfog) (GPL-3.0) | Volumetric fog and modern water, built into the client in `WoW/Source/VolFog` |

Not merged: the Vulkan backend on SatyPardus/wowdecomp's `vulkan` branch (from WarcraftXL). It defines the same
`CGxDeviceVulkan` class as arlyon's, so only one can be kept; arlyon's was kept because it supports Linux.

## Changes on top of the merge

Every changed spot in the original sources is marked `// CHANGED:`.

### Models (Engine/Source/Model2)
- `M2Compat.cpp`: loads M2 versions 265-272 (4.x). Particle records (0x1EC to 0x1DC bytes) and camera records
  (0x74 to 0x64, fov taken from the fov track) are reshaped before relocation; packed multi-texture particle
  texture ids are unpacked and out-of-range ones clamped; compressed gravity keys are expanded.
- 4.x skins carry final shader values and special effect ids in the file. Effects 0-2 map to this client's
  specialized effects 1-3; the rest map to the nearest standard combiner. This client's own substitution
  passes are skipped for these models, since 4.3.4 had neither.
- `CM2Shared::GetEffect`: unknown special shader ids no longer crash (null shader names reached `strcpy`).
- Particle blend modes past 6 get an additive material instead of keeping the opaque alpha-tested one
  (which drew as solid quads).
- 4.x batches that draw three textures (special effect 3, used by many glowing spell models) are drawn with
  their first two: this client's shaders read two, and its texture setup broke past the second.
- Checked on 117 real 4.3.4 models (spells, particles, cameras): every converted particle emitter and camera
  matches its source record, every array stays inside the file, and every skin batch indexes valid
  materials, textures and combos.

### Terrain (WoW/Source/WorldClient)
- `AdtCompat.cpp`: a tile with a `_tex0.adt` file is stored the 4.x way. Root, `_tex0` and `_obj0` are merged in
  memory into one tile laid out the way `CMapArea`/`CMapChunk::ProcessIffChunks` read it. Tiles without one
  load exactly as before.
- `LiquidCompat.cpp`: 4.x MH2O stores a LiquidObject id (42 and up) where this client expects the vertex
  format. Each layer's real format is resolved the way 4.3.4's `Liquid::GetVertexFormat` does (ocean: depth
  only; otherwise the 4.3.4 LiquidType's material decides), and 4.x layouts 3-5 are rewritten into 0 or 1.
  Liquid types this client has no record for become stock water, ocean, magma or slime by sound bank.
  Checked on real Lost Isles tiles: every height and depth this client reads equals what 4.3.4 reads.

### Shaders (Engine/Source/Gx/CGxDevice)
- `ShaderCompat.cpp`: the shader loader also reads 4.x shader files (GXSH 0x10004: permutations in one
  stream cut into zlib-compressed 16 KB blocks), so 4.x render features can use 4.x's own shaders. 3.3.5a
  files load exactly as before.
- Checked on 209 4.3.4 `ps_3_0`/`vs_3_0` files (4,842 permutations): each decodes to a complete D3D9 shader,
  byte-identical to an independent reader, and damaged copies are refused without reading out of bounds.

### Depth texture and sun shafts (Engine/Source/Gx, Engine/Source/ShaderEffect)
- New texture format `GxTex_Intz` (D3D9 INTZ) and `CGxDevice::DeviceSetSampledDepth`: the back buffer's depth
  buffer can be a texture shaders read. 4.3.4 copied depth with NVAPI or AMD's RESZ, which DXVK lacks; here the
  world renders straight into it, so it works on Windows drivers and on Linux through DXVK. Needs
  multisampling off.
- `DepthCache.cpp`: keeps that texture sized to the window while an effect uses it.
- `SunShafts.cpp`: 4.3.4's sun shafts, using 4.3.4's own shaders. With shafts on, the sun glare draws with 4.3.4's
  DNGlare shader, which adds them. Shader constants the passes overwrite are put back afterwards, since the
  client's own shaders keep the projection and fog colour for the whole frame.
- To use it: put these 4.3.4 shader files in a patch MPQ under `Shaders\Cata\` (keeping the 4.3.4 folder
  layout), then `/console sunShafts 2` (1 is the cheaper mode):
  - `Shaders\Cata\Pixel\ps_3_0\SunShaftsMask.bls`, `SunShaftsPropagate.bls`, `DNGlare.bls`
  - `Shaders\Cata\Vertex\vs_3_0\SunShaftsMask.bls`, `SunShaftsPropagate.bls`, `DNGlare.bls`
  If they are missing, or the device cannot sample depth, the setting stays off and the log says why. With
  multisampling the depth is copied into the texture with RESZ (DXVK and AMD drivers offer it).

### 4.3.4 water (WoW/Source/LiquidSystem/CataWater.cpp)
- 4.3.4's water surface (its Liquid::CMaterialWater at waterDetail 2), using 4.3.4's ProcWater, ProcWaterAbove,
  ProcWaterBelow and FourTapBlur shaders with the constants and textures 4.3.4 gives them: refraction of the frame
  drawn so far, depth tint and shoreline fade from the scene depth, animated normal maps, sun specular, foam, a
  blurred screen-space reflection, and a sky reflection rendered from the mirrored camera.
- `/console waterDetail 2` (default) is the 4.3.4 water, `0` the 3.3.5 water. `reflectionMode` and
  `reflectionDownscale` are registered too (downscale shrinks the sky reflection texture).
- Needs, in a patch MPQ: the shaders above under `Shaders\Cata\...`, `XTextures\ocean\newoceanbump.blp`,
  `XTextures\foam\waterchop7bw.blp` and a `DBFilesClient\LiquidType.dbc` with 4.3.4's water values (the 3.3.5
  file with textures 2-3 and floats 3-15 filled in; classic fields unchanged). Water types without them draw the
  3.3.5 way. Needs the depth texture; with multisampling it is copied with RESZ (DXVK, AMD drivers).
- 4.3.4's ripple simulation (`/console rippleDetail 1`, default; `2` adds a second level and steps the first at
  25 ms): 1024x1024 two-channel float height maps covering 128 yards around the camera, moved in whole texels,
  stepped with 4.3.4's PropagateDisplMap wave shader every 50 ms and disturbed with PerturbDisplMap splats of
  `XTextures\Ripples\perturbTex.blp` by units in water (CGUnit_C::CreateRipple, as 4.3.4: strongest half
  submerged, scaled by speed, pulsing every 800 ms) and by rain heavier than 0.2. The water shaders read them as
  4.3.4's do (vertex c51-c52, pixel c29-c31, textures 4 and 8). With it on, units make no 3.3.5 ripple sprites;
  `rippleDetail 0` brings those back. Needs the DisplMap shaders and perturbTex in the patch MPQ.
- 4.3.4's magma and slime (material 2 liquids) with the 4.3.4 water on: the ProcMagma shaders, three scrolling
  and turning layers (LiquidType Tex1-3) over a base colour (int 2), blended by a 128x128x16 noise volume
  (`XTextures\FX\noise.blob`; the D3D9 device gained DeviceTexSetVolume for it). Needs the ProcMagma shaders,
  the layer textures and noise.blob in the patch MPQ.
- Real reflections (`waterDetail 3`, `reflectionMode` 2 terrain, 3 terrain and buildings, default 3): before the
  world draws, the terrain and map objects culled for the frame are drawn again into a reflection texture through
  the view mirrored in the plane of the water nearest the camera (found while drawing the previous frame), with a
  clip plane at the water and the winding turned (CGxDevice::DeviceSetCullFlip); the render lists are kept for the
  real pass (CWorldScene::s_reflectionPass). The sky is drawn mirrored behind them. Water at that height samples
  it with 4.3.4's real reflection shader permutation; other water keeps the sky reflection. `reflectionStrength`
  (default 0.75) fades the reflected world towards the fog colour. The water's normal map, foam and depth tint
  textures are filtered anisotropically so distant water keeps its detail. The reflection plane is held for 45
  frames after its water was last seen and eases between nearby heights, and the reflection fades in and out
  (about a third of a second) instead of switching. Outdoors, the culling (the first chunk pass, the sort table and the models' own off-view flag) reaches two screens' height above the view
  while reflections are on, so what the water mirrors does not pop as the camera turns, and the reflection is
  softened slightly (as 4.3.4's blurredReflection). The reflection draws into its own INTZ depth texture, and what lies
  beyond a third of `reflectionDistance` (default 200 yards) fades into the fog by that depth, so distant terrain,
  buildings and trees neither stand out nor appear abruptly. With reflectionMode 3 the models (trees, characters, all three passes, geometry only;
  creatures; `reflectionModels 0` leaves them out) are drawn too: their matrices already hold the view, so they are
  mirrored through the projection (the view-space mirror in front of it), which keeps the same clip plane
  (CM2Scene::s_keepClipPlane stops models in liquid from setting their own). The chunks considered for culling
  also reach a screen's height above the view.
- `shorelineSoftness` (default 1): the depths the water shader compares are scaled down for distant water (PS
  c5-c8 z and c22), so its shoreline fade and foam cover more of the shallows instead of ending within a pixel.
- Not ported yet: ripples from the collision meshes of moving objects
  (4.3.4's PerturbDisplMapMesh).

### Advanced video options (Engine/Source/ShaderEffect/PostEffects.cpp)
- The settings behind Waystone's Advanced Effects panel, with its console variable names and defaults, as
  Direct3D 9 shaders compiled into the client (`PostShaders/*.hlsl`, headers made with Tools/VfHlsl by
  `PostShaders/build.sh`): `ambientOcclusion` (depth-based ambient obscurance with a bounded per-sample term, half resolution, depth-aware blur;
  `aoRadius`, `aoStrength`, `aoDebug`), `aerialFog` (exponential height haze in the zone fog colour, density default 0.0015;
  `aerialFogDensity`, `aerialFogFalloff`, `aerialFogStart`, `aerialFogMax`), both over the opaque world before
  the liquids; `postBloom` (quarter and eighth resolution bloom with highlight roll-off, exposure, saturation,
  contrast; `bloomIntensity`, `bloomThreshold`, `toneMapping`, `toneExposure`, `colorSaturation`,
  `colorContrast`) before the full screen effects; `postAA` (FXAA-style edge smoothing, then contrast adaptive
  sharpening, `sharpen`) last.
- Farther view: `farclip` up to 2500 yards on every map (was 1583, or 791 on the old continents);
  `groundEffectDist` up to 260 (detail doodad buffers grow with it); `particleDensity` and `environmentDetail`
  also take 4.3.4's percent values.
- The panels themselves are Waystone's VideoOptionsPanels (FrameXML and GlueXML) and GraphicsQualityLevels.lua in
  the patch MPQ, with shadow quality mapped onto 3.3.5's `extShadowQuality` and the view distance slider to 2500.

### 4.3.4 shadows (Engine/Source/ShaderEffect/ShadowCache.cpp, WoW/Source/WorldClient/ShadowQuery.cpp)
- 4.3.4 kept 3.3.5's shadow cache (the same textures, shaders and PCF kernel) and gave it new settings, which
  are added here on top of it: `shadowMode` 0-3 with `shadowTextureSize` 1024 or 2048 picks one of
  `extShadowQuality`'s six levels (1: dynamic shadows near the player, 2: environment shadows too, 3: cascaded
  shadow maps); -1, the default, leaves `extShadowQuality` and the video options in charge, and setting
  `extShadowQuality` puts `shadowMode` back to -1. `shadowTextureSize` 0 sizes by quality as 3.3.5 does.
- `shadowSoftness` scales the PCF kernel (1 = original, 0.25-6, clamped as 4.3.4/Waystone do).
- `shadowFollowSun` 1 casts the shadows from the sky's sun, or the moon at night, instead of the zone light's
  fixed direction (Waystone's steps: light kept at least 0.2 below level, shadows only rebuilt once it has
  turned about half a degree). 0, the default, is 3.3.5's direction.

### Volumetric fog and water (WoW/Source/VolFog)
- CoAVolFog's renderers, shaders, data and `CoAVolFog.ini` as upstream; its DLL loader, call-site patches and
  address reads are replaced by calls from the client's world render and reads of the client's own globals.
  `WoW/Source/VolFog/NATIVE.md` maps every upstream patch site and address to the source that replaces it.
- Direct3D 9 backend on Windows; DXVK backend on Linux. Settings window: Ctrl+F7 in game.
- On and off: `/console coaVolFog 0` switches all of it off from the next frame (the client then draws as
  unmodified) and `1` back on. The setting is saved; starting with it at 0 leaves CoAVolFog unloaded, so turning
  it on again then needs a restart. To remove it, delete `CoAVolFog.ini`, `fogdata.bin` and `waterdata.bin` next
  to the executable: without `CoAVolFog.ini` it is not loaded. `-DWOWDECOMP_VOLFOG=OFF` builds the client
  without it.
- Linux: the Win32-only parts have Linux equivalents (`src/platform.h`): INI reading and writing, timers, locks.
  The settings window reads SDL input (the client's event loop offers each event to it first, `vf_on_os_event`)
  and draws with ImGui's SDL2 backend. NVAPI depth copy is skipped (it is a Windows driver DLL); fault guards run
  unguarded, so a fault in CoAVolFog crashes rather than switching the effect off.
- Linux shaders: `shaders/vkd3d` holds the compiled headers, made by `Tools/VfHlsl` (vkd3d-shader with an fxc-style
  front end, since fxc.exe is Windows-only). See `Tools/VfHlsl/README.md` to rebuild them.

### Build fixes after the merge
- `WoW/Source/Game/Types.h`: `windows.h` guarded for non-Windows builds.
- `WoW/Source/WowServices/Posix/Callbacks.cpp`: send functions return `WC_SEND_RESULT`, as `main` now declares.

## Known limits

- The converters are checked against real 4.3.4 files, but the client itself has not been run on them yet.
- No sample model used 4.x special effects 0-2 or compressed gravity, so those two conversions are untested.
- Sun shafts are checked against the 4.3.4 shaders' inputs, constants and permutations, but have not been run
  in the client yet.
- 4.x models whose effects need shaders that 3.3.5a's shader archive does not contain draw with the closest
  standard shader. Exact results need the 4.x shaders ported too.
- Split tiles are read synchronously when first needed, which can hitch while flying into new areas.
- 4.x liquid types need matching `LiquidType.dbc` records to look right; until then they are water.
- CoAVolFog has not been run in the client yet; it was written for Ascension's client, so if something looks off,
  `CoAVolFog.log` next to the executable says which part stopped and why.
- Still to port from WarcraftXL's model module: bone-budget splitting, the shadow bone-flag fix and hit-test
  ordering.

## Building

Windows: as in README.md (Visual Studio 2022, CMake). Use `-DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON` to load
modified MPQs. CoAVolFog needs `fxc.exe` from the Windows SDK (installed with Visual Studio's C++ workload);
`-DWOWDECOMP_VOLFOG=OFF` leaves it out.

Linux builds CoAVolFog too, from the committed shader headers; `-DWOWDECOMP_VOLFOG=OFF` leaves it out.

Linux (DXVK backend), with SDL2, DXVK-native, libvorbis and libcurl installed:

```sh
cmake -S . -B out/client-dxvk -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DDXVK_NATIVE_ROOT=/path/to/dxvk-native/prefix
cmake --build out/client-dxvk --target Wow
```

For the native Vulkan backend on Linux, see the "Native Vulkan" section of README.md.

## Licence note

The converters follow GPL-3.0-or-later code from WarcraftXL. If you distribute a client built from this tree,
also make its source available.
