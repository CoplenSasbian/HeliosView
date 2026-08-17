# =====================================================================
# clean-submodules.ps1
#
# Fully deinitializes HeliosView's vendored git submodules so you can
# re-test cmake/ensure-submodule.cmake from a clean slate.
#
# WHY YOU NEED THIS
#   The auto-config script (cmake/ensure-submodule.cmake) is designed to
#   be a no-op when submodules already look healthy. So once they have been
#   cloned on a previous CMake run, re-running CMake after editing the
#   script will NOT exercise the "init from scratch" path you just want to
#   verify. This script wipes them so the next CMake run has to rebuild
#   everything from an empty third_party/.
#
# WHAT THIS REMOVES
#   1. third_party/boost, third_party/json, third_party/stdexec worktrees
#   2. The submodule entries from .git/config (git submodule deinit -f)
#   3. The cloned-objects cache under .git/modules/third_party/
#      (set -KeepCache to keep it and avoid a full re-download)
#   4. Stale submodule.* config keys left in .git/config by nested Boost
#      lib initializations (asio, beast, ...) -- harmless to build, but
#      they would leave a non-pristine local repo.
#
# It does NOT touch .gitmodules or the superproject gitlinks in the index:
# those stay in place so `git submodule update` can rebuild on the next
# CMake configure.
#
# USAGE
#   PowerShell:
#       .\scripts\clean-submodules.ps1            # full clean
#       .\scripts\clean-submodules.ps1 -KeepCache # keep re-downloadable cache
#
#   Bash / git-bash (equivalent one-liner):
#       git submodule deinit -f third_party/boost third_party/json third_party/stdexec
#       rm -rf third_party ; rm -rf .git/modules/third_party
#       git config --get-regexp '^submodule\.' | cut -d' ' -f1 \
#            | xargs -r git config --unset
#
#   Then re-run CMake configure. The script recreates every submodule.
# =====================================================================
param(
    [switch]$KeepCache
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
Write-Host "Working directory: $root"

# The direct submodules HeliosView vendors. Keep in sync with the
# HELIOSVIEW_DIRECT_SUBMODULES list + boost in cmake/ensure-submodule.cmake.
$submodules = @('third_party/boost', 'third_party/json', 'third_party/stdexec')

# 1) Unregister each submodule (this also clears the working-tree files).
Write-Host ">> git submodule deinit -f"
foreach ($sm in $submodules) {
    git submodule deinit -f -- $sm
    # NB: keep the loop simple; the noisy "cleared directory"/"could not
    # create empty" stderr output here is harmless.
    Write-Host "   deinitialized: $sm"
}
# Robust sweep: `third_party` contains ONLY submodule checkouts (all three
# gitlinks in .gitmodules live under third_party/), so after every submodule
# is deinitialized we can drop the whole tree. A plain per-submodule Remove-Item
# is not enough because git keeps re-materializing EMPTY shells for gitlinks
# still present in the index, so any leftover empty dirs are removed here too.
$tp = Join-Path $root 'third_party'
if (Test-Path $tp -PathType Container) {
    Write-Host ">> Removing third_party/ worktrees"
    Remove-Item $tp -Recurse -Force -ErrorAction SilentlyContinue
}

# 2) Drop the cloned-objects cache so the re-test is truly from scratch.
if (-not $KeepCache) {
    $modulesDir = Join-Path $root '.git\modules\third_party'
    if (Test-Path $modulesDir) {
        Write-Host ">> Removing .git/modules/third_party cache"
        Remove-Item $modulesDir -Recurse -Force
    }
} else {
    Write-Host ">> -KeepCache: keeping .git/modules cache (next fetch reuses it)"
}

# 3) Prune stale submodule.* keys from .git/config. Nested Boost lib
#    initialization writes these; they are not in .gitmodules, so deinit
#    does not remove them.
Write-Host ">> Pruning stale submodule.* keys from .git/config"
$stale = git config --get-regexp '^submodule\.' 2>$null
foreach ($line in $stale) {
    $key = ($line -split '\s+')[0]
    git config --unset "$key"
}

Write-Host ""
Write-Host "Done. third_party/ is now empty; .gitmodules and gitlinks are intact."
Write-Host "Re-run CMake configure to rebuild all submodules from scratch."
Write-Host "To verify after configuring:  git submodule status"
