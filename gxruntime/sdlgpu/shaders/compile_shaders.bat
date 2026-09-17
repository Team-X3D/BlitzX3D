@echo off
setlocal
set HERE=%~dp0
if "%DXC_EXE%"=="" set DXC_EXE=%HERE%..\..\..\tools\dxc-x64\dxc.exe
"%DXC_EXE%" -spirv -T vs_6_0 -E VSMain mesh.hlsl -Fo mesh_vs.spv || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMain mesh.hlsl -Fo mesh_ps.spv || exit /b 1
"%DXC_EXE%" -T vs_6_0 -E VSMain mesh.hlsl -Fo mesh_vs.dxil || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMain mesh.hlsl -Fo mesh_ps.dxil || exit /b 1
"%DXC_EXE%" -spirv -T vs_6_0 -E VSMainSkinned mesh.hlsl -Fo skin_vs.spv || exit /b 1
"%DXC_EXE%" -T vs_6_0 -E VSMainSkinned mesh.hlsl -Fo skin_vs.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMain2Tex mesh.hlsl -Fo mesh_ps2.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMain2Tex mesh.hlsl -Fo mesh_ps2.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMainExtra mesh.hlsl -Fo mesh_psextra.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMainExtra mesh.hlsl -Fo mesh_psextra.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMainCube mesh.hlsl -Fo mesh_pscube.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMainCube mesh.hlsl -Fo mesh_pscube.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMainCubeTex mesh.hlsl -Fo mesh_pscubetex.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMainCubeTex mesh.hlsl -Fo mesh_pscubetex.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMainTexCube mesh.hlsl -Fo mesh_pstexcube.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMainTexCube mesh.hlsl -Fo mesh_pstexcube.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMainCubeCube mesh.hlsl -Fo mesh_pscubecube.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMainCubeCube mesh.hlsl -Fo mesh_pscubecube.dxil || exit /b 1
"%DXC_EXE%" -spirv -T vs_6_0 -E VSMain canvas.hlsl -Fo canvas_vs.spv || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMain canvas.hlsl -Fo canvas_ps.spv || exit /b 1
"%DXC_EXE%" -T vs_6_0 -E VSMain canvas.hlsl -Fo canvas_vs.dxil || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMain canvas.hlsl -Fo canvas_ps.dxil || exit /b 1
"%DXC_EXE%" -spirv -T vs_6_0 -E VSMain text.hlsl -Fo text_vs.spv || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMain text.hlsl -Fo text_ps.spv || exit /b 1
"%DXC_EXE%" -T vs_6_0 -E VSMain text.hlsl -Fo text_vs.dxil || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMain text.hlsl -Fo text_ps.dxil || exit /b 1
"%DXC_EXE%" -spirv -T ps_6_0 -E PSMain present.hlsl -Fo present_ps.spv || exit /b 1
"%DXC_EXE%" -T ps_6_0 -E PSMain present.hlsl -Fo present_ps.dxil || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%embed_shaders.ps1" || exit /b 1
