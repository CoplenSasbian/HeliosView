// HeliosView.dll — Windows tray icon backend (heliosview_tray_* API).
//
// A tray icon is attached to a window and shown in the OS notification area via
// Shell_NotifyIcon. When the icon is clicked, the shell posts ONE shared
// callback message (kTrayCallbackMessage, the same for every tray) to the
// icon's window with:
//
//     wParam = the tray icon's id (uID)     ← the per-tray identity
//     lParam = the mouse event (WM_LBUTTONUP, ...)
//
// Routing: ONE comctl32 subclass callout per window (fixed key, installed
// lazily by the first tray on it; the (proc, key) pair is unique per window,
// and the key value is arbitrary — it coexists with the WebView's and menu's
// own (proc, 0) entries because those are different procs). The callout
// resolves the tray from wParam (uID) through a THREAD-LOCAL table
// (tls_tray_userdata): uIDs come from a process-global counter and are never
// reused, so a uID maps to at most one live tray — destroyed trays erase their
// entry (fail-closed). No per-tray registration, no routing-id allocation, no
// registries beyond the one small TLS table.
//
// Threading: trays are created, used and clicked on the message-loop thread;
// the table is thread-local to match (the event queue is thread-local too).

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"    /* hv::hv_alloc / hv_dealloc / event queue */
#include "heliosview_win32_internal.h" /* utf8_to_wide / hv_window_hwnd */

#include <shellapi.h> /* Shell_NotifyIcon / NOTIFYICONDATAW */

#include <atomic>
#include <cstring>
#include <flat_map>
#include <string>

/* ================= Tray icon (Shell_NotifyIcon) ================= */

struct heliosview_tray {
    HWND hwnd = nullptr;       /* owning window (receives the callback messages) */
    UINT uid = 0;              /* tray icon id (process-unique; the routing key in tls_tray_userdata) */
    HICON icon = nullptr;      /* current icon (owned; NULL = default) */
    std::string tooltip;       /* UTF-8 */
    bool added = false;        /* NIM_ADD succeeded (so NIM_DELETE is safe) */
    void* userdata = nullptr;  /* caller data (the C++ wrapper stores an object pointer) */
};

namespace {

/* One process-wide callback message shared by EVERY tray: the shell routes the
 * event to the right window, and wParam carries the tray's uID, so per-tray
 * message numbers are unnecessary. WM_APP + 0x42 avoids WM_APP + 0x41 (the
 * WebView's deferred sync message) and the WM_APP + 0x100+ caller-routing ids. */
constexpr UINT kTrayCallbackMessage = WM_APP + 0x42;

/* Subclass key of the tray callout: unique among hv_tray_subclass_proc's own
 * entries on a window (the tray installs at most one per window), any value
 * works — 0 coexists with the WebView's and menu's (proc, 0) entries because
 * those are different procs. */
constexpr UINT_PTR kTraySubclassKey = 0;

/* Tray icon id allocator: per-process, never reused while a tray lives, so the
 * uID → userdata table below has no collisions and no stale entries. */
std::atomic<UINT> g_next_tray_uid{1};

/* uID → userdata (thread-local: trays live on the message-loop thread). A
 * destroyed tray erases its entry, so a late callback for it fails the lookup
 * (fail-closed). */
inline thread_local std::flat_map<UINT, void*> tls_tray_userdata;

/* The tray routing callout: installed ONCE per window (fixed key), shared by
 * all trays on it. Shell_NotifyIcon posts kTrayCallbackMessage with the tray's
 * uID in wParam; resolve the userdata from the TLS table, translate the mouse
 * event in lParam into a TRAY_* event. Every other message is forwarded. */
LRESULT CALLBACK hv_tray_subclass_proc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == kTrayCallbackMessage) {
        const auto it = tls_tray_userdata.find(static_cast<UINT>(wparam)); /* wParam = the tray's uID */
        if (it != tls_tray_userdata.end()) {
            heliosview_event_t ev{};
            ev.window_id = reinterpret_cast<uintptr_t>(hwnd);
            ev.timestamp_ms = hv::now_ms();
            switch (lparam) {
            case WM_LBUTTONUP:       ev.type = HELIOSVIEW_EVENT_TRAY_LEFT_CLICK; break;
            case WM_LBUTTONDBLCLK:   ev.type = HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK; break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:     ev.type = HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK; break;
            case WM_MBUTTONUP:       ev.type = HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK; break;
            default:
                return 1; /* consume; not a click we translate */
            }
            ev.userdata = it->second;
            hv::queue_push(ev);
            return 1; /* consumed: this tray's callback */
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

/* Install the tray callout on `hwnd`. Idempotent: SetWindowSubclass upserts
 * the same (proc, key) pair — one entry per window, shared by all its trays. */
void tray_ensure_window_subclass(HWND hwnd)
{
    if (hwnd)
        SetWindowSubclass(hwnd, hv_tray_subclass_proc, kTraySubclassKey, 0);
}

/* Load an icon from a file path (UTF-8), or the default application icon when NULL/empty */
HICON load_tray_icon(const char* path)
{
    if (path && *path) {
        const std::wstring wpath = utf8_to_wide(path);
        return static_cast<HICON>(LoadImageW(nullptr, wpath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
    }
    return LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
}

/* Build the NOTIFYICONDATA for this tray (fresh each call; uFlags always set) */
NOTIFYICONDATAW tray_nid(const heliosview_tray_t* tray)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = tray->hwnd;
    nid.uID = tray->uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMessage; /* shared by all trays; wParam carries the uID */
    nid.hIcon = tray->icon;
    if (!tray->tooltip.empty()) {
        const std::wstring wtip = utf8_to_wide(tray->tooltip);
        wcsncpy_s(nid.szTip, wtip.c_str(), _TRUNCATE);
    }
    return nid;
}

/* Apply a Shell_NotifyIcon operation; returns 0 on success, negative on failure */
int tray_apply(heliosview_tray_t* tray, DWORD msg)
{
    if (!tray || !tray->hwnd)
        return -1;
    NOTIFYICONDATAW nid = tray_nid(tray);
    return Shell_NotifyIconW(msg, &nid) ? 0 : -1;
}

} // namespace

heliosview_tray_t* heliosview_tray_create(heliosview_window_t* window, const char* tooltip,
                                          const char* icon_path, void* userdata)
{
    if (!window || !hv_window_hwnd(window)) { /* the window must exist to receive callback messages */
        hv_fail(-1, "window must exist (native window created) before creating a tray");
        return nullptr;
    }

    auto* tray = hv::hv_alloc<heliosview_tray>();
    tray->hwnd = hv_window_hwnd(window);
    tray->uid = g_next_tray_uid.fetch_add(1);
    tray->tooltip = tooltip ? tooltip : "";
    tray->icon = load_tray_icon(icon_path);
    tray->userdata = userdata;

    /* Associate the uID with the userdata in the TLS table and make sure this
     * window has the shared tray callout — the rest of the routing (message,
     * wParam) is handled by the OS + the one callout. */
    tls_tray_userdata[tray->uid] = userdata;
    tray_ensure_window_subclass(tray->hwnd);

    if (tray_apply(tray, NIM_ADD) != 0) {
        hv_fail_win32(GetLastError(), "Shell_NotifyIcon (NIM_ADD) failed — tray not created");
        tls_tray_userdata.erase(tray->uid);
        if (tray->icon)
            DestroyIcon(tray->icon);
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
    if (!tray)
        return hv_fail(-1, "tray is NULL");
    HICON new_icon = load_tray_icon(icon_path);
    if (tray->icon)
        DestroyIcon(tray->icon);
    tray->icon = new_icon;
    return tray->added ? tray_apply(tray, NIM_MODIFY) : 0;
}

void heliosview_tray_destroy(heliosview_tray_t* tray)
{
    if (!tray)
        return;
    if (tray->added)
        tray_apply(tray, NIM_DELETE);
    tls_tray_userdata.erase(tray->uid); /* the window's shared callout stays (dies with the window) */
    if (tray->icon)
        DestroyIcon(tray->icon);
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