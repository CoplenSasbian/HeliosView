// HeliosView.dll — the library's hidden host window.
//
// Some subsystems need a native window that belongs to the library rather than
// to the application:
//   - Shell_NotifyIcon delivers a tray icon's callback messages, and Explorer
//     broadcasts TaskbarCreated, to an HWND;
//   - TrackPopupMenu needs an owner HWND when a menu is shown without an
//     application window: the selection is delivered to it, and it is what makes
//     the popup dismiss when the user clicks elsewhere.
//
// One hidden top-level window per thread serves them all. It is created lazily
// on first use and kept for the thread's lifetime: it is invisible, has no
// message traffic of its own, and destroying it while a subsystem might still
// use it would only add a lifetime hazard.
//
// This file knows nothing about the subsystems: each one registers its own
// message handler the first time it needs the host (hv_host_window(handler)).
// The window procedure walks the registered handlers in registration order and
// gives the message to DefWindowProc when nobody consumed it.
//
// Deliberately NOT a comctl32 subclass chain: the window is ours, so there is
// nothing to hook; and subclass procedures demonstrably do not receive
// WM_MENUCOMMAND, which the menu backend needs.

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"    /* last-error state used by the shared helpers */
#include "heliosview_win32_internal.h" /* windows.h + hv_host_message_fn */

#include <mutex>
#include <vector>

namespace {

constexpr const wchar_t* kHostClass = L"HeliosViewHost";

/* Handlers registered for this thread's host window, in registration order.
 * Thread-local like the window itself (it is created on, and dispatches to, the
 * message-loop thread). */
inline thread_local std::vector<hv_host_message_fn> tls_host_handlers;
inline thread_local HWND tls_host_hwnd = nullptr;

LRESULT CALLBACK hv_host_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    for (hv_host_message_fn handler : tls_host_handlers) {
        if (handler && handler(hwnd, message, wparam, lparam))
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

HWND hv_host_window(hv_host_message_fn handler)
{
    if (handler) {
        bool registered = false;
        for (hv_host_message_fn known : tls_host_handlers) {
            if (known == handler) {
                registered = true;
                break;
            }
        }
        if (!registered)
            tls_host_handlers.push_back(handler);
    }

    if (tls_host_hwnd && IsWindow(tls_host_hwnd))
        return tls_host_hwnd;

    static std::once_flag s_class_registered;
    std::call_once(s_class_registered, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = hv_host_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kHostClass;
        RegisterClassExW(&wc);
    });

    /* Unshown top-level window: a menu owner must be able to become the
     * foreground window, which a message-only (HWND_MESSAGE) window cannot. */
    tls_host_hwnd = CreateWindowExW(0, kHostClass, L"HeliosViewHost", WS_OVERLAPPED,
                                    0, 0, 0, 0, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    return tls_host_hwnd;
}

bool hv_host_is(HWND hwnd)
{
    if (!hwnd)
        return false;
    wchar_t cls[64];
    return GetClassNameW(hwnd, cls, 64) != 0 && std::wcscmp(cls, kHostClass) == 0;
}
