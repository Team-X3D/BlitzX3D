#!/usr/bin/env bash

B3D_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export B3D_ROOT

B3D_TOOLCHAIN="${B3D_TOOLCHAIN:-${XDG_DATA_HOME:-$HOME/.local/share}/blitzx3d-toolchain}"
export B3D_TOOLCHAIN

B3D_WINEPREFIX="${B3D_WINEPREFIX:-$B3D_TOOLCHAIN/wineprefix}"
export B3D_WINEPREFIX
export WINEPREFIX="$B3D_WINEPREFIX"

if [ ! -f "$B3D_WINEPREFIX/system.reg" ]; then
    export WINEARCH=win64
fi

export WINEDEBUG="${WINEDEBUG:--all}"

B3D_MSVC_WINE_REV="514f8ea34842cd6d831804d0e9658d3a32870ae1"
export B3D_MSVC_WINE_REV

B3D_VS_MAJOR="17"
B3D_MSVC_VERSION="17.14"
export B3D_VS_MAJOR B3D_MSVC_VERSION

B3D_MSVC_DIR="$B3D_TOOLCHAIN/msvc"
export B3D_MSVC_DIR

B3D_DXSDK_DIR="$B3D_TOOLCHAIN/dxsdk/DXSDK"
export B3D_DXSDK_DIR

B3D_BUILD_TARGETS="${B3D_BUILD_TARGETS:-bblaunch;blitzide_imgui;debugger_imgui;debugger_ui}"
export B3D_BUILD_TARGETS

b3d_die() {
    printf '%s\n' "error: $*" >&2
    exit 1
}

b3d_info() {
    printf '%s\n' "-> $*" >&2
}

b3d_warn() {
    printf '%s\n' "warning: $*" >&2
}

b3d_find_wine() {
    command -v wine64 2>/dev/null || command -v wine 2>/dev/null
}

b3d_find_7z() {
    command -v 7zz 2>/dev/null || command -v 7z 2>/dev/null || command -v 7za 2>/dev/null
}

b3d_have() {
    command -v "$1" >/dev/null 2>&1
}

b3d_wine_path() {
    local path="$1"
    case "$path" in
        /*) printf 'z:%s' "${path//\//\\}" ;;
        *)  printf '%s' "$path" ;;
    esac
}
