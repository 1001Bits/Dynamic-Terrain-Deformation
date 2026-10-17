@echo off
setlocal EnableDelayedExpansion

rem Configures and builds the Release DLL with Ninja + vcpkg, then runs the offline
rem shader and deformation checks. "build.cmd repair" rebuilds the CMake cache first.

set "ROOT=%~dp0"
set "BUILD=%ROOT%build"

set "VCVARS="
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo ERROR: no Visual Studio 2022 with the C++ x64 tools was found.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: vcvars64 failed.
    exit /b 1
)

if /i "%~1"=="repair" (
    echo Rebuilding the CMake cache...
    if exist "%BUILD%\CMakeCache.txt" del /q "%BUILD%\CMakeCache.txt"
    if exist "%BUILD%\CMakeFiles" rmdir /s /q "%BUILD%\CMakeFiles"
)

rem Reconfigure so the validation binaries are built even if an old cache disabled
rem them. Resolve the preset relative to this script, not the caller's directory.
if exist "%BUILD%\CMakeCache.txt" (
    rem Preserve the cache's absolute compiler and toolchain. Reapplying a preset
    rem compiler spelled "cl" can reset that cache and lose vcpkg dependencies.
    cmake -S "%ROOT%." -B "%BUILD%" -DBUILD_OFFLINE_TOOLS=ON || exit /b 1
) else (
    cmake --preset vs2022-ninja-windows-vcpkg -S "%ROOT%." -DBUILD_OFFLINE_TOOLS=ON || exit /b 1
)

cmake --build "%BUILD%" || exit /b 1

for %%T in (ShaderGen StampSurfaceTest SettingsTest DrawHookTest ProfilesTest ShaderStateTest) do (
    if not exist "%BUILD%\tools\offline\%%T.exe" (
        echo ERROR: required validation executable %%T.exe is missing.
        exit /b 1
    )
)

if exist "%BUILD%\tools\offline\ShaderGen.exe" (
    "%BUILD%\tools\offline\ShaderGen.exe" "%BUILD%\generated_shaders" > "%BUILD%\shader-check.log" 2>&1
    if errorlevel 1 (
        echo ERROR: the shader harness reported failures:
        type "%BUILD%\shader-check.log"
        exit /b 1
    )
    echo Shaders: ALL PASS
)

if exist "%BUILD%\tools\offline\StampSurfaceTest.exe" (
    "%BUILD%\tools\offline\StampSurfaceTest.exe" --textures "%ROOT%data\Data\Textures\NMN_DeformableTerrain" || exit /b 1
)

if exist "%BUILD%\tools\offline\SettingsTest.exe" (
    "%BUILD%\tools\offline\SettingsTest.exe" "%ROOT%data\Data\F4SE\Plugins\NMN_DeformableTerrain.ini" || exit /b 1
)

if exist "%BUILD%\tools\offline\DrawHookTest.exe" (
    "%BUILD%\tools\offline\DrawHookTest.exe" || exit /b 1
)

"%BUILD%\tools\offline\ShaderStateTest.exe" || exit /b 1

if exist "%BUILD%\tools\offline\ProfilesTest.exe" (
    "%BUILD%\tools\offline\ProfilesTest.exe" || exit /b 1
)

rem The in-game menu (MCM) must name settings the plugin knows, with the INI's defaults.
where python >nul 2>&1
if not errorlevel 1 (
    python -I "%ROOT%tools\offline\test_mcm_config.py" || exit /b 1
)

echo.
echo Built %BUILD%\src\NMN_DeformableTerrain.dll
endlocal
exit /b 0
