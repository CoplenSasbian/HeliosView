@echo off
rem HeliosView - build in the CLion build tree with the VS 18 (insider) toolchain.
rem
rem Run this from a terminal (or through the IDE's MCP terminal) when cmake.exe is
rem not on the PATH and the MSVC environment is not set up. vcvars64.bat supplies
rem INCLUDE/LIB/PATH for cl.exe; the rest is a plain cmake --build of the existing
rem cmake-build-debug tree (no reconfigure, so nothing is downloaded).
rem
rem Usage:  scripts\build.cmd                    -- build everything (default target)
rem         scripts\build.cmd HeliosView          -- one target
rem         scripts\build.cmd HeliosView CanvasTest ...  (targets are space separated)

setlocal
set "VCVARS=E:\Microsoft Visual Studio\18 insider\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=E:\Microsoft Visual Studio\18 insider\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD_DIR=%~dp0..\cmake-build-debug"

if not exist "%CMAKE%" (
    echo [build] cmake not found at "%CMAKE%"
    exit /b 1
)
if not exist "%VCVARS%" (
    echo [build] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] vcvars64.bat failed
    exit /b 1
)

if "%~1"=="" (
    echo [build] cmake --build "%BUILD_DIR%"
    "%CMAKE%" --build "%BUILD_DIR%"
) else (
    echo [build] cmake --build "%BUILD_DIR%" --target %*
    "%CMAKE%" --build "%BUILD_DIR%" --target %*
)
exit /b %errorlevel%
