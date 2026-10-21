#!/bin/sh
# Rebuilds the post effect shader headers in vkd3d/ with Tools/VfHlsl's vfhlsl (see Tools/VfHlsl/README.md):
#   Engine/Source/ShaderEffect/PostShaders/build.sh <path to vfhlsl>
set -e
VFHLSL=${1:?usage: build.sh <path to vfhlsl>}
cd "$(dirname "$0")"
"$VFHLSL" /T vs_3_0 /E main /Vn g_post_vs /Fh vkd3d/post_vs.h post_vs.hlsl
for name in haze ao ao_blur copy bright blur bloom fxaa sharpen; do
    "$VFHLSL" /T ps_3_0 /E main /Vn "g_post_${name}_ps" /Fh "vkd3d/${name}_ps.h" "${name}_ps.hlsl"
done
