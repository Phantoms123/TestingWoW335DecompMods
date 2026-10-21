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

### Volumetric fog and water (WoW/Source/VolFog)
- CoAVolFog's renderers, shaders, data and `CoAVolFog.ini` as upstream; its DLL loader, call-site patches and
  address reads are replaced by calls from the client's world render and reads of the client's own globals.
  `WoW/Source/VolFog/NATIVE.md` maps every upstream patch site and address to the source that replaces it.
- Windows only, Direct3D 9 backend. Settings window: Ctrl+F7 in game.

### Build fixes after the merge
- `WoW/Source/Game/Types.h`: `windows.h` guarded for non-Windows builds.
- `WoW/Source/WowServices/Posix/Callbacks.cpp`: send functions return `WC_SEND_RESULT`, as `main` now declares.

## Known limits

- The converters are checked against real 4.3.4 files, but the client itself has not been run on them yet.
- No sample model used 4.x special effects 0-2 or compressed gravity, so those two conversions are untested.
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
