param([string]$Dir = $PSScriptRoot)

$headers = [ordered]@{
    'mesh_shaders.h' = @(
        @('kMeshVS_DXIL', 'mesh_vs.dxil'), @('kMeshVS_SPIRV', 'mesh_vs.spv'),
        @('kMeshPS1_DXIL', 'mesh_ps1.dxil'), @('kMeshPS1_SPIRV', 'mesh_ps1.spv'),
        @('kMeshPS2_DXIL', 'mesh_ps2.dxil'), @('kMeshPS2_SPIRV', 'mesh_ps2.spv'),
        @('kMeshPS3_DXIL', 'mesh_ps3.dxil'), @('kMeshPS3_SPIRV', 'mesh_ps3.spv'),
        @('kMeshPS4_DXIL', 'mesh_ps4.dxil'), @('kMeshPS4_SPIRV', 'mesh_ps4.spv'),
        @('kMeshPS5_DXIL', 'mesh_ps5.dxil'), @('kMeshPS5_SPIRV', 'mesh_ps5.spv'),
        @('kMeshPS6_DXIL', 'mesh_ps6.dxil'), @('kMeshPS6_SPIRV', 'mesh_ps6.spv'),
        @('kMeshPS7_DXIL', 'mesh_ps7.dxil'), @('kMeshPS7_SPIRV', 'mesh_ps7.spv'),
        @('kMeshPS8_DXIL', 'mesh_ps8.dxil'), @('kMeshPS8_SPIRV', 'mesh_ps8.spv'),
        @('kSkinVS_DXIL', 'skin_vs.dxil'), @('kSkinVS_SPIRV', 'skin_vs.spv'),
        @('kMeshPSCube_DXIL', 'mesh_pscube.dxil'), @('kMeshPSCube_SPIRV', 'mesh_pscube.spv'),
        @('kMeshPSCubeTex_DXIL', 'mesh_pscubetex.dxil'), @('kMeshPSCubeTex_SPIRV', 'mesh_pscubetex.spv'),
        @('kMeshPSTexCube_DXIL', 'mesh_pstexcube.dxil'), @('kMeshPSTexCube_SPIRV', 'mesh_pstexcube.spv'),
        @('kMeshPSCubeCube_DXIL', 'mesh_pscubecube.dxil'), @('kMeshPSCubeCube_SPIRV', 'mesh_pscubecube.spv')
    )
    'canvas_shaders.h' = @(
        @('kCanvasVS_DXIL', 'canvas_vs.dxil'), @('kCanvasPS_DXIL', 'canvas_ps.dxil'),
        @('kCanvasVS_SPIRV', 'canvas_vs.spv'), @('kCanvasPS_SPIRV', 'canvas_ps.spv')
    )
    'text_shaders.h' = @(
        @('kTextVS_DXIL', 'text_vs.dxil'), @('kTextPS_DXIL', 'text_ps.dxil'),
        @('kTextVS_SPIRV', 'text_vs.spv'), @('kTextPS_SPIRV', 'text_ps.spv')
    )
    'present_shaders.h' = @(
        @('kPresentPS_DXIL', 'present_ps.dxil'), @('kPresentPS_SPIRV', 'present_ps.spv')
    )
}

foreach ($header in $headers.Keys) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('#pragma once')
    $lines.Add('#include <stddef.h>')
    $lines.Add('#include <stdint.h>')
    $lines.Add('')
    foreach ($entry in $headers[$header]) {
        $name = $entry[0]
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Dir $entry[1]))
        $lines.Add('static const uint8_t ' + $name + '[' + $bytes.Length + '] = {')
        for ($i = 0; $i -lt $bytes.Length; $i += 12) {
            $n = [Math]::Min(12, $bytes.Length - $i)
            $parts = for ($j = 0; $j -lt $n; $j++) { '0x{0:x2}' -f $bytes[$i + $j] }
            $lines.Add('    ' + ($parts -join ', ') + ',')
        }
        $lines.Add('};')
        $lines.Add('static const size_t ' + $name + '_size = sizeof(' + $name + ');')
        $lines.Add('')
    }
    if ($lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    [System.IO.File]::WriteAllLines((Join-Path $Dir $header), $lines)
}
