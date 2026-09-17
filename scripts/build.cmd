@echo off
setlocal

for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"

set "BUILD_DIR=%REPO_ROOT%\out\build\x64-Debug"
if not exist "%BUILD_DIR%" set "BUILD_DIR=%REPO_ROOT%\cmake-build-debug"
if not exist "%BUILD_DIR%" set "BUILD_DIR=%REPO_ROOT%\build"

rem Check if cl.exe is already on PATH
where cl.exe >nul 2>nul
if %errorlevel% equ 0 goto :find_cmake

rem Try locating VS via vswhere or standard directories
set "VS_PATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)

if not defined VS_PATH (
    if exist "E:\Microsoft Visual Studio\18 insider" set "VS_PATH=E:\Microsoft Visual Studio\18 insider"
)
if not defined VS_PATH (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community" set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
)
if not defined VS_PATH (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
)
if not defined VS_PATH (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
)

if not defined VS_PATH (
    echo [build] Could not locate Visual Studio installation.
    exit /b 1
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [build] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] vcvars64.bat failed
    exit /b 1
)

:find_cmake
where cmake.exe >nul 2>nul
if %errorlevel% equ 0 (
    set "CMAKE=cmake"
) else if defined VS_PATH (
    set "CMAKE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE (
    echo [build] cmake.exe not found on PATH or in Visual Studio.
    exit /b 1
)

if not exist "%BUILD_DIR%" (
    echo [build] Build directory not found: "%BUILD_DIR%"
    echo [build] Please configure CMake first: cmake -S . -B out\build\x64-Debug
    exit /b 1
)

if "%~1"=="" (
    echo [build] "%CMAKE%" --build "%BUILD_DIR%"
    "%CMAKE%" --build "%BUILD_DIR%"
) else (
    echo [build] "%CMAKE%" --build "%BUILD_DIR%" --target %*
    "%CMAKE%" --build "%BUILD_DIR%" --target %*
)
exit /b %errorlevel%
