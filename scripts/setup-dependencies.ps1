# =====================================================================
# setup-dependencies.ps1
#
# HeliosView Dependency Setup Script
#
# Fetches all external dependencies outside of CMake configure:
#   1. Git submodules (stdexec, boost superproject + required boost libs,
#      blend2d, asmjit)
#   2. Prebuilt OpenSSL NuGet package (extracted to third_party/openssl)
#   3. WebView2 SDK NuGet package (extracted to third_party/webview2-sdk)
#   4. CA Certificate bundle (downloaded to third_party/cacert.pem)
#
# Usage:
#   .\scripts\setup-dependencies.ps1
#   .\scripts\setup-dependencies.ps1 -Force
#   .\scripts\setup-dependencies.ps1 -SkipSubmodules
#   .\scripts\setup-dependencies.ps1 -SkipDownloads
# =====================================================================

param(
    [switch]$Force,
    [switch]$SkipSubmodules,
    [switch]$SkipDownloads,
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " HeliosView Dependency Setup" -ForegroundColor Cyan
Write-Host " Working directory: $RepoRoot" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# ---------------------------------------------------------------------
# Helper: Download file with progress and fallback URLs
# ---------------------------------------------------------------------
function Invoke-DownloadWithFallback {
    param(
        [string[]]$Urls,
        [string]$DestinationPath,
        [string]$Description
    )

    if ((Test-Path $DestinationPath) -and (-not $Force)) {
        Write-Host "  [OK] $Description already exists: $DestinationPath" -ForegroundColor Green
        return $true
    }

    $destDir = Split-Path -Parent $DestinationPath
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }

    $tempFile = "$DestinationPath.tmp"
    if (Test-Path $tempFile) {
        Remove-Item $tempFile -Force -ErrorAction SilentlyContinue
    }

    foreach ($url in $Urls) {
        Write-Host "  Downloading $Description from: $url" -ForegroundColor Yellow
        try {
            # Use System.Net.WebClient or Invoke-WebRequest
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
            Invoke-WebRequest -Uri $url -OutFile $tempFile -UseBasicParsing -TimeoutSec 60
            if ((Test-Path $tempFile) -and ((Get-Item $tempFile).Length -gt 0)) {
                Move-Item -Path $tempFile -Destination $DestinationPath -Force
                Write-Host "  [SUCCESS] Saved to $DestinationPath" -ForegroundColor Green
                return $true
            }
        }
        catch {
            Write-Warning "  Failed downloading from $url : $_"
            if (Test-Path $tempFile) {
                Remove-Item $tempFile -Force -ErrorAction SilentlyContinue
            }
        }
    }

    Write-Error "Failed to download $Description from all sources."
    return $false
}

# ---------------------------------------------------------------------
# Helper: Extract zip/nupkg archive
# ---------------------------------------------------------------------
function Expand-ArchiveCustom {
    param(
        [string]$ArchivePath,
        [string]$DestinationPath
    )

    Write-Host "  Extracting $(Split-Path -Leaf $ArchivePath) -> $DestinationPath..." -ForegroundColor Yellow
    if (Test-Path $DestinationPath) {
        Remove-Item -Path $DestinationPath -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Path $DestinationPath -Force | Out-Null

    # Expand-Archive natively supports .zip and .nupkg (which is a zip)
    # Using .NET ZipFile to ensure reliable extraction across PowerShell versions
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $DestinationPath)
    Write-Host "  [SUCCESS] Extracted to $DestinationPath" -ForegroundColor Green
}

# ---------------------------------------------------------------------
# 1. Submodule Initialization
# ---------------------------------------------------------------------
if (-not $SkipSubmodules) {
    Write-Host "`n[1/4] Checking and Initializing Git Submodules..." -ForegroundColor Cyan

    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    if (-not $gitCmd) {
        Write-Error "Git is not installed or not in PATH. Cannot manage submodules."
        exit 1
    }

    $boostLibs = @(
        'asio', 'beast', 'json', 'container', 'endian', 'variant2', 'compat', 'system',
        'config', 'core', 'assert', 'throw_exception', 'static_assert', 'type_traits',
        'utility', 'detail', 'winapi', 'move', 'align', 'mp11', 'predef', 'optional',
        'smart_ptr', 'bind', 'intrusive', 'logic', 'static_string', 'container_hash',
        'describe', 'io'
    )

    $directSubmodules = @(
        'third_party/stdexec',
        'third_party/boost',
        'third_party/blend2d',
        'third_party/asmjit'
    )

    # Check uninitialized direct submodules
    $missingDirect = @()
    foreach ($sm in $directSubmodules) {
        if ($Force -or (-not (Test-Path "$RepoRoot/$sm/.git"))) {
            $missingDirect += $sm
        }
    }

    if ($missingDirect.Count -gt 0) {
        Write-Host "  Updating direct submodules ($($missingDirect.Count) to update)..." -ForegroundColor Yellow
        $args = @('submodule', 'update', '--init', '--depth', '1', '--progress', "--jobs=$Jobs", '--') + $missingDirect
        & git $args
        if ($LASTEXITCODE -ne 0) {
            Write-Error "git submodule update failed for direct submodules."
            exit 1
        }
    } else {
        Write-Host "  Direct submodules are already initialized." -ForegroundColor Green
    }

    # Check uninitialized Boost libraries
    $boostRoot = "$RepoRoot/third_party/boost"
    $missingBoostLibs = @()
    foreach ($lib in $boostLibs) {
        if ($Force -or (-not (Test-Path "$boostRoot/libs/$lib/.git"))) {
            $missingBoostLibs += "libs/$lib"
        }
    }

    if ($missingBoostLibs.Count -gt 0) {
        Write-Host "  Updating Boost libraries ($($missingBoostLibs.Count) libraries to update)..." -ForegroundColor Yellow
        Push-Location $boostRoot
        try {
            $args = @('submodule', 'update', '--init', '--depth', '1', '--progress', "--jobs=$Jobs", '--') + $missingBoostLibs
            & git $args
            if ($LASTEXITCODE -ne 0) {
                Write-Error "git submodule update failed for Boost libraries."
                exit 1
            }
        } finally {
            Pop-Location
        }
    } else {
        Write-Host "  All required Boost libraries are already initialized." -ForegroundColor Green
    }
} else {
    Write-Host "`n[1/4] Skipping submodules (-SkipSubmodules specified)." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------
# 2. OpenSSL (NuGet package)
# ---------------------------------------------------------------------
if (-not $SkipDownloads) {
    Write-Host "`n[2/4] Setting up OpenSSL (vcpkg NuGet package)..." -ForegroundColor Cyan
    $openSslVersion = "3.5.2"
    $openSslDestDir = "$RepoRoot/third_party/openssl"
    $openSslHeader = "$openSslDestDir/build/native/include/openssl/ssl.h"
    $openSslNupkg = "$RepoRoot/third_party/openssl.vcpkg.$openSslVersion.nupkg"

    if ((Test-Path $openSslHeader) -and (-not $Force)) {
        Write-Host "  OpenSSL already set up at $openSslDestDir" -ForegroundColor Green
    } else {
        $openSslUrls = @(
            "https://api.nuget.org/v3-flatcontainer/openssl.vcpkg/$openSslVersion/openssl.vcpkg.$openSslVersion.nupkg",
            "https://www.nuget.org/api/v2/package/openssl.vcpkg/$openSslVersion"
        )
        Invoke-DownloadWithFallback -Urls $openSslUrls -DestinationPath $openSslNupkg -Description "OpenSSL $openSslVersion NuGet"
        Expand-ArchiveCustom -ArchivePath $openSslNupkg -DestinationPath $openSslDestDir
    }

    # ---------------------------------------------------------------------
    # 3. CA Certificates bundle (cacert.pem)
    # ---------------------------------------------------------------------
    Write-Host "`n[3/4] Setting up CA Certificate Bundle (cacert.pem)..." -ForegroundColor Cyan
    $caCertPath = "$RepoRoot/third_party/cacert.pem"
    if ((Test-Path $caCertPath) -and (-not $Force)) {
        Write-Host "  cacert.pem already exists at $caCertPath" -ForegroundColor Green
    } else {
        $caCertUrls = @(
            "https://curl.se/ca/cacert.pem",
            "https://ghproxy.net/https://raw.githubusercontent.com/bagder/ca-bundle/master/ca-bundle.crt",
            "https://raw.githubusercontent.com/bagder/ca-bundle/master/ca-bundle.crt"
        )
        Invoke-DownloadWithFallback -Urls $caCertUrls -DestinationPath $caCertPath -Description "CA Certificate Bundle"
    }

    # ---------------------------------------------------------------------
    # 4. WebView2 SDK (NuGet package)
    # ---------------------------------------------------------------------
    Write-Host "`n[4/4] Setting up Microsoft WebView2 SDK..." -ForegroundColor Cyan
    $wv2Version = "1.0.4181-prerelease"
    $wv2DestDir = "$RepoRoot/third_party/webview2-sdk"
    $wv2Header = "$wv2DestDir/build/native/include/WebView2.h"
    $wv2Nupkg = "$RepoRoot/third_party/webview2.$wv2Version.nupkg"

    if ((Test-Path $wv2Header) -and (-not $Force)) {
        Write-Host "  WebView2 SDK already set up at $wv2DestDir" -ForegroundColor Green
    } else {
        $wv2Urls = @(
            "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$wv2Version/microsoft.web.webview2.$wv2Version.nupkg",
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$wv2Version"
        )
        Invoke-DownloadWithFallback -Urls $wv2Urls -DestinationPath $wv2Nupkg -Description "WebView2 SDK $wv2Version NuGet"
        Expand-ArchiveCustom -ArchivePath $wv2Nupkg -DestinationPath $wv2DestDir
    }
} else {
    Write-Host "`n[2..4] Skipping external downloads (-SkipDownloads specified)." -ForegroundColor DarkGray
}

Write-Host "`n==========================================================" -ForegroundColor Green
Write-Host " Dependencies setup complete! You can now configure CMake." -ForegroundColor Green
Write-Host " Example: cmake -B build -S . " -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
