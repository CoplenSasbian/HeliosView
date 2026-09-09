# HeliosView - regenerate src/portable/heliosview_portable_stub.cpp from the public header.
#
# The portable stub backend implements the whole public C API for platforms that
# have no real backend yet. Writing 100+ signatures by hand invites drift, so the
# stub is generated from include/HeliosView/heliosview.h: one definition per
# HELIOSVIEW_API declaration, with the parameter names dropped (C++ allows that)
# and a body chosen from the return type. A few functions need real semantics
# instead of "unsupported" (see $overrides).
#
# Usage:  pwsh -File scripts/gen_portable_stub.ps1
#
# Run it after adding/removing/changing a public API function. If you forget,
# the build tells you: the linker reports the missing or duplicate definition.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$headerPath = Join-Path $root 'include/HeliosView/heliosview.h'
$outPath = Join-Path $root 'src/portable/heliosview_portable_stub.cpp'

# Implemented by the platform-independent core (src/heliosview.cpp), so the
# backend must NOT define them.
$coreSymbols = @(
    'heliosview_version', 'heliosview_backend_name', 'heliosview_last_error',
    'heliosview_last_error_string', 'heliosview_set_allocator', 'heliosview_free',
    'heliosview_utf8_to_wide', 'heliosview_wide_to_utf8', 'heliosview_poll',
    'heliosview_wait', 'heliosview_post_event', 'heliosview_quit',
    'heliosview_wake_loop', 'heliosview_add_native_filter',
    'heliosview_remove_native_filter', 'heliosview_add_native_handler',
    'heliosview_remove_native_handler', 'heliosview_window_from_id',
    'heliosview_window_count', 'heliosview_action_from_id',
    'heliosview_app_init', 'heliosview_app_id', 'heliosview_set_activation_policy',
    'heliosview_activation_policy', 'heliosview_delay', 'heliosview_interval',
    'heliosview_timer_cancel'
)

# Functions whose stub body is not just "unsupported": the out-parameters must be
# nulled so a caller that ignores the return code cannot free garbage, and the
# engine-version query reports "" (the documented "no engine" value).
$overrides = @{
    'heliosview_webview_engine_version' = @'
int heliosview_webview_engine_version(char* buf, size_t size)
{
    /* No web engine on this platform: report "" (the documented "unavailable"
     * value) instead of an error, so callers branch on emptiness. */
    if (!buf || size == 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "buf is NULL or size is 0");
    buf[0] = '\0';
    return 0;
}
'@
    'heliosview_select_folder' = @'
int heliosview_select_folder(heliosview_window_t*, const char*, char** out_path)
{
    if (out_path)
        *out_path = nullptr;
    return HV_STUB_UNSUPPORTED;
}
'@
    'heliosview_open_files' = @'
int heliosview_open_files(heliosview_window_t*, const char*, const heliosview_file_filter_t*,
                          size_t, int, char*** out_paths)
{
    if (out_paths)
        *out_paths = nullptr;
    return HV_STUB_UNSUPPORTED;
}
'@
    'heliosview_save_file' = @'
int heliosview_save_file(heliosview_window_t*, const char*, const heliosview_file_filter_t*,
                         size_t, const char*, char** out_path)
{
    if (out_path)
        *out_path = nullptr;
    return HV_STUB_UNSUPPORTED;
}
'@
    'heliosview_clipboard_get_text' = @'
int heliosview_clipboard_get_text(char** out)
{
    if (out)
        *out = nullptr;
    return HV_STUB_UNSUPPORTED;
}
'@
    'heliosview_run_program_wait' = @'
int heliosview_run_program_wait(const char*, const char*, heliosview_program_show_t, int* out_exit_code)
{
    if (out_exit_code)
        *out_exit_code = 0;
    return HV_STUB_UNSUPPORTED;
}
'@
    'heliosview_system_path' = @'
int heliosview_system_path(heliosview_system_path_kind_t, char** out)
{
    if (out)
        *out = nullptr;
    return HV_STUB_UNSUPPORTED;
}
'@
    'heliosview_os_version' = @'
int heliosview_os_version(char* buf, size_t size)
{
    if (!buf || size == 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "buf is NULL or size is 0");
    buf[0] = '\0';
    return 0;
}
'@
    'heliosview_window_scale_factor' = @'
float heliosview_window_scale_factor(const heliosview_window_t*)
{
    /* No window backend on this platform: report the documented "not created"
     * value instead of an error code (a getter, not an operation). */
    return 1.0f;
}
'@
}

$header = Get-Content $headerPath -Raw
$withoutComments = [regex]::Replace($header, '/\*.*?\*/', '', 'Singleline')
$declarations = [regex]::Matches($withoutComments, 'HELIOSVIEW_API\s+([^;]+);', 'Singleline') |
    ForEach-Object { ($_.Groups[1].Value -replace '\s+', ' ').Trim() }

$bodies = New-Object System.Collections.Generic.List[string]
$generated = 0
foreach ($declaration in $declarations) {
    $m = [regex]::Match($declaration, '^(.+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$')
    if (-not $m.Success) { throw "cannot parse declaration: $declaration" }
    $returnType = $m.Groups[1].Value.Trim()
    $name = $m.Groups[2].Value
    $params = $m.Groups[3].Value.Trim()
    if ($coreSymbols -contains $name) { continue }

    if ($overrides.ContainsKey($name)) {
        $bodies.Add($overrides[$name])
        $generated++
        continue
    }

    # Drop the parameter names; the types are what a definition needs.
    $namelessParams = if ($params -eq 'void' -or $params -eq '') { '' } else {
        ($params -split ',' | ForEach-Object {
            ($_ -replace '([A-Za-z0-9_\*>]+)\s+[A-Za-z_][A-Za-z0-9_]*$', '$1').Trim()
        }) -join ', '
    }

    # PowerShell switch runs EVERY matching clause unless it hits break, and
    # 'void*' matches both the '^void\*$' and the '\*$' pattern — so break out
    # of the first match explicitly.
    $body = switch -Regex ($returnType) {
        '^void$'                                     { '    /* no state to release */'; break }
        '^void\*$'                                   { '    return nullptr;'; break }
        '\*$'                                        { "    HV_STUB_UNSUPPORTED;`n    return nullptr;"; break }
        '^uint32_t$|^uintptr_t$|^int32_t$|^size_t$'  { '    return 0;'; break }
        '^heliosview_[a-z0-9_]+_t$'                  { "    return static_cast<$returnType>(0);"; break }
        default                                      { '    return HV_STUB_UNSUPPORTED;' }
    }
    $bodies.Add("$returnType $name($namelessParams)`n{`n$body`n}")
    $generated++
}

$fileHeader = @'
// HeliosView - portable stub backend.
//
// Built on platforms that have no real backend yet (see the else() branch in
// src/CMakeLists.txt). Every feature reports HELIOSVIEW_ERROR_UNSUPPORTED (-4)
// and the WebView engine query reports "no engine", so the library still links
// and the public C API is exercisable everywhere - a conformance test suite can
// run on any platform and assert the "unsupported" contract.
//
// GENERATED FILE - do not edit by hand. The definitions below mirror
// include/HeliosView/heliosview.h one-for-one (signatures copied, parameter
// names dropped). Regenerate with:
//
//     pwsh -File scripts/gen_portable_stub.ps1
//
// The linker is the safety net: a new public API function that is missing here
// shows up as an unresolved symbol when this backend is built.
#include "../heliosview_backend.h"

#define HV_STUB_UNSUPPORTED \
    hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "no HeliosView backend for this platform yet (see src/heliosview_backend.h)")

/* ================= Backend entry points ================= */

const char* hv_backend_name()
{
    return "portable";
}

bool hv_backend_window_alive(uintptr_t)
{
    return false; /* the stub never creates native windows */
}

'@

Set-Content -Path $outPath -Value ($fileHeader + ($bodies -join "`n")) -Encoding utf8
Write-Host "generated $generated definitions -> $outPath"
