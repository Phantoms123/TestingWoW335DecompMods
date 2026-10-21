#!/bin/sh
# Builds vfhlsl: Wine 11.9's bundled vkd3d-shader (1.19) with the two patches in patches/, linked to vfhlsl.c.
# Needs git, a C compiler, bison and flex. Usage: Tools/VfHlsl/build.sh [work directory]   (default: ./vfhlsl-build)
# The result is <work directory>/vfhlsl; pass it to CMake as -DVFHLSL=<path> to rebuild the CoAVolFog shader headers.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$PWD/vfhlsl-build}
CC=${CC:-cc}
WINE_TAG=wine-11.9

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
SRC=$WORK/wine
OBJ=$WORK/obj

if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$WINE_TAG" --filter=blob:none --sparse https://github.com/wine-mirror/wine.git "$SRC"
    git -C "$SRC" sparse-checkout set libs/vkd3d include/wine
    for p in "$HERE"/patches/*.patch; do
        git -C "$SRC" apply "$p"
    done
fi

V=$SRC/libs/vkd3d
S=$V/libs/vkd3d-shader
rm -rf "$OBJ"
mkdir -p "$OBJ/wine"
cp "$HERE"/shim/*.h "$OBJ/"
cp "$SRC/include/wine/list.h" "$SRC/include/wine/rbtree.h" "$OBJ/wine/"

cd "$OBJ"
bison -o hlsl.tab.c --defines=hlsl.tab.h "$S/hlsl.y"
bison -o preproc.tab.c --defines=preproc.tab.h "$S/preproc.y"
flex -o hlsl.yy.c "$S/hlsl.l"
flex -o preproc.yy.c "$S/preproc.l"

FLAGS="-O2 -w -fPIC -DLIBVKD3D_SHADER_SOURCE -DVKD3D_NO_WIN32_TYPES -DVKD3D_NO_VULKAN_H -I. -I$V/include -I$V/include/private -I$S -I$V/include/vulkan"
for f in "$V"/libs/vkd3d-common/debug.c "$V"/libs/vkd3d-common/error.c "$V"/libs/vkd3d-common/memory.c \
         "$V"/libs/vkd3d-common/utf8.c "$S"/*.c hlsl.tab.c preproc.tab.c hlsl.yy.c preproc.yy.c; do
    # shellcheck disable=SC2086
    $CC -c $FLAGS "$f" -o "$(basename "$f" .c).o"
done
ar rcs libvkd3d-shader.a ./*.o
$CC -O2 -w -DVKD3D_NO_WIN32_TYPES -DVKD3D_NO_VULKAN_H -I"$V/include" "$HERE/vfhlsl.c" libvkd3d-shader.a \
    -lm -lpthread -ldl -o "$WORK/vfhlsl"
echo "built $WORK/vfhlsl"
