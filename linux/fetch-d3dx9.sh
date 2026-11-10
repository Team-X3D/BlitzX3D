#!/usr/bin/env bash

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DEST="${1:-$B3D_ROOT/_release/bin}"
STAGE="$B3D_ROOT/vendor/d3dx9/x86"
CACHE="$B3D_TOOLCHAIN/downloads"
REDIST="$CACHE/directx_Jun2010_redist.exe"
REDIST_URL="https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe"
REDIST_SHA256="053f76dcbb28802e23341b6a787e3b0791c0fa5c8d4d011b1044172dbf89c73b"
FILES=(d3dx9_43.dll d3dcompiler_43.dll)

mkdir -p "$DEST" "$STAGE"

have_all() {
    local dir="$1"
    local f
    for f in "${FILES[@]}"; do
        [ -f "$dir/$f" ] || return 1
    done
    return 0
}

copy_all() {
    local dir="$1"
    local f
    for f in "${FILES[@]}"; do
        cp -f "$dir/$f" "$DEST/$f"
        cp -f "$dir/$f" "$STAGE/$f"
    done
}

if have_all "$DEST"; then
    b3d_info "native DirectX already staged in $DEST"
    exit 0
fi

if have_all "$STAGE"; then
    copy_all "$STAGE"
    b3d_info "staged native DirectX from $STAGE"
    exit 0
fi

if [ -n "${B3D_D3DX9_DIR:-}" ] && have_all "$B3D_D3DX9_DIR"; then
    copy_all "$B3D_D3DX9_DIR"
    b3d_info "staged native DirectX from $B3D_D3DX9_DIR"
    exit 0
fi

if ! b3d_have cabextract; then
    SEVENZIP="$(b3d_find_7z)" || true
    if [ -z "$SEVENZIP" ]; then
        b3d_die "cabextract or 7z is required. install one, or put d3dx9_43.dll and d3dcompiler_43.dll in $STAGE"
    fi
else
    SEVENZIP=""
fi

mkdir -p "$CACHE"
if [ ! -f "$REDIST" ]; then
    b3d_info "downloading DirectX June 2010 redistributable"
    if b3d_have curl; then
        curl -fL --retry 3 -o "$REDIST.part" "$REDIST_URL"
    else
        wget -O "$REDIST.part" "$REDIST_URL"
    fi
    mv "$REDIST.part" "$REDIST"
fi

b3d_info "verifying DirectX redistributable"
echo "$REDIST_SHA256  $REDIST" | sha256sum -c - >/dev/null

TMP="$B3D_TOOLCHAIN/d3dx9/tmp"
rm -rf "$TMP"
mkdir -p "$TMP/cabs" "$TMP/out"

b3d_info "extracting DirectX runtime DLLs"
if b3d_have cabextract; then
    cabextract -q -d "$TMP/cabs" -L -F '*d3dx9*x86*' "$REDIST"
    cabextract -q -d "$TMP/cabs" -L -F '*d3dcompiler*x86*' "$REDIST"
else
    "$SEVENZIP" x -y -o"$TMP/cabs" "$REDIST" >/dev/null
fi

while IFS= read -r -d '' cab; do
    if b3d_have cabextract; then
        cabextract -q -d "$TMP/out" -L -F 'd3dx9_43.dll' "$cab" 2>/dev/null || true
        cabextract -q -d "$TMP/out" -L -F 'd3dcompiler_43.dll' "$cab" 2>/dev/null || true
    else
        "$SEVENZIP" e -y -o"$TMP/out" "$cab" 'd3dx9_43.dll' >/dev/null 2>&1 || true
        "$SEVENZIP" e -y -o"$TMP/out" "$cab" 'd3dcompiler_43.dll' >/dev/null 2>&1 || true
    fi
done < <(find "$TMP/cabs" -type f -iname '*x86*.cab' -print0)

if ! have_all "$TMP/out"; then
    b3d_die "could not extract d3dx9_43.dll and d3dcompiler_43.dll from the DirectX redist"
fi

copy_all "$TMP/out"
rm -rf "$TMP"
b3d_info "staged native DirectX in $DEST and $STAGE"
