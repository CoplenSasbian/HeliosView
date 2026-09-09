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

/* ShellExecuteW/ShellExecuteExW signal failure with small positive SE_ERR_*
 * values rather than system error codes (2/3/5/8/26–32 in shellapi.h; a NULL
 * hwnd form also returns them directly). FormatMessageW would render those as
 * unrelated messages (e.g. SE_ERR_NOASSOC (31) comes out as ERROR_GEN_FAILURE
 * "A device attached to the system is not functioning"), so map the common ones
 * to their own text here. Returns nullptr when `code` is not a known SE_ERR
 * value — callers then format it as a normal system code. */
inline const char* hv_se_err_text(DWORD code)
{
    switch (code) {
    case 0:  return "out of memory";
    case 2:  return "file not found";
    case 3:  return "path not found";
    case 5:  return "access denied";
    case 8:  return "out of memory";
    case 26: return "the file cannot be opened for sharing";
    case 27: return "the file association is incomplete";
    case 28: return "DDE timed out";
    case 29: return "the DDE transaction failed";
    case 30: return "DDE is busy";
    case 31: return "no application is associated with this file type";
    case 32: return "a required DLL was not found";
    default: return nullptr;
    }
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
        const char* se = hv_se_err_text(win32_code);
        const std::string fmt = se ? std::string(se) : hv_format_message(win32_code);
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

/* ================= OS version =================
 *
 * Real Windows build number (RtlGetVersion — not affected by the per-app
 * compatibility shims that make GetVersionExW lie). Backends use it to gate
 * features that only exist on newer builds (DWMWA_SYSTEMBACKDROP_TYPE needs
 * 22621, the dark-mode attribute 20 needs 19041) and to pick the caption icon
 * font (Windows 11 = build 22000+). Returns 0 when it cannot be read. */
inline int hv_os_build()
{
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const auto rtl = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtl)
        return 0;
    RTL_OSVERSIONINFOW ovi{};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    return rtl(&ovi) == 0 ? static_cast<int>(ovi.dwBuildNumber) : 0;
}

/* ================= Hidden host window =================
 *
 * One hidden top-level window per thread, owned by the library. Subsystems that
 * need a native window of their own (tray callbacks / TaskbarCreated, the owner
 * of a window-less popup menu) use it. The host itself knows nothing about
 * them: each subsystem registers its message handler here the first time it
 * needs the host, and the host's procedure gives every message to the handlers
 * in registration order. Defined in heliosview_host_win32.cpp. */

/* A host message handler: return true when the message was consumed. */
using hv_host_message_fn = bool (*)(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/* Get (creating on first use) this thread's hidden host window, registering
 * `handler` (idempotent, nullptr = none). Returns NULL on failure. */
HWND hv_host_window(hv_host_message_fn handler = nullptr);

/* Is `hwnd` the library's hidden host window? */
bool hv_host_is(HWND hwnd);

/* ================= Menu messages =================
 *
 * The menu backend routes selections and refreshes action state inside the real
 * window procedure (not a comctl32 subclass: WM_MENUCOMMAND does not travel
 * through the subclass chain). heliosview_wndproc_t calls this for every
 * message of a library window, and it is registered as a host handler for
 * window-less popups. Returns true when the message was consumed.
 * Defined in heliosview_menu_win32.cpp. */
bool hv_menu_handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/* Tray messages (TaskbarCreated, tray callbacks); registered as a host handler
 * by the tray backend. Returns true when consumed.
 * Defined in heliosview_tray_win32.cpp. */
bool hv_tray_handle_host_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/* Application menu bar (heliosview_menu_set_app_menu) + keyboard accelerators.
 * Defined in heliosview_menu_win32.cpp. */
void hv_menu_apply_app_menu(HWND hwnd);          /* attach the current bar to a new window */
bool hv_menu_translate_accelerator(MSG* msg);    /* true = consumed as an accelerator */

/* Menu reference counting for other subsystems that display a menu (a tray's
 * attached context menu). A menu's HMENU must outlive every holder, so a holder
 * takes a reference and releases it when it stops displaying the menu.
 * Defined in heliosview_menu_win32.cpp. */
void hv_menu_retain(heliosview_menu_t* menu);
void hv_menu_release(heliosview_menu_t* menu);

/* True when `menu` is a menu BAR (heliosview_menu_create_bar). Other backends
 * (tray) use this to reject bars where only popup menus fit.
 * Defined in heliosview_menu_win32.cpp. */
bool hv_menu_is_bar(heliosview_menu_t* menu);

/* ================= Keyboard shortcuts =================
 *
 * A portable shortcut string ("Primary+S", "Ctrl+Shift+Z", "F11", "Cmd+O") is
 * parsed into a Win32 accelerator: modifier flags plus a virtual key.
 * Primary/Cmd mean Command on macOS and Control elsewhere, so portable code
 * writes one string for both. Shared by the menu backend (display) and the
 * message loop (accelerator table). */

struct hv_shortcut {
    UINT vk = 0;   /* virtual key; 0 = unset/invalid */
    BYTE mods = 0; /* MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN */
};

/* Virtual key for one key token ("s", "F11", "pageup"), 0 = unknown. */
inline UINT hv_shortcut_key(const std::string& token)
{
    if (token.size() == 1) {
        const char c = token[0];
        if (c >= 'a' && c <= 'z') return static_cast<UINT>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return static_cast<UINT>(c);
        if (c >= '0' && c <= '9') return static_cast<UINT>(c);
        switch (c) {
        case ',':  return VK_OEM_COMMA;
        case '.':  return VK_OEM_PERIOD;
        case '/':  return VK_OEM_2;
        case ';':  return VK_OEM_1;
        case '\'': return VK_OEM_7;
        case '`':  return VK_OEM_3;
        case '-':  return VK_OEM_MINUS;
        case '=':  return VK_OEM_PLUS;
        case '[':  return VK_OEM_4;
        case ']':  return VK_OEM_6;
        case '\\': return VK_OEM_5;
        default:   return 0;
        }
    }
    if (token.size() >= 2 && token[0] == 'f') {
        const std::string digits = token.substr(1);
        if (digits.find_first_not_of("0123456789") == std::string::npos) {
            const int n = std::atoi(digits.c_str());
            if (n >= 1 && n <= 24)
                return VK_F1 + static_cast<UINT>(n - 1);
        }
        return 0;
    }
    static const struct { const char* name; UINT vk; } kNamed[] = {
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"return", VK_RETURN}, {"enter", VK_RETURN},
        {"space", VK_SPACE}, {"tab", VK_TAB}, {"backspace", VK_BACK}, {"back", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE}, {"insert", VK_INSERT}, {"ins", VK_INSERT},
        {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"up", VK_UP}, {"down", VK_DOWN}, {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD},
        {"slash", VK_OEM_2}, {"semicolon", VK_OEM_1}, {"apostrophe", VK_OEM_7},
        {"grave", VK_OEM_3}, {"minus", VK_OEM_MINUS}, {"equal", VK_OEM_PLUS},
        {"backslash", VK_OEM_5}, {"bracketleft", VK_OEM_4}, {"bracketright", VK_OEM_6},
    };
    for (const auto& entry : kNamed) {
        if (token == entry.name)
            return entry.vk;
    }
    return 0;
}

/* Parse a portable shortcut string; vk == 0 means it could not be parsed. */
inline hv_shortcut hv_parse_shortcut(const std::string& text)
{
    hv_shortcut sc;
    size_t start = 0;
    while (start <= text.size()) {
        size_t pos = text.find('+', start);
        if (pos == std::string::npos)
            pos = text.size();
        std::string token = text.substr(start, pos - start);
        const size_t b = token.find_first_not_of(" \t");
        const size_t e = token.find_last_not_of(" \t");
        token = (b == std::string::npos) ? std::string() : token.substr(b, e - b + 1);
        for (char& c : token) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }

        if (!token.empty()) {
            if (token == "primary" || token == "cmd" || token == "command"
                || token == "ctrl" || token == "control")
                sc.mods |= MOD_CONTROL;
            else if (token == "shift")
                sc.mods |= MOD_SHIFT;
            else if (token == "alt" || token == "option")
                sc.mods |= MOD_ALT;
            else if (token == "meta" || token == "win" || token == "super")
                sc.mods |= MOD_WIN;
            else if (pos == text.size())
                sc.vk = hv_shortcut_key(token); /* last token = the key */
            else
                return hv_shortcut{}; /* a modifier was expected */
        }
        if (pos == text.size())
            break;
        start = pos + 1;
    }
    return sc; /* vk == 0 when no key token was present */
}

/* Human-readable form for menu display ("Ctrl+Shift+Z"). */
inline std::wstring hv_shortcut_display(const hv_shortcut& sc)
{
    if (sc.vk == 0)
        return {};
    std::wstring out;
    if (sc.mods & MOD_CONTROL) out += L"Ctrl+";
    if (sc.mods & MOD_ALT)     out += L"Alt+";
    if (sc.mods & MOD_SHIFT)   out += L"Shift+";
    if (sc.mods & MOD_WIN)     out += L"Win+";
    if ((sc.vk >= 'A' && sc.vk <= 'Z') || (sc.vk >= '0' && sc.vk <= '9')) {
        out += static_cast<wchar_t>(sc.vk);
    } else if (sc.vk >= VK_F1 && sc.vk <= VK_F24) {
        out += L"F" + std::to_wstring(sc.vk - VK_F1 + 1);
    } else {
        switch (sc.vk) {
        case VK_ESCAPE:     out += L"Esc"; break;
        case VK_RETURN:     out += L"Enter"; break;
        case VK_SPACE:      out += L"Space"; break;
        case VK_TAB:        out += L"Tab"; break;
        case VK_BACK:       out += L"Backspace"; break;
        case VK_DELETE:     out += L"Del"; break;
        case VK_INSERT:     out += L"Ins"; break;
        case VK_HOME:       out += L"Home"; break;
        case VK_END:        out += L"End"; break;
        case VK_PRIOR:      out += L"PgUp"; break;
        case VK_NEXT:       out += L"PgDn"; break;
        case VK_LEFT:       out += L"Left"; break;
        case VK_RIGHT:      out += L"Right"; break;
        case VK_UP:         out += L"Up"; break;
        case VK_DOWN:       out += L"Down"; break;
        case VK_OEM_COMMA:  out += L","; break;
        case VK_OEM_PERIOD: out += L"."; break;
        case VK_OEM_1:      out += L";"; break;
        case VK_OEM_2:      out += L"/"; break;
        case VK_OEM_3:      out += L"`"; break;
        case VK_OEM_4:      out += L"["; break;
        case VK_OEM_5:      out += L"\\"; break;
        case VK_OEM_6:      out += L"]"; break;
        case VK_OEM_7:      out += L"'"; break;
        case VK_OEM_MINUS:  out += L"-"; break;
        case VK_OEM_PLUS:   out += L"="; break;
        default:            out += L"?"; break;
        }
    }
    return out;
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

/* ================= Window handle (HWND) =================
 *
 * The backends address a window only through the public API: the native handle
 * is exposed as an opaque uintptr_t id (heliosview_window_id). hv_window_hwnd
 * recovers the HWND from it. */

inline HWND hv_window_hwnd(heliosview_window_t* window)
{
    return reinterpret_cast<HWND>(heliosview_window_id(window));
}