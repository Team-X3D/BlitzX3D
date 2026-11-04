#!/usr/bin/env bash

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DL_DIR="$B3D_TOOLCHAIN/downloads"
SRC_DIR="$B3D_TOOLCHAIN/msvc-wine-$B3D_MSVC_WINE_REV"

DXSDK_URL="https://download.microsoft.com/download/A/E/7/AE743F1F-632B-4809-87A9-AA1BB3458E31/DXSDK_Jun10.exe"
DXSDK_FILE="$DL_DIR/DXSDK_Jun10.exe"

LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-win32.exe"
LLVM_FILE="$DL_DIR/LLVM-21.1.8-win32.exe"

mkdir -p "$DL_DIR" "$B3D_TOOLCHAIN"

WINE="$(b3d_find_wine)" || true
[ -n "$WINE" ] || b3d_die "wine not found. install wine and wine-mono"

missing=()
for tool in curl tar python3 msiextract; do
    b3d_have "$tool" || missing+=("$tool")
done
SEVENZIP="$(b3d_find_7z)" || true
[ -n "$SEVENZIP" ] || missing+=("7z")
if [ "${#missing[@]}" -ne 0 ]; then
    b3d_die "missing tools: ${missing[*]}"
fi

if ! b3d_have ntlm_auth && [ ! -x /usr/bin/ntlm_auth ] && [ ! -x /usr/sbin/ntlm_auth ]; then
    b3d_warn "winbind not found. build may fail"
fi

printf 'toolchain: %s\n' "$B3D_TOOLCHAIN"
printf 'wine: %s\n' "$WINE"
printf '7-zip: %s\n' "$SEVENZIP"

if [ ! -f "$WINEPREFIX/system.reg" ]; then
    b3d_info "creating wine prefix"
    "$WINE" wineboot --init >/dev/null 2>&1 || true
    if b3d_have wineserver; then
        wineserver -w >/dev/null 2>&1 || true
    fi
fi

if [ ! -d "$WINEPREFIX/drive_c/windows/mono" ]; then
    mono_msi="$(find /usr/share/wine /usr/share/wine-mono /usr/lib/wine /usr/lib64/wine \
        -iname 'wine-mono-*.msi' 2>/dev/null | head -n 1 || true)"

    if [ -z "$mono_msi" ]; then
        mono_msi="$DL_DIR/wine-mono-8.1.0-x86.msi"
        if [ ! -f "$mono_msi" ]; then
            b3d_info "downloading wine mono"
            curl -fL --retry 3 -o "$mono_msi.part" \
                "https://github.com/madewokherd/wine-mono/releases/download/wine-mono-8.1.0/wine-mono-8.1.0-x86.msi"
            mv "$mono_msi.part" "$mono_msi"
        fi
    fi

    b3d_info "installing wine mono"
    if ! "$WINE" msiexec /i "$mono_msi" /qn >/dev/null 2>&1; then
        b3d_warn "wine mono install failed"
        b3d_warn "install wine-mono and retry"
    fi
    if b3d_have wineserver; then
        wineserver -w >/dev/null 2>&1 || true
    fi
fi

if [ ! -f "$SRC_DIR/vsdownload.py" ] || [ ! -f "$SRC_DIR/install.sh" ]; then
    b3d_info "fetching msvc-wine"
    tmp="$DL_DIR/msvc-wine.tar.gz"
    curl -fL --retry 3 -o "$tmp" \
        "https://github.com/mstorsjo/msvc-wine/archive/$B3D_MSVC_WINE_REV.tar.gz"
    rm -rf "$SRC_DIR"
    mkdir -p "$SRC_DIR"
    tar -xzf "$tmp" -C "$SRC_DIR" --strip-components=1
fi

b3d_info "patching vsdownload"
python3 - "$SRC_DIR/vsdownload.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()

def add(after, addition):
    global src
    if addition.strip() in src:
        return
    if src.count(after) != 1:
        sys.exit("cannot patch %s: anchor found %d times: %r" % (path, src.count(after), after))
    src = src.replace(after, after + addition, 1)

add('    parser.add_argument("--with-atl", action=OptionalBoolean, help="Include ATL (default)")\n',
    '    parser.add_argument("--with-mfc", action=OptionalBoolean, help="Include MFC")\n')
add('            appendPackageSelection(args, args.with_atl, "Microsoft.VisualStudio.Component.VC." + toolversion + ".ATL")\n',
    '            appendPackageSelection(args, args.with_mfc, "Microsoft.VisualStudio.Component.VC." + toolversion + ".MFC")\n')
add('        appendPackageSelection(args, args.with_atl, "Microsoft.VisualStudio.Component.VC.ATL")\n',
    '        appendPackageSelection(args, args.with_mfc, "Microsoft.VisualStudio.Component.VC.ATLMFC")\n')

open(path, "w", encoding="utf-8").write(src)
PY

if grep -q 'VCToolsInstallDir_180' "$SRC_DIR/wrappers/msbuild" 2>/dev/null; then
    b3d_info "patching msvc-wine wrapper"
    sed -i 's/VCToolsInstallDir_180/VCToolsInstallDir_170/g; s/VCInstallDir_180/VCInstallDir_170/g' \
        "$SRC_DIR/wrappers/msbuild"
fi

if [ ! -f "$B3D_TOOLCHAIN/.msvc-ready" ]; then
    if [ ! -x "$B3D_MSVC_DIR/bin/x86/msbuild" ]; then
        b3d_info "downloading MSVC and MFC"
        b3d_info "large download, this takes a while"
        (
            cd "$SRC_DIR"
            python3 vsdownload.py \
                --accept-license \
                --dest "$B3D_MSVC_DIR" \
                --cache "$DL_DIR/msvc" \
                --major "$B3D_VS_MAJOR" \
                --msvc-version "$B3D_MSVC_VERSION" \
                --architecture x86 x64 \
                --with-mfc \
                --with-msbuild \
                --with-asan no \
                --with-devcmd no \
                --language en
        )
    fi
    b3d_info "unpacking MSVC"
    sh "$SRC_DIR/install.sh" "$B3D_MSVC_DIR"
    touch "$B3D_TOOLCHAIN/.msvc-ready"
else
    b3d_info "MSVC already installed"
fi

[ -x "$B3D_MSVC_DIR/bin/x86/msbuild" ] || b3d_die "MSVC install incomplete. run setup again"
[ -f "$B3D_MSVC_DIR/MSBuild/Current/Bin/amd64/MSBuild.exe" ] || b3d_die "MSBuild missing. run setup again"

if [ ! -f "$B3D_DXSDK_DIR/Include/d3dx9.h" ] || [ ! -f "$B3D_DXSDK_DIR/Lib/x86/d3dx9.lib" ]; then
    if [ ! -f "$DXSDK_FILE" ]; then
        b3d_info "downloading DirectX SDK"
        curl -fL --retry 3 -o "$DXSDK_FILE.part" "$DXSDK_URL"
        mv "$DXSDK_FILE.part" "$DXSDK_FILE"
    fi
    b3d_info "extracting DirectX SDK"
    rm -rf "$B3D_TOOLCHAIN/dxsdk"
    mkdir -p "$B3D_TOOLCHAIN/dxsdk"
    "$SEVENZIP" x -y "-o$B3D_TOOLCHAIN/dxsdk" "$DXSDK_FILE" \
        'DXSDK/Include/*' 'DXSDK/Lib/x86/*' >/dev/null
fi

[ -f "$B3D_DXSDK_DIR/Include/d3dx9.h" ] || b3d_die "d3dx9.h missing"
[ -f "$B3D_DXSDK_DIR/Lib/x86/d3dx9.lib" ] || b3d_die "d3dx9.lib missing"

if [ ! -f "$B3D_ROOT/vendor/llvm32/bin/LLVM-C.dll" ] || [ ! -f "$B3D_ROOT/vendor/llvm32/lib/LLVM-C.lib" ]; then
    if [ ! -f "$LLVM_FILE" ]; then
        b3d_info "downloading LLVM"
        curl -fL --retry 3 -o "$LLVM_FILE.part" "$LLVM_URL"
        mv "$LLVM_FILE.part" "$LLVM_FILE"
    fi
    b3d_info "extracting LLVM"
    rm -rf "$B3D_TOOLCHAIN/llvm"
    mkdir -p "$B3D_TOOLCHAIN/llvm"
    "$SEVENZIP" x -y "-o$B3D_TOOLCHAIN/llvm" "$LLVM_FILE" \
        'bin/LLVM-C.dll' 'lib/LLVM-C.lib' 'include/llvm-c/*' >/dev/null

    dll="$(find "$B3D_TOOLCHAIN/llvm" -name LLVM-C.dll -print -quit)"
    lib="$(find "$B3D_TOOLCHAIN/llvm" -name LLVM-C.lib -print -quit)"
    [ -n "$dll" ] || b3d_die "LLVM-C.dll missing"
    [ -n "$lib" ] || b3d_die "LLVM-C.lib missing"

    mkdir -p "$B3D_ROOT/vendor/llvm32/bin" "$B3D_ROOT/vendor/llvm32/lib"
    install -m 644 "$dll" "$B3D_ROOT/vendor/llvm32/bin/LLVM-C.dll"
    install -m 644 "$lib" "$B3D_ROOT/vendor/llvm32/lib/LLVM-C.lib"
    if [ -d "$B3D_TOOLCHAIN/llvm/include" ]; then
        mkdir -p "$B3D_ROOT/vendor/llvm32/include"
        cp -a "$B3D_TOOLCHAIN/llvm/include/." "$B3D_ROOT/vendor/llvm32/include/"
    fi
fi

[ -f "$B3D_ROOT/vendor/llvm32/bin/LLVM-C.dll" ] || b3d_die "LLVM-C.dll missing"
[ -f "$B3D_ROOT/vendor/llvm32/lib/LLVM-C.lib" ] || b3d_die "LLVM-C.lib missing"

if [ ! -f "$B3D_ROOT/vendor/llvm32/include/llvm/Config/llvm-config.h" ]; then
    mkdir -p "$B3D_ROOT/vendor/llvm32/include/llvm/Config"
    cat > "$B3D_ROOT/vendor/llvm32/include/llvm/Config/llvm-config.h" <<'EOF'
#ifndef LLVM_CONFIG_H
#define LLVM_CONFIG_H

#define LLVM_VERSION_MAJOR 21
#define LLVM_VERSION_MINOR 1
#define LLVM_VERSION_PATCH 8
#define LLVM_VERSION_SUFFIX ""
#define LLVM_VERSION_STRING "21.1.8"

#define LLVM_ENABLE_THREADS 1
#define LLVM_ENABLE_LLVM_C_EXPORT_ANNOTATIONS 1
#define LLVM_DEFAULT_TARGET_TRIPLE "i686-pc-windows-msvc"
#define LLVM_HOST_TRIPLE "i686-pc-windows-msvc"

#endif
EOF
fi

LLVM_CONFIG_DIR="$B3D_ROOT/vendor/llvm32/include/llvm/Config"
mkdir -p "$LLVM_CONFIG_DIR"

if [ ! -f "$LLVM_CONFIG_DIR/Targets.def" ]; then
    cat > "$LLVM_CONFIG_DIR/Targets.def" <<'EOF'
#ifndef LLVM_TARGET
#error Please define the macro LLVM_TARGET(TargetName)
#endif

LLVM_TARGET(X86)

#undef LLVM_TARGET
EOF
fi

if [ ! -f "$LLVM_CONFIG_DIR/AsmPrinters.def" ]; then
    cat > "$LLVM_CONFIG_DIR/AsmPrinters.def" <<'EOF'
#ifndef LLVM_ASM_PRINTER
#error Please define the macro LLVM_ASM_PRINTER(TargetName)
#endif

LLVM_ASM_PRINTER(X86)

#undef LLVM_ASM_PRINTER
EOF
fi

if [ ! -f "$LLVM_CONFIG_DIR/AsmParsers.def" ]; then
    cat > "$LLVM_CONFIG_DIR/AsmParsers.def" <<'EOF'
#ifndef LLVM_ASM_PARSER
#error Please define the macro LLVM_ASM_PARSER(TargetName)
#endif

LLVM_ASM_PARSER(X86)

#undef LLVM_ASM_PARSER
EOF
fi

if [ ! -f "$LLVM_CONFIG_DIR/Disassemblers.def" ]; then
    cat > "$LLVM_CONFIG_DIR/Disassemblers.def" <<'EOF'
#ifndef LLVM_DISASSEMBLER
#error Please define the macro LLVM_DISASSEMBLER(TargetName)
#endif

LLVM_DISASSEMBLER(X86)

#undef LLVM_DISASSEMBLER
EOF
fi

b3d_info "toolchain ready"
b3d_info "run ./linux/build.sh"
