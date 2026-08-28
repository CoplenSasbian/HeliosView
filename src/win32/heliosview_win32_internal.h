#pragma once

/**
 * Internal win32 shared header: helpers shared between the window
 * implementation (heliosview_window_win32.cpp) and the other win32 backends
 * (WebView2, tray, menu). Not part of the public API.
 *
 * The split exists so the window layer never has to know about WebView2, and
 * the other backends never touch `heliosview_window` internals: they reach the
 * window only through the public API (heliosview_window_id / _is_resizable /
 * _is_fullscreen / _from_id) or hv_window_hwnd for the raw handle. This header
 * carries the shared codecs, the title-bar strip metric, the routing-id
 * unregister helper and the HWND recovery helper. */
#include <HeliosView/heliosview.h>

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00 /* Windows 10: GetDpiForWindow / ActivateActivatedContext */
#include <windows.h>
#include <commctrl.h> /* SetWindowSubclass / DefSubclassProc / RemoveWindowSubclass */

#include <cstdio>
#include <cstdlib>
#include <string>

/* ================= Shared UTF-8 <-> UTF-16 codecs ================= */

/* UTF-8 → UTF-16 */
inline std::wstring utf8_to_wide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

/* UTF-16 → UTF-8 */
inline std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/* ================= Formatted platform errors (win32 layer) =================
 *
 * Failures that carry a real platform code record a message that APPENDS the
 * system's own description, so the caller's heliosview_last_error_string reads
 * e.g. "CreateWindowExW failed: The system cannot find the file specified."
 * instead of a bare context string. The storage itself stays the
 * platform-independent hv_fail (see heliosview_internal.h); these two are the
 * win32 convenience wrappers around it. */

/* Format a Win32 / HRESULT code through the system message tables; "" when the
 * system does not know the code. */
inline std::string hv_format_message(DWORD code)
{
    wchar_t* raw = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                       FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0,
                                   reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    if (!n || !raw)
        return {};
    std::wstring w(raw, n);
    LocalFree(raw);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' '))
        w.pop_back();
    return wide_to_utf8(w);
}

/* Record a Win32 GetLastError-style failure: error code = -win32_code (or -1
 * when 0, i.e. the call did not set a usable error), message =
 * "<context>: <system message>" (with a numeric fallback when the system does
 * not know the code). Returns the code for `return hv_fail_win32(...);`. */
inline int hv_fail_win32(DWORD win32_code, const char* context)
{
    g_hv_last_error_code = win32_code ? -static_cast<int>(win32_code) : -1;
    std::string msg = context ? context : "";
    if (win32_code) {
        const std::string fmt = hv_format_message(win32_code);
        if (!fmt.empty())
            msg += ": " + fmt;
        else {
            char num[32];
            std::snprintf(num, sizeof(num), " (Win32 error %lu)",
                          static_cast<unsigned long>(win32_code));
            msg += num;
        }
    }
    g_hv_last_error_message = msg;
    return g_hv_last_error_code;
}

/* Record an HRESULT failure: error code = -hr (the library's negated-HRESULT
 * form; -1 when hr is not actually a failure), message =
 * "<context>: <system message>" (with a 0x-hex fallback when the system does
 * not know the code). Returns the code for `return hv_fail_hresult(...);`. */
inline int hv_fail_hresult(HRESULT hr, const char* context)
{
    g_hv_last_error_code = FAILED(hr) ? -static_cast<int>(hr) : -1;
    std::string msg = context ? context : "";
    if (FAILED(hr)) {
        const std::string fmt = hv_format_message(static_cast<DWORD>(hr));
        if (!fmt.empty())
            msg += ": " + fmt;
        else {
            char hex[32];
            std::snprintf(hex, sizeof(hex), " (HRESULT 0x%08lX)", static_cast<unsigned long>(hr));
            msg += hex;
        }
    }
    g_hv_last_error_message = msg;
    return g_hv_last_error_code;
}

/* ================= Title-bar strip metric =================
 *
 * Frameless windows draw all their chrome in the page (optionally with the
 * injected <helios-window-controls> web component for the buttons). This is the
 * 96-DPI height of the drag strip, scaled with the DPI. Shared because the
 * window hit-tests it (WM_NCHITTEST) and the WebView2 backend reports it to the
 * page (__hv.state). */

/* Height of the title-bar strip the drag area occupies, in 96-DPI pixels. */
inline constexpr int kTitleBarHeight = 48;

inline int hv_title_bar_height(HWND hwnd)
{
    return MulDiv(kTitleBarHeight, GetDpiForWindow(hwnd), 96);
}

/* ================= Routing id allocation (caller-registered ids) =================
 *
 * The window keeps a per-window routing id space (WM_APP range) that
 * heliosview_window_add_item draws from for caller-registered ids. The tray
 * backend uses its own process-wide callback message + a TLS uID table, and
 * menu item ids are menu-local (MNS_NOTIFYBYPOS / WM_MENUCOMMAND routing) —
 * neither touches this space. */

/* Allocate the window's next routing id without registering anything; 0 when
 * the window or its native handle is missing. Defined in
 * heliosview_window_win32.cpp. */
uint32_t hv_window_next_routing_id(heliosview_window_t* window);

/* ================= Window handle (HWND) =================
 *
 * The backends address a window only through the public API: the native handle
 * is exposed as an opaque uintptr_t id (heliosview_window_id). hv_window_hwnd
 * recovers the HWND from it. */

inline HWND hv_window_hwnd(heliosview_window_t* window)
{
    return reinterpret_cast<HWND>(heliosview_window_id(window));
}