@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ================================================================
echo DLSS Cam Demo - Windows x64 D3D12 / NGX / Media Foundation
echo ================================================================

echo [0/4] Locating build tools...

set "GIT_EXE="
for /f "delims=" %%I in ('where git.exe 2^>nul') do if not defined GIT_EXE set "GIT_EXE=%%I"
if not defined GIT_EXE if exist "%ProgramFiles%\Git\cmd\git.exe" set "GIT_EXE=%ProgramFiles%\Git\cmd\git.exe"
if not defined GIT_EXE if exist "%ProgramFiles(x86)%\Git\cmd\git.exe" set "GIT_EXE=%ProgramFiles(x86)%\Git\cmd\git.exe"
if not defined GIT_EXE (
    echo [ERROR] git.exe not found. Install Git for Windows.
    pause
    exit /b 1
)
echo [INFO] Git   : "%GIT_EXE%"

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles(x86)%\CMake\bin\cmake.exe"
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined CMAKE_EXE if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
        if exist "%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)
if not defined CMAKE_EXE (
    echo.
    echo [ERROR] CMake was not found in PATH or inside Visual Studio 2022.
    echo Install "C++ CMake tools for Windows" in Visual Studio Installer.
    pause
    exit /b 1
)
echo [INFO] CMake : "%CMAKE_EXE%"

set "VS_INSTALL="
for %%P in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VS_INSTALL if exist "%%~P\Microsoft Visual Studio\2022\%%E\VC\Tools\MSVC" (
            set "VS_INSTALL=%%~P\Microsoft Visual Studio\2022\%%E"
        )
    )
)
if defined VS_INSTALL (
    echo [INFO] VS2022: "!VS_INSTALL!"
) else (
    echo [WARN] VS2022 C++ toolset not verified; CMake will try to locate it.
)

if not exist "external\DLSS\include\nvsdk_ngx.h" (
    echo.
    echo [1/4] Cloning official NVIDIA DLSS SDK...
    if not exist "external" mkdir "external"
    "%GIT_EXE%" clone --depth 1 https://github.com/NVIDIA/DLSS.git "external\DLSS"
    if errorlevel 1 (
        echo [ERROR] Failed to clone NVIDIA/DLSS.
        pause
        exit /b 1
    )
) else (
    echo.
    echo [1/4] NVIDIA DLSS SDK already present. Updating official checkout...
    "%GIT_EXE%" -C "external\DLSS" pull --ff-only
    if errorlevel 1 (
        echo [WARN] Could not update NVIDIA/DLSS; continuing with the existing checkout.
    )
)

if not exist "external\onnxruntime\include\onnxruntime_cxx_api.h" (
    echo.
    echo [ERROR] ONNX Runtime was not found in external\onnxruntime.
    echo Download onnxruntime-win-x64 and extract it there ^(README.md, "Required"^), then run this BAT again.
    pause
    exit /b 1
)
echo [INFO] ONNX Runtime ready: external\onnxruntime

echo.
echo [2/4] Configuring Visual Studio 2022 x64...
"%CMAKE_EXE%" -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    pause
    exit /b 1
)

echo.
echo [3/4] Building Release...
"%CMAKE_EXE%" --build build --config Release --parallel
if errorlevel 1 (
    echo [ERROR] Build failed. Send the compiler output above.
    pause
    exit /b 1
)

echo.
echo [4/4] Verifying the build output...
if not exist "build\Release\DLSSCamDemo.exe" (
    echo [ERROR] Build reported success but the EXE was not found.
    pause
    exit /b 1
)

rem CMake stages everything beside the EXE as a POST_BUILD step: onnxruntime.dll,
rem the official nvngx_dlss.dll, the docs, whatever the user placed in runtime\
rem (ReShade, the NR runtime, an NR consumer), models\rvm_mobilenetv3_fp32.onnx,
rem and the config\*.template seeds. Nothing has to be copied here; only report
rem what the optional pieces would have added.
if not exist "build\Release\nvngx_dlss.dll" echo [WARN] nvngx_dlss.dll is missing beside the EXE.
if not exist "build\Release\onnxruntime.dll" echo [WARN] onnxruntime.dll is missing beside the EXE.
if not exist "build\Release\rvm_mobilenetv3_fp32.onnx" echo [WARN] models\rvm_mobilenetv3_fp32.onnx was not staged; person segmentation stays off.
if not exist "build\Release\dxgi.dll" echo [WARN] No ReShade dxgi.dll in runtime\; Neural Rendering stays off.
if not exist "build\Release\nvngx_dlssnr.dll" echo [WARN] No nvngx_dlssnr.dll in runtime\; Neural Rendering stays off.

echo.
echo ================================================================
echo [OK] build\Release\DLSSCamDemo.exe
echo ================================================================
echo.
echo Native DLSS SR is self-contained in build\Release.
echo For DLSS 5 Neural Rendering, put ReShade, the NR runtime and one NR consumer
echo in runtime\ and build again; CMake stages them next to the EXE.
echo See README.md for the required layout.
echo.
pause
