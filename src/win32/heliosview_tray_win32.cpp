// HeliosView.dll — Windows tray icon backend (heliosview_tray_* API).
//
// Standalone tray icon backend, completely decoupled from business windows.
//
// Architecture:
// - Shell_NotifyIcon callback messages and the system-wide TaskbarCreated
//   broadcast are delivered to the library's hidden host window
//   (hv_host_window(), shared with window-less popup menus — see
//   heliosview_host_win32.cpp). hv_tray_handle_host_message() below is the hook
//   that window's procedure calls.
// - Multiple trays share the host window, differentiated by uID (wParam).
// - Business windows are never subclassed.

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"    /* hv::hv_alloc / hv_dealloc / event queue */
#include "heliosview_win32_internal.h" /* utf8_to_wide / hv_fail_win32 */

#include <shellapi.h> /* Shell_NotifyIcon / NOTIFYICONDATAW */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <flat_map>
#include <string>
#include <vector>

/* ================= Tray icon structure ================= */

struct heliosview_tray {
    HWND hwnd = nullptr;       /* hidden host window that receives callback messages */
    UINT uid = 0;              /* tray icon id (process-unique; key in tls_tray_userdata) */
    HICON icon = nullptr;      /* current icon (NULL = default) */
    bool icon_owned = false;   /* true when `icon` came from LoadImage and must be destroyed */
    std::string tooltip;       /* UTF-8 */
    bool added = false;        /* NIM_ADD succeeded (so NIM_DELETE is safe) */
    void* userdata = nullptr;  /* caller data (C++ wrapper object pointer) */
    heliosview_menu_t* menu = nullptr; /* attached context menu (refcounted; NULL = none) */
};

namespace {

/* One process-wide callback message shared by every tray: wParam carries the tray's uID */
constexpr UINT kTrayCallbackMessage = WM_APP + 0x42;

/* TaskbarCreated broadcast message: registered when Explorer restarts */
UINT g_taskbar_created_message = 0;

UINT taskbar_created_message()
{
    if (g_taskbar_created_message == 0)
        g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    return g_taskbar_created_message;
}

/* Tray icon id allocator: process-unique */
std::atomic<UINT> g_next_tray_uid{1};

/* Thread-local tray registries (message loop thread) */
inline thread_local std::flat_map<UINT, void*> tls_tray_userdata;
inline thread_local std::flat_map<UINT, heliosview_tray_t*> tls_tray_objects;
inline thread_local std::vector<heliosview_tray_t*> tls_trays;

int tray_apply(heliosview_tray_t* tray, DWORD msg);

} // namespace

/* Hook called by the hidden host window's procedure (heliosview_host_win32.cpp).
 * Returns true when the message belonged to the tray subsystem. */
bool hv_tray_handle_host_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)hwnd;
    if (message == taskbar_created_message()) {
        /* Explorer restarted: re-add all live trays on this thread */
        for (heliosview_tray_t* t : tls_trays)
            tray_apply(t, NIM_ADD);
        return true;
    }

    if (message == kTrayCallbackMessage) {
        const auto it = tls_tray_userdata.find(static_cast<UINT>(wparam));
        if (it != tls_tray_userdata.end()) {
            heliosview_tray_t* tray = tls_tray_objects.contains(static_cast<UINT>(wparam))
                                          ? tls_tray_objects[static_cast<UINT>(wparam)]
                                          : nullptr;
            /* An attached context menu replaces the right-click event: the
             * library opens the menu (the shell does that on macOS/Linux, where
             * the event may never arrive — see heliosview.h). */
            if (tray && tray->menu && (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU)) {
                heliosview_menu_show(tray->menu, nullptr); /* hidden host window is the owner */
                return true;
            }
            heliosview_event_t ev{};
            ev.window_id = 0; /* decoupled from business windows */
            ev.timestamp_ms = hv::now_ms();
            switch (lparam) {
            case WM_LBUTTONUP:       ev.type = HELIOSVIEW_EVENT_TRAY_LEFT_CLICK; break;
            case WM_LBUTTONDBLCLK:   ev.type = HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK; break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:     ev.type = HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK; break;
            case WM_MBUTTONUP:       ev.type = HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK; break;
            default:
                return true; /* consume other mouse events */
            }
            ev.userdata = it->second;
            hv::queue_push(ev);
            return true;
        }
    }
    return false;
}

namespace {

/* Load an icon from a file path (UTF-8), or the shared default application icon.
 * `owned` reports whether the returned handle is ours to destroy — the shared
 * IDI_APPLICATION handle must never be passed to DestroyIcon. */
HICON load_tray_icon(const char* path, bool* owned)
{
    if (owned)
        *owned = false;
    if (path && *path) {
        const std::wstring wpath = utf8_to_wide(path);
        HICON loaded = static_cast<HICON>(LoadImageW(nullptr, wpath.c_str(), IMAGE_ICON, 0, 0,
                                                    LR_LOADFROMFILE));
        if (loaded && owned)
            *owned = true;
        return loaded;
    }
    return LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
}

/* Release a tray's current icon unless it is the shared default. */
void release_tray_icon(heliosview_tray_t* tray)
{
    if (tray->icon && tray->icon_owned)
        DestroyIcon(tray->icon);
    tray->icon = nullptr;
    tray->icon_owned = false;
}

/* Build NOTIFYICONDATAW for Shell_NotifyIcon */
NOTIFYICONDATAW tray_nid(const heliosview_tray_t* tray)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = tray->hwnd;
    nid.uID = tray->uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMessage;
    nid.hIcon = tray->icon;
    if (!tray->tooltip.empty()) {
        const std::wstring wtip = utf8_to_wide(tray->tooltip);
        wcsncpy_s(nid.szTip, wtip.c_str(), _TRUNCATE);
    }
    return nid;
}

int tray_apply(heliosview_tray_t* tray, DWORD msg)
{
    if (!tray || !tray->hwnd)
        return -1;
    NOTIFYICONDATAW nid = tray_nid(tray);
    return Shell_NotifyIconW(msg, &nid) ? 0 : -1;
}

} // namespace

/* ================= Public Tray C API ================= */

heliosview_tray_t* heliosview_tray_create(const char* tooltip, const char* icon_path, void* userdata)
{
    HWND host = hv_host_window(&hv_tray_handle_host_message);
    if (!host) {
        hv_fail(-1, "failed to create tray host window");
        return nullptr;
    }

    auto* tray = hv::hv_alloc<heliosview_tray>();
    tray->hwnd = host;
    tray->uid = g_next_tray_uid.fetch_add(1);
    tray->tooltip = tooltip ? tooltip : "";
    tray->icon = load_tray_icon(icon_path, &tray->icon_owned);
    tray->userdata = userdata;

    tls_tray_userdata[tray->uid] = userdata;
    tls_tray_objects[tray->uid] = tray;
    tls_trays.push_back(tray);

    if (tray_apply(tray, NIM_ADD) != 0) {
        hv_fail_win32(GetLastError(), "Shell_NotifyIcon (NIM_ADD) failed — tray not created");
        tls_tray_userdata.erase(tray->uid);
        tls_tray_objects.erase(tray->uid);
        tls_trays.erase(std::remove(tls_trays.begin(), tls_trays.end(), tray), tls_trays.end());
        release_tray_icon(tray);
        hv::hv_dealloc(tray);
        return nullptr;
    }
    tray->added = true;
    return tray;
}

int heliosview_tray_set_tooltip(heliosview_tray_t* tray, const char* tooltip)
{
    if (!tray)
        return hv_fail(-1, "tray is NULL");
    tray->tooltip = tooltip ? tooltip : "";
    return tray->added ? tray_apply(tray, NIM_MODIFY) : 0;
}

int heliosview_tray_set_icon(heliosview_tray_t* tray, const char* icon_path)
{
    return heliosview_tray_set_icon_ex(tray, icon_path, HELIOSVIEW_ICON_FLAG_NONE);
}

int heliosview_tray_set_icon_ex(heliosview_tray_t* tray, const char* icon_path, uint32_t flags)
{
    (void)flags; /* HELIOSVIEW_ICON_FLAG_TEMPLATE is a macOS concept */
    if (!tray)
        return hv_fail(-1, "tray is NULL");
    bool owned = false;
    HICON new_icon = load_tray_icon(icon_path, &owned);
    if ((icon_path && *icon_path) && !new_icon)
        return hv_fail_win32(GetLastError(), "LoadImageW failed — tray icon unchanged");
    release_tray_icon(tray);
    tray->icon = new_icon;
    tray->icon_owned = owned;
    return tray->added ? tray_apply(tray, NIM_MODIFY) : 0;
}

int heliosview_tray_set_menu(heliosview_tray_t* tray, heliosview_menu_t* menu)
{
    if (!tray)
        return hv_fail(-1, "tray is NULL");
    if (tray->menu == menu)
        return 0;
    heliosview_menu_t* previous = tray->menu;
    tray->menu = menu;
    hv_menu_retain(menu); /* the tray displays it until detached or destroyed */
    hv_menu_release(previous);
    return 0;
}

void heliosview_tray_destroy(heliosview_tray_t* tray)
{
    if (!tray)
        return;
    if (tray->added)
        tray_apply(tray, NIM_DELETE);

    tls_tray_userdata.erase(tray->uid);
    tls_tray_objects.erase(tray->uid);
    tls_trays.erase(std::remove(tls_trays.begin(), tls_trays.end(), tray), tls_trays.end());

    release_tray_icon(tray);
    hv_menu_release(tray->menu);

    hv::hv_dealloc(tray);
}

/* ================= Tray balloon notification (NIF_INFO) ================= */

int heliosview_tray_notify(heliosview_tray_t* tray, const char* title, const char* message,
                           heliosview_tray_notify_icon_t icon_type, uint32_t timeout_ms)
{
    if (!tray || !tray->added)
        return hv_fail(-1, "tray is NULL or was never added (NIM_ADD)");
    NOTIFYICONDATAW nid = tray_nid(tray);
    nid.uFlags |= NIF_INFO;
    nid.uTimeout = timeout_ms ? timeout_ms : 5000;
    nid.dwInfoFlags = (icon_type == HELIOSVIEW_TRAY_NOTIFY_INFO) ? NIIF_INFO
                      : (icon_type == HELIOSVIEW_TRAY_NOTIFY_WARNING) ? NIIF_WARNING
                      : (icon_type == HELIOSVIEW_TRAY_NOTIFY_ERROR) ? NIIF_ERROR
                      : NIIF_NONE;
    const std::wstring wt = utf8_to_wide(title ? title : "");
    const std::wstring wm = utf8_to_wide(message ? message : "");
    wcsncpy_s(nid.szInfoTitle, wt.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, wm.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_MODIFY, &nid) ? 0 : hv_fail_win32(GetLastError(), "Shell_NotifyIcon (NIM_MODIFY balloon) failed");
}
