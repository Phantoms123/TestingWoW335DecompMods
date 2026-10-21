#!/bin/sh
# Rebuilds the post effect shader headers in vkd3d/ with Tools/VfHlsl's vfhlsl (see Tools/VfHlsl/README.md):
#   Engine/Source/ShaderEffect/PostShaders/build.sh <path to vfhlsl>
set -e
VFHLSL=${1:?usage: build.sh <path to vfhlsl>}
cd "$(dirname "$0")"
"$VFHLSL" /T vs_3_0 /E main /Vn g_post_vs /Fh vkd3d/post_vs.h post_vs.hlsl
for name in haze ao ao_blur copy bright blur bloom fxaa sharpen fill depthfade mix; do
    "$VFHLSL" /T ps_3_0 /E main /Vn "g_post_${name}_ps" /Fh "vkd3d/${name}_ps.h" "${name}_ps.hlsl"
done
# Shader model 4 (Direct3D 11): vkd3d accepts the shader model 3 syntax, register(c#) becoming constant buffer 0.
"$VFHLSL" /T vs_4_0 /E main /Vn g_post_vs_sm4 /DSM4=1 /Fh vkd3d/post_vs_sm4.h post_vs.hlsl
for name in haze ao ao_blur copy bright blur bloom fxaa sharpen fill depthfade mix; do
    "$VFHLSL" /T ps_4_0 /E main /Vn "g_post_${name}_ps_sm4" /DSM4=1 /Fh "vkd3d/${name}_ps_sm4.h" "${name}_ps.hlsl"
done
