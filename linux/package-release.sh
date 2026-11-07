#!/usr/bin/env bash

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if [ ! -d "$B3D_ROOT/_release" ]; then
    b3d_die "missing $B3D_ROOT/_release; build first"
fi

if [ "${B3D_SKIP_D3DX9:-0}" != "1" ]; then
    "$B3D_ROOT/linux/fetch-d3dx9.sh" "$B3D_ROOT/_release/bin" || \
        b3d_die "could not stage native DirectX; set B3D_SKIP_D3DX9=1 to package without it"
fi

TAG="$(git -C "$B3D_ROOT" describe --tags --abbrev=0 2>/dev/null || echo dev)"
OUT="${1:-$B3D_ROOT/BlitzX3D_${TAG}.zip}"
case "$OUT" in
    /*) ;;
    *) OUT="$B3D_ROOT/$OUT" ;;
esac
case "$OUT" in
    *.zip) ;;
    *) OUT="$OUT.zip" ;;
esac

STAGE_ROOT="$B3D_ROOT/_release_package"
STAGE="$STAGE_ROOT/BlitzX3D"
rm -rf "$STAGE_ROOT"
mkdir -p "$STAGE"
cp -a "$B3D_ROOT/_release/." "$STAGE/"

rm -rf "$STAGE/userlibs" "$STAGE/temp"
find "$STAGE" -type f \( -name '*.pdb' -o -name '*.ilk' -o -name '*.aps' \
    -o -name '*.log' -o -name '*.bb' -o -name '*.exp' -o -name '*.lib' \) -delete
if [ -d "$STAGE/cfg" ]; then
    find "$STAGE/cfg" -maxdepth 1 -type f -name '*.ini' -delete
fi

SEVENZIP="$(b3d_find_7z)" || true
if [ -n "$SEVENZIP" ]; then
    (cd "$STAGE_ROOT" && "$SEVENZIP" a -tzip -mx=9 "$OUT" BlitzX3D >/dev/null)
elif b3d_have zip; then
    (cd "$STAGE_ROOT" && zip -9 -r "$OUT" BlitzX3D >/dev/null)
else
    b3d_die "7z or zip is required"
fi

rm -rf "$STAGE_ROOT"
b3d_info "wrote $OUT"
