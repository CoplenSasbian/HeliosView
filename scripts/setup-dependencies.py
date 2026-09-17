#!/usr/bin/env python3
"""
setup-dependencies.py

HeliosView Dependency Setup Script (Python implementation, zero external pip dependencies).
Manages git submodules and external prebuilt binary packages outside CMake.

Environment Considerations:
- Detects OS platform (Windows vs Linux/macOS). Windows-specific packages
  (WebView2 SDK, OpenSSL vcpkg NuGet) are only fetched on Windows.
- Supports system proxies, environment proxies (HTTP_PROXY/HTTPS_PROXY), and manual --proxy.
- Tolerant SSL context handling for environments with broken or missing root certs on Windows.
"""

import argparse
import os
import platform
import shutil
import ssl
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

BOOST_LIBS = [
    'asio', 'beast', 'json', 'container', 'endian', 'variant2', 'compat', 'system',
    'config', 'core', 'assert', 'throw_exception', 'static_assert', 'type_traits',
    'utility', 'detail', 'winapi', 'move', 'align', 'mp11', 'predef', 'optional',
    'smart_ptr', 'bind', 'intrusive', 'logic', 'static_string', 'container_hash',
    'describe', 'io'
]

DIRECT_SUBMODULES = [
    'third_party/stdexec',
    'third_party/boost',
    'third_party/blend2d',
    'third_party/asmjit'
]

def get_ssl_contexts():
    """Returns a list of SSL contexts to try: default verified, followed by unverified fallback."""
    contexts = []
    try:
        ctx = ssl.create_default_context()
        contexts.append(ctx)
    except Exception:
        pass

    try:
        unverified = ssl._create_unverified_context()
        contexts.append(unverified)
    except Exception:
        pass

    return contexts or [None]

def create_opener(proxy_url=None):
    """Creates a urllib opener respecting environment proxies and optional manual proxy."""
    handlers = []
    if proxy_url:
        handlers.append(urllib.request.ProxyHandler({
            'http': proxy_url,
            'https': proxy_url
        }))
    else:
        # Detect system and environment proxy automatically
        handlers.append(urllib.request.ProxyHandler())
    return urllib.request.build_opener(*handlers)

def download_file(urls, dest_path, desc, force=False, opener=None):
    dest = Path(dest_path)
    if dest.exists() and not force:
        print(f"  [OK] {desc} already exists: {dest}")
        return True

    dest.parent.mkdir(parents=True, exist_ok=True)
    temp_path = dest.with_suffix(dest.suffix + ".tmp")
    if temp_path.exists():
        temp_path.unlink()

    if opener is None:
        opener = create_opener()

    ssl_contexts = get_ssl_contexts()

    for url in urls:
        print(f"  Downloading {desc} from {url}...")
        for ctx in ssl_contexts:
            try:
                req = urllib.request.Request(
                    url,
                    headers={'User-Agent': 'HeliosView-Setup/1.0 (Python; ' + platform.system() + ')'}
                )
                with opener.open(req, context=ctx, timeout=60) as resp, open(temp_path, 'wb') as f:
                    shutil.copyfileobj(resp, f)

                if temp_path.exists() and temp_path.stat().st_size > 0:
                    temp_path.replace(dest)
                    print(f"  [SUCCESS] Saved to {dest}")
                    return True
            except Exception as e:
                # If verified SSL fails, retry with unverified context next
                if temp_path.exists():
                    temp_path.unlink()
                err_msg = str(e)
                if "CERTIFICATE_VERIFY_FAILED" in err_msg and ctx is ssl_contexts[0]:
                    print("  Notice: SSL certificate verification failed; retrying with system fallback...")
                    continue
                print(f"  Warning: Download attempt failed from {url}: {e}")
                break

    print(f"  Error: Failed to download {desc} from all candidate URLs.")
    return False

def extract_archive(archive_path, extract_dir):
    print(f"  Extracting {Path(archive_path).name} -> {extract_dir}...")
    dest = Path(extract_dir)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(archive_path, 'r') as z:
        z.extractall(dest)
    print(f"  [SUCCESS] Extracted to {dest}")

def setup_submodules(repo_root, force=False, jobs=4):
    print("\n[1/4] Checking and Initializing Git Submodules...")
    # Check if git is available
    git_bin = shutil.which("git")
    if not git_bin:
        print("  Error: 'git' executable not found in PATH. Submodules cannot be managed.")
        sys.exit(1)

    # Check direct submodules
    missing_direct = []
    for sm in DIRECT_SUBMODULES:
        marker = repo_root / sm / ".git"
        if force or not marker.exists():
            missing_direct.append(sm)

    if missing_direct:
        print(f"  Updating direct submodules ({len(missing_direct)} to update)...")
        cmd = [git_bin, 'submodule', 'update', '--init', '--depth', '1', '--progress', f'--jobs={jobs}', '--'] + missing_direct
        subprocess.check_call(cmd, cwd=repo_root)
    else:
        print("  Direct submodules are already initialized.")

    # Check Boost libraries
    boost_root = repo_root / 'third_party' / 'boost'
    missing_boost = []
    for lib in BOOST_LIBS:
        marker = boost_root / 'libs' / lib / ".git"
        if force or not marker.exists():
            missing_boost.append(f"libs/{lib}")

    if missing_boost:
        print(f"  Updating Boost libraries ({len(missing_boost)} libraries to update)...")
        cmd = [git_bin, 'submodule', 'update', '--init', '--depth', '1', '--progress', f'--jobs={jobs}', '--'] + missing_boost
        subprocess.check_call(cmd, cwd=boost_root)
    else:
        print("  All required Boost libraries are already initialized.")

def setup_openssl(repo_root, force=False, opener=None):
    print("\n[2/4] Setting up OpenSSL (vcpkg NuGet package)...")
    version = "3.5.2"
    dest_dir = repo_root / 'third_party' / 'openssl'
    header = dest_dir / 'build' / 'native' / 'include' / 'openssl' / 'ssl.h'
    nupkg = repo_root / 'third_party' / f'openssl.vcpkg.{version}.nupkg'

    if header.exists() and not force:
        print(f"  OpenSSL already set up at {dest_dir}")
        return

    urls = [
        f"https://api.nuget.org/v3-flatcontainer/openssl.vcpkg/{version}/openssl.vcpkg.{version}.nupkg",
        f"https://www.nuget.org/api/v2/package/openssl.vcpkg/{version}"
    ]
    if download_file(urls, nupkg, f"OpenSSL {version} NuGet", force, opener=opener):
        extract_archive(nupkg, dest_dir)
    else:
        sys.exit(1)

def setup_cacert(repo_root, force=False, opener=None):
    print("\n[3/4] Setting up CA Certificate Bundle (cacert.pem)...")
    cacert_path = repo_root / 'third_party' / 'cacert.pem'
    if cacert_path.exists() and not force:
        print(f"  cacert.pem already exists at {cacert_path}")
        return

    urls = [
        "https://curl.se/ca/cacert.pem",
        "https://ghproxy.net/https://raw.githubusercontent.com/bagder/ca-bundle/master/ca-bundle.crt",
        "https://raw.githubusercontent.com/bagder/ca-bundle/master/ca-bundle.crt"
    ]
    if not download_file(urls, cacert_path, "CA Certificate Bundle", force, opener=opener):
        sys.exit(1)

def setup_webview2(repo_root, force=False, opener=None):
    print("\n[4/4] Setting up Microsoft WebView2 SDK...")
    version = "1.0.4181-prerelease"
    dest_dir = repo_root / 'third_party' / 'webview2-sdk'
    header = dest_dir / 'build' / 'native' / 'include' / 'WebView2.h'
    nupkg = repo_root / 'third_party' / f'webview2.{version}.nupkg'

    if header.exists() and not force:
        print(f"  WebView2 SDK already set up at {dest_dir}")
        return

    urls = [
        f"https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/{version}/microsoft.web.webview2.{version}.nupkg",
        f"https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/{version}"
    ]
    if download_file(urls, nupkg, f"WebView2 SDK {version} NuGet", force, opener=opener):
        extract_archive(nupkg, dest_dir)
    else:
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="HeliosView Dependency Setup")
    parser.add_argument("--force", action="store_true", help="Force re-download and re-extraction")
    parser.add_argument("--skip-submodules", action="store_true", help="Skip git submodule updates")
    parser.add_argument("--skip-downloads", action="store_true", help="Skip binary downloads")
    parser.add_argument("--proxy", type=str, default=None, help="HTTP/HTTPS proxy URL (e.g. http://127.0.0.1:7890)")
    parser.add_argument("--jobs", type=int, default=4, help="Number of parallel git jobs")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    is_windows = sys.platform == 'win32'

    print("==========================================================")
    print(" HeliosView Dependency Setup (Python)")
    print(f" Working directory: {repo_root}")
    print(f" OS Platform:      {platform.system()} ({platform.machine()})")
    print(f" Python Version:   {platform.python_version()}")
    if args.proxy:
        print(f" Custom Proxy:     {args.proxy}")
    print("==========================================================")

    # 1. Submodules (cross-platform)
    if not args.skip_submodules:
        setup_submodules(repo_root, force=args.force, jobs=args.jobs)
    else:
        print("\n[1/4] Skipping submodules (--skip-submodules specified).")

    # 2. Binary Downloads (platform check)
    if not args.skip_downloads:
        opener = create_opener(args.proxy)
        if is_windows:
            setup_openssl(repo_root, force=args.force, opener=opener)
            setup_cacert(repo_root, force=args.force, opener=opener)
            setup_webview2(repo_root, force=args.force, opener=opener)
        else:
            print("\n[2..4] Non-Windows OS detected.")
            print("  Note: OpenSSL NuGet and WebView2 SDK are Windows-specific packages.")
            print("  On Linux/macOS, please ensure system OpenSSL and webview libraries are installed.")
    else:
        print("\n[2..4] Skipping external downloads (--skip-downloads specified).")

    print("\n==========================================================")
    print(" Dependencies setup complete! You can now configure CMake.")
    print(" Example: cmake -B build -S .")
    print("==========================================================")

if __name__ == "__main__":
    main()
