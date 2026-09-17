#!/usr/bin/env python3
"""
reset-dependencies.py

Resets the repository dependencies back to pristine post-clone state:
Deinitializes git submodules and removes downloaded external binary resources,
without touching your working code, tracked files, or git branches.

Environment Considerations:
- Identifies OS platform: cleanly cleans Windows-specific binary downloads on Windows,
  and skips non-existent Windows resources on other systems.
- Resolves 'git' executable via PATH across environments.
"""

import argparse
import glob
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

SUBMODULES = [
    'third_party/stdexec',
    'third_party/boost',
    'third_party/blend2d',
    'third_party/asmjit'
]

DOWNLOAD_DIRS = [
    'third_party/openssl',
    'third_party/webview2-sdk'
]

DOWNLOAD_FILES = [
    'third_party/cacert.pem'
]

def reset_submodules(repo_root, keep_git_cache=False):
    print("\n[1/2] Resetting Git Submodules...")
    git_bin = shutil.which("git")
    if git_bin:
        # 1. git submodule deinit
        for sm in SUBMODULES:
            print(f"  Deinitializing {sm}...")
            subprocess.run([git_bin, 'submodule', 'deinit', '-f', '--', sm], cwd=repo_root, capture_output=True)

        # 2. Prune stale submodule keys from .git/config
        res = subprocess.run([git_bin, 'config', '--get-regexp', '^submodule\\.'], cwd=repo_root, capture_output=True, text=True)
        if res.stdout:
            for line in res.stdout.strip().splitlines():
                parts = line.split()
                if parts:
                    subprocess.run([git_bin, 'config', '--unset', parts[0]], cwd=repo_root, capture_output=True)

        # 3. Clean .git/modules/third_party
        if not keep_git_cache:
            modules_dir = repo_root / '.git' / 'modules' / 'third_party'
            if modules_dir.exists():
                print(f"  Removing git module cache: {modules_dir}...")
                shutil.rmtree(modules_dir, ignore_errors=True)
        else:
            print("  --keep-git-cache specified: keeping .git/modules/third_party cache.")
    else:
        print("  Warning: 'git' executable not found. Skipping git deinit, removing worktrees directly.")

    # 4. Remove worktree directories specifically (never touches tracked repo files like third_party/stb)
    for sm in SUBMODULES:
        sm_path = repo_root / sm
        if sm_path.exists():
            print(f"  Removing worktree: {sm}...")
            shutil.rmtree(sm_path, ignore_errors=True)

    print("  [DONE] Submodules have been completely deinitialized and removed.")

def reset_downloads(repo_root):
    print("\n[2/2] Resetting Downloaded External Resources...")
    for d in DOWNLOAD_DIRS:
        p = repo_root / d
        if p.exists():
            print(f"  Removing {d}...")
            shutil.rmtree(p, ignore_errors=True)
            print(f"  [REMOVED] {d}")

    for f in DOWNLOAD_FILES:
        p = repo_root / f
        if p.exists():
            print(f"  Removing {f}...")
            p.unlink(missing_ok=True)
            print(f"  [REMOVED] {f}")

    # Remove archives (*.nupkg, *.tmp) in third_party
    tp = repo_root / 'third_party'
    if tp.exists():
        for f in tp.glob('*.nupkg'):
            f.unlink(missing_ok=True)
            print(f"  [REMOVED] {f.name}")
        for f in tp.glob('*.tmp'):
            f.unlink(missing_ok=True)
            print(f"  [REMOVED] {f.name}")

    print("  [DONE] Downloaded resources have been cleaned.")

def main():
    parser = argparse.ArgumentParser(description="HeliosView Reset Dependencies")
    parser.add_argument("--keep-git-cache", action="store_true", help="Keep .git/modules cache to speed up next fetch")
    parser.add_argument("--only-downloads", action="store_true", help="Only clean downloaded packages, keep submodules")
    parser.add_argument("--only-submodules", action="store_true", help="Only clean git submodules, keep downloaded packages")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent

    print("==========================================================")
    print(" HeliosView Reset Dependencies (Python)")
    print(f" Working directory: {repo_root}")
    print(f" OS Platform:      {platform.system()} ({platform.machine()})")
    print("==========================================================")

    if not args.only_downloads:
        reset_submodules(repo_root, keep_git_cache=args.keep_git_cache)
    else:
        print("\n[1/2] Skipping submodules (--only-downloads specified).")

    if not args.only_submodules:
        reset_downloads(repo_root)
    else:
        print("\n[2/2] Skipping downloaded resources (--only-submodules specified).")

    print("\n==========================================================")
    print(" Repository reset complete!")
    print(" Submodules are uninitialized and external resources removed.")
    print(" Your development code and git branches are intact.")
    print(" To re-initialize at any time, run:")
    print("   python scripts/setup-dependencies.py")
    print("==========================================================")

if __name__ == "__main__":
    main()
