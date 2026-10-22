#!/bin/sh
# Rebuilds the Direct3D 11 device's shader headers in vkd3d/ with Tools/VfHlsl's vfhlsl (see Tools/VfHlsl/README.md):
#   Engine/Source/Gx/CGxDeviceD3d11/Shaders/build.sh <path to vfhlsl>
set -e
VFHLSL=${1:?usage: build.sh <path to vfhlsl>}
cd "$(dirname "$0")"
"$VFHLSL" /T vs_4_0 /E VsMain /Vn g_ffp_vs /Fh vkd3d/ffp_vs.h FixedFunction.hlsl
"$VFHLSL" /T ps_4_0 /E PsMain /Vn g_ffp_ps /Fh vkd3d/ffp_ps.h FixedFunction.hlsl
"$VFHLSL" /T vs_4_0 /E VsMain /Vn g_blit_vs /Fh vkd3d/blit_vs.h Blit.hlsl
"$VFHLSL" /T ps_4_0 /E PsMain /Vn g_blit_ps /Fh vkd3d/blit_ps.h Blit.hlsl
"$VFHLSL" /T ps_4_1 /E PsMain /Vn g_depthcopy_ps /Fh vkd3d/depthcopy_ps.h DepthCopy.hlsl
"$VFHLSL" /T gs_4_0 /E GsMain /Vn g_clipplanes_gs /Fh vkd3d/clipplanes_gs.h ClipPlanes.hlsl
"$VFHLSL" /T ps_4_0 /E PsMain /Vn g_cleardepth_ps /Fh vkd3d/cleardepth_ps.h ClearDepth.hlsl
