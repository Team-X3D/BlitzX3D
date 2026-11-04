#!/usr/bin/env bash

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CONFIG="Release"
ACTION="build"
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        clean)  ACTION="clean" ;;
        Debug|Release|RelWithDebInfo|MinSizeRel) CONFIG="$1" ;;
        -h|--help)
            echo "usage: $0 [clean] [configuration] [msbuild args]"
            exit 0
            ;;
        *) EXTRA+=("$1") ;;
    esac
    shift
done

MSBUILD="$B3D_MSVC_DIR/bin/x86/msbuild"
if [ ! -x "$MSBUILD" ]; then
    b3d_die "MSVC not found. run ./linux/setup-toolchain.sh"
fi
[ -f "$WINEPREFIX/system.reg" ] || b3d_die "wine prefix missing. run ./linux/setup-toolchain.sh"
[ -f "$B3D_DXSDK_DIR/Include/d3dx9.h" ] || b3d_die "DirectX SDK missing. run ./linux/setup-toolchain.sh"
[ -f "$B3D_ROOT/vendor/SDL3/include/SDL3/SDL.h" ] || b3d_die "SDL3 submodule missing. run git submodule update --init"
[ -f "$B3D_ROOT/angelscript/sdk/angelscript/include/angelscript.h" ] || b3d_die "angelscript submodule missing. run git submodule update --init"

export PATH="$B3D_MSVC_DIR/bin/x86:$PATH"
export MSBUILDDISABLENODEREUSE=1

DXSDK_DIR="$(b3d_wine_path "$B3D_DXSDK_DIR")"
case "$DXSDK_DIR" in
    *'\') ;;
    *) DXSDK_DIR="$DXSDK_DIR\\" ;;
esac
export DXSDK_DIR

SDK_VERSION=""
if [ -d "$B3D_MSVC_DIR/kits/10/include" ]; then
    SDK_VERSION="$(ls -1 "$B3D_MSVC_DIR/kits/10/include" 2>/dev/null | grep -E '^10\.' | sort -V | tail -n 1 || true)"
fi

COMMON_ARGS=(
    "$B3D_ROOT/blitz3d.sln"
    "/p:Configuration=$CONFIG"
    "/p:Platform=x86"
    "/p:TrackFileAccess=false"
    "/v:minimal"
)
if [ -n "$SDK_VERSION" ]; then
    COMMON_ARGS+=("/p:WindowsTargetPlatformVersion=$SDK_VERSION")
fi

if [ "$ACTION" = "clean" ]; then
    b3d_info "cleaning $CONFIG"
    exec "$MSBUILD" "${COMMON_ARGS[@]}" /t:Clean "${EXTRA[@]+"${EXTRA[@]}"}"
fi

b3d_info "building $CONFIG"
b3d_info "targets: $B3D_BUILD_TARGETS"

PARALLEL=(/m:1)
for arg in "${EXTRA[@]+"${EXTRA[@]}"}"; do
    case "$arg" in
        /m|/m:*|/maxcpucount*) PARALLEL=(); break ;;
    esac
done

"$MSBUILD" \
    "${COMMON_ARGS[@]}" \
    "/t:$B3D_BUILD_TARGETS" \
    "${PARALLEL[@]+"${PARALLEL[@]}"}" \
    "${EXTRA[@]+"${EXTRA[@]}"}"

b3d_info "done. output in $B3D_ROOT/_release"
