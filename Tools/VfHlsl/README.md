# VfHlsl

`vfhlsl` compiles CoAVolFog's HLSL shaders (WoW/Source/VolFog/shaders) to Direct3D 9 byte code without fxc.exe, so
the Linux client can be built with CoAVolFog. It is not part of the original 12340 client.

It is [vkd3d-shader](https://gitlab.winehq.org/wine/vkd3d) 1.19, as bundled with Wine 11.9, behind a small front
end (`vfhlsl.c`) that takes the fxc options CoAVolFog's CMake passes (`/T /E /Vn /D /Fh`; others are ignored) and
writes the same kind of C header fxc's `/Fh` does.

## Changes to vkd3d-shader

- `patches/0001-hlsl-add-tex3Dlod.patch`: adds the `tex3Dlod` intrinsic (the fog volume lookups use it).
- `patches/0002-d3dbc-shader-model-3-loops.patch`: emits shader model 3 loops. A loop that is not unrolled
  becomes `rep i15` with `defi i15, 255, 0, 1, 0` and leaves through `break`; `continue` is still unsupported.
- `patches/0003-tpf-clip-distance.patch`: accepts `SV_ClipDistance` (vertex shader outputs, pixel shader inputs)
  in shader model 4 and 5 shaders; the Direct3D 11 device's fixed-function shader uses it for clip planes.

`vfhlsl` writes DXBC (Direct3D 10 and 11) for `_4_` and `_5_` profiles (`vs_4_0`, `ps_5_0`, ...) and Direct3D 9
byte code for the rest.

`vfhlsl` also defines `VF_VKD3D=1` for every shader. `vf_common.hlsli` uses it to unroll the three fixed-count loops
(`VF_FIXED_LOOP`) that vkd3d-shader would otherwise have to keep as loops; the result is the same.

## Using it

The Linux build does not need it: the headers it produced are committed in `WoW/Source/VolFog/shaders/vkd3d` and
used as they are. To rebuild them after changing a shader:

```sh
Tools/VfHlsl/build.sh /tmp/vfhlsl          # needs git, a C compiler, bison and flex
cmake -B out/client-dxvk -DVFHLSL=/tmp/vfhlsl/vfhlsl
ninja -C out/client-dxvk vf_shaders
cp out/client-dxvk/WoW/Source/VolFog/shaders/*.h WoW/Source/VolFog/shaders/vkd3d/
```

`shim/` holds the few Windows headers vkd3d-shader expects inside Wine's tree: a minimal `windows.h`, the enums of
`d3dcommon.h` and `d3dx9shader.h` (taken from MinGW-w64), and a `config.h`.

All 60 shaders compile; the output was read back with vkd3d-shader's own D3D9 parser, which validates it.
