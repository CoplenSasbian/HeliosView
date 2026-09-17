# =====================================================================
# reset-dependencies.ps1
#
# Resets the repository dependencies back to the pristine post-clone state:
#   1. Deinitializes and removes all git submodules:
#      - third_party/stdexec
#      - third_party/boost (and all selective Boost libraries)
#      - third_party/blend2d
#      - third_party/asmjit
#   2. Cleans up git submodule configuration and cached modules:
#      - Removes .git/modules/third_party cache (unless -KeepGitCache is specified)
#      - Prunes all submodule.* keys from .git/config
#   3. Cleans up all downloaded and extracted third-party binary resources:
#      - third_party/openssl/
#      - third_party/webview2-sdk/
#      - third_party/cacert.pem
#      - third_party/*.nupkg
#      - third_party/*.tmp
#
# SAFETY GUARANTEE:
#   - Does NOT touch your working branch, uncommitted code, or tracked files
#     (such as third_party/stb, src/, include/, etc.).
#
# Usage:
#   .\scripts\reset-dependencies.ps1
#   .\scripts\reset-dependencies.ps1 -KeepGitCache    # Keep git object cache
#   .\scripts\reset-dependencies.ps1 -OnlyDownloads   # Only clean downloaded packages
#   .\scripts\reset-dependencies.ps1 -OnlySubmodules  # Only clean git submodules
# =====================================================================

param(
    [switch]$KeepGitCache,
    [switch]$OnlyDownloads,
    [switch]$OnlySubmodules,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

Write-Host "==========================================================" -ForegroundColor Yellow
Write-Host " HeliosView Reset Dependencies" -ForegroundColor Yellow
Write-Host " Working directory: $RepoRoot" -ForegroundColor Yellow
Write-Host "==========================================================" -ForegroundColor Yellow

$cleanSubmodules = -not $OnlyDownloads
$cleanDownloads = -not $OnlySubmodules

# ---------------------------------------------------------------------
# 1. Reset Git Submodules
# ---------------------------------------------------------------------
if ($cleanSubmodules) {
    Write-Host "`n[1/2] Resetting Git Submodules..." -ForegroundColor Cyan

    $submodules = @(
        'third_party/stdexec',
        'third_party/boost',
        'third_party/blend2d',
        'third_party/asmjit'
    )

    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    if ($gitCmd) {
        # Deinitialize registered submodules
        foreach ($sm in $submodules) {
            Write-Host "  Deinitializing $sm..." -ForegroundColor DarkGray
            & git submodule deinit -f -- $sm 2>$null
        }

        # Prune submodule.* keys from .git/config (including nested boost libs)
        Write-Host "  Pruning submodule keys from .git/config..." -ForegroundColor DarkGray
        $stale = git config --get-regexp '^submodule\.' 2>$null
        foreach ($line in $stale) {
            $key = ($line -split '\s+')[0]
            if ($key) {
                & git config --unset "$key" 2>$null
            }
        }

        # Clean git module object cache under .git/modules/third_party
        if (-not $KeepGitCache) {
            $gitModulesDir = Join-Path $RepoRoot '.git\modules\third_party'
            if (Test-Path $gitModulesDir) {
                Write-Host "  Removing git module cache: $gitModulesDir..." -ForegroundColor DarkGray
                Remove-Item -Path $gitModulesDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        } else {
            Write-Host "  -KeepGitCache specified: keeping .git/modules/third_party cache." -ForegroundColor Green
        }
    } else {
        Write-Warning "git command not found; skipping git submodule deinit and proceeding with directory cleanup."
    }

    # Remove the submodule working directories specifically (preserves tracked files like third_party/stb)
    foreach ($sm in $submodules) {
        $smPath = Join-Path $RepoRoot $sm
        if (Test-Path $smPath) {
            Write-Host "  Removing worktree: $sm..." -ForegroundColor DarkGray
            Remove-Item -Path $smPath -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "  [DONE] Submodules have been completely deinitialized and removed." -ForegroundColor Green
} else {
    Write-Host "`n[1/2] Skipping submodules (-OnlyDownloads specified)." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------
# 2. Reset Downloaded Resources
# ---------------------------------------------------------------------
if ($cleanDownloads) {
    Write-Host "`n[2/2] Resetting Downloaded External Resources..." -ForegroundColor Cyan

    $downloadItems = @(
        'third_party/openssl',
        'third_party/webview2-sdk',
        'third_party/cacert.pem'
    )

    foreach ($item in $downloadItems) {
        $itemPath = Join-Path $RepoRoot $item
        if (Test-Path $itemPath) {
            Write-Host "  Removing $item..." -ForegroundColor DarkGray
            Remove-Item -Path $itemPath -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "  [REMOVED] $item" -ForegroundColor Green
        } else {
            Write-Host "  [NOT FOUND] $item (already clean)" -ForegroundColor DarkGray
        }
    }

    # Remove downloaded archives and temporary files (.nupkg, .tmp)
    $archives = Get-ChildItem -Path (Join-Path $RepoRoot 'third_party') -Include *.nupkg, *.tmp -File -ErrorAction SilentlyContinue
    foreach ($file in $archives) {
        Write-Host "  Removing archive $($file.Name)..." -ForegroundColor DarkGray
        Remove-Item -Path $file.FullName -Force -ErrorAction SilentlyContinue
        Write-Host "  [REMOVED] $($file.Name)" -ForegroundColor Green
    }

    Write-Host "  [DONE] Downloaded resources have been cleaned." -ForegroundColor Green
} else {
    Write-Host "`n[2/2] Skipping downloaded resources (-OnlySubmodules specified)." -ForegroundColor DarkGray
}

Write-Host "`n==========================================================" -ForegroundColor Yellow
Write-Host " Repository reset complete!" -ForegroundColor Green
Write-Host " Submodules are uninitialized and external resources removed." -ForegroundColor Green
Write-Host " Your development code and git branches are intact." -ForegroundColor Green
Write-Host " To re-initialize at any time, run:" -ForegroundColor Cyan
Write-Host "   .\scripts\setup-dependencies.cmd" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Yellow
