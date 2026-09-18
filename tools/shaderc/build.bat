@echo off
setlocal
set HERE=%~dp0
set MSBUILD=msbuild
where msbuild >nul 2>nul
if errorlevel 1 (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
)
"%MSBUILD%" "%HERE%shaderc.vcxproj" /p:Configuration=Release /p:Platform=Win32 /m /v:minimal
