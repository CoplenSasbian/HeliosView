#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"
// HeliosView.dll — Windows implementation: windows, message loop, native-message → event conversion.
// The cross-platform interface is in heliosview.h; the win32 layer is split by subsystem:
//   heliosview_window_win32.cpp (this file) — windows / message loop / native-message conversion / dialogs
//   heliosview_tray_win32.cpp / heliosview_menu_win32.cpp — tray icons / popup menus (routing via window subclasses)
//   heliosview_webview2_win32.cpp / heliosview_notify_win32.cpp — WebView2 backend / OS toasts

/* Enable modern (visual-styled) common controls for the whole process, from the
 * library itself: HeliosView.dll embeds a Common-Controls v6 manifest resource
 * (see hv_resources.rc / hv_common_controls.manifest). The loader merges a
 * DLL's manifest dependencies into the hosting process's activation context, so
 * any app that loads HeliosView.dll gets the themed ComCtl32 v6 (instead of the
 * legacy v5 look) for message boxes and common controls - without needing its
 * own manifest. This is the library-side equivalent of the classic application
 * manifest, and it keeps the examples and consumers manifest-free. */

#include "heliosview_win32_internal.h" /* shared codecs + helpers; defines WIN32_LEAN_AND_MEAN/_WIN32_WINNT and includes windows.h/commctrl.h */
#include <wrl/client.h> /* ComPtr (taskbar / file dialogs) */
#include <shellapi.h> /* ShellExecuteW (open URL / Explorer) */
#include <objbase.h>  /* CoCreateInstance */
#include <shobjidl.h> /* IFileOpenDialog / IShellItem (file pickers) / ITaskbarList3 */
#include <dwmapi.h>   /* DwmSetWindowAttribute (backdrop / dark mode) */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <flat_map>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/* ================= Window (completes the header's opaque declaration; must be at global scope) =================
 *
 * Private to this file: every other backend addresses a window only through the
 * public API — the WebView2 backend through heliosview_window_id /
 * _is_resizable / _is_fullscreen / _from_id, the tray/menu backends through
 * hv_window_hwnd — never through this struct. */

struct heliosview_window {
    int width = 0;
    int height = 0;
    std::string title; /* UTF-8 */
    heliosview_window_style_t style = HELIOSVIEW_WINDOW_NORMAL;
    HWND hwnd = nullptr;
    void* userdata = nullptr; /* caller data (the C++ wrapper stores an object pointer) */
    HICON icon = nullptr;     /* custom window icon (owned; NULL = default) */
    bool resizable = true;    /* whether the user can resize / maximize the window */
    bool shown = false;       /* first show happened (the WINDOW_FIRST_SHOWN event fired once) */
    heliosview_show_state_t last_state = HELIOSVIEW_SHOW_NORMAL; /* last reported show state (WM_SIZE transition tracking: RESTORED fires only on a real restore) */
    int32_t min_w = 0, min_h = 0; /* minimum client size (0 = unconstrained) */
    int32_t max_w = 0, max_h = 0; /* maximum client size (0 = unconstrained) */
    bool fullscreen = false;       /* whether the window covers the whole monitor */
    RECT fs_restore_rect{};        /* pre-fullscreen window rect (restored on exit) */
    DWORD fs_restore_style = 0;    /* pre-fullscreen window style */
    DWORD fs_restore_exstyle = 0;  /* pre-fullscreen extended style */
    std::vector<RECT> drag_regions; /* client-area move regions (WM_NCHITTEST -> HTCAPTION) */

    /* Routing ids (tray callback messages, caller-registered ids) are allocated
     * from one per-window id space (next_route_id; WM_APP range, so every id is
     * a valid WM_APP message / fits a WM_COMMAND LOWORD). Each id becomes one
     * comctl32 window subclass entry (uIdSubclass = the id, dwRefData = the
     * userdata), so the id → userdata binding lives in the subclass entry — no
     * lookup table. Menu item ids are menu-local (MNS_NOTIFYBYPOS / WM_MENUCOMMAND
     * routing, see heliosview_menu_win32.cpp) and do NOT come from this space;
     * trays allocate their callback message ids here. `registry` below is the
     * PREVIOUS routing design's table
     * (id → userdata); it is dead now and kept only as leftover — deletable
     * with <flat_map>. */
    uint32_t next_route_id = WM_APP + 0x100;       /* per-window routing id allocator */
    std::flat_map<uint32_t, void*> registry;        /* dead: old routing table (id → userdata), superseded by subclasses */
};

/* ================= Window style mapping ================= */
DWORD map_win32_style(const heliosview_window_t* window)
{
    DWORD style = 0;
    switch (window->style) {
    case HELIOSVIEW_WINDOW_NORMAL:
        style = WS_OVERLAPPEDWINDOW;
        break;
    case HELIOSVIEW_WINDOW_BORDERLESS:
        style = WS_POPUP; /* borderless and titleless; fully custom */
        break;
    case HELIOSVIEW_WINDOW_FRAMELESS:
        style = WS_OVERLAPPEDWINDOW;
        break;
    default:
        style = WS_OVERLAPPEDWINDOW;
        break;
    }
    if (!window->resizable)
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    return style;
}

namespace {

/* Event object that wakes the message loop's wait (triggered by post_event / quit via hv::g_platform_wake) */
HANDLE g_wakeup_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

/* Force the Common-Controls v6 activation context, from the library itself.
 * HeliosView.dll embeds a v6 manifest dependency (hv_common_controls.manifest,
 * passed to the MSVC linker via CMake), but the loader applies a DLL's manifest
 * only to the DLL's own activation context - it does not merge it into the
 * hosting process. So the library creates an activation context from the
 * embedded resource and keeps it active: message boxes and common controls
 * render themed (modern) for every consumer, with no exe-side manifest
 * required. Created lazily on first use (not from a static initializer:
 * CreateActCtxW during DLL load re-enters the loader's SxS state and can raise
 * STATUS_SXS_CANT_GEN_ACTCTX, failing the load). */
HANDLE g_cc_ctx = INVALID_HANDLE_VALUE;
ULONG_PTR g_cc_cookie = 0;
bool g_cc_attempted = false;

void hv_ensure_common_controls_ctx()
{
    if (g_cc_attempted || g_cc_ctx != INVALID_HANDLE_VALUE)
        return;
    g_cc_attempted = true;

    const HMODULE self = GetModuleHandleW(L"HeliosView.dll");
    if (!self)
        return;
    wchar_t dll[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, dll, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;

    /* The manifest may sit at resource id 1 (CREATEPROCESS) or 2
     * (ISOLATIONAWARE) depending on the link pipeline, so pick the one that
     * actually contains the Common-Controls dependency (CMake's default
     * trustInfo-only manifest would activate without theming anything). */
    static constexpr char kNeedle[] = "Microsoft.Windows.Common-Controls";
    constexpr size_t kNeedleLen = sizeof(kNeedle) - 1;

    for (const WORD id : {WORD{1}, WORD{2}, WORD{3}}) {
        const HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(24)); /* 24 = RT_MANIFEST */
        if (!res)
            continue;
        const HGLOBAL hg = LoadResource(self, res);
        const char* data = hg ? static_cast<const char*>(LockResource(hg)) : nullptr;
        const DWORD size = res ? SizeofResource(self, res) : 0;
        if (!data || size < kNeedleLen)
            continue;
        bool has_v6 = false;
        for (DWORD i = 0; i + kNeedleLen <= size; ++i)
            if (std::memcmp(data + i, kNeedle, kNeedleLen) == 0) {
                has_v6 = true;
                break;
            }
        if (!has_v6)
            continue;

        ACTCTXW a{};
        a.cbSize = sizeof(a);
        a.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
        a.lpSource = dll;
        a.lpResourceName = MAKEINTRESOURCEW(id);
        g_cc_ctx = CreateActCtxW(&a);
        if (g_cc_ctx != INVALID_HANDLE_VALUE && ActivateActCtx(g_cc_ctx, &g_cc_cookie) != FALSE)
            return; /* stays active for the process lifetime */
        if (g_cc_ctx != INVALID_HANDLE_VALUE) {
            ReleaseActCtx(g_cc_ctx);
            g_cc_ctx = INVALID_HANDLE_VALUE;
        }
    }
}

/* Initialize the common controls (v6, loaded through the DLL manifest above).
 * Runs once, at DLL load, before main: the app's first native window (and any
 * controls it creates) is themed only after this. Idempotent. The static's
 * value is never read — the declaration exists only to run the call once at
 * load time (namespace scope cannot hold a bare statement). */
const bool g_common_controls_initialized = [] {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    return true;
}();

/* Register the platform wake callback with the cross-platform core (static init, before main) */
const bool g_wake_registered = (hv::g_platform_wake = [] { SetEvent(g_wakeup_event); }, true);

/* Per-thread library state. The win32 implementation is single-UI-thread by
 * design — the same model as the cross-platform event queue (hv::tls_event_queue in
 * heliosview_internal.h): windows are created, looked up and destroyed on the
 * thread that runs the message loop, so the live-window count is thread-local
 * state, not a process-global variable. There is no id → window registry at
 * all: a window's identity is its native handle (HWND), which events carry and
 * heliosview_window_from_id resolves via IsWindow + GWLP_USERDATA — nothing to
 * keep in sync with destroy, and a stale queued event resolves to NULL. */
struct hv_ui_state {
    int32_t window_count = 0; /* live windows on this thread (create/destroy) */
};
thread_local hv_ui_state tls_ui_state;

/* Session-end (WM_QUERYENDSESSION) callback: runs synchronously on the message-loop
 * thread before shutdown/logoff; return non-zero to veto. */
heliosview_session_end_cb g_session_end_cb = nullptr;
void* g_session_end_userdata = nullptr;

/* ================= Default native-message → event conversion (Win32 MSG → event) ================= */

heliosview_keycode_t map_vk(UINT vk)
{
    switch (vk) {
    case VK_ESCAPE: return HELIOSVIEW_KEY_ESCAPE;
    case VK_RETURN: return HELIOSVIEW_KEY_RETURN;
    case VK_SPACE:  return HELIOSVIEW_KEY_SPACE;
    case VK_LEFT:   return HELIOSVIEW_KEY_LEFT;
    case VK_RIGHT:  return HELIOSVIEW_KEY_RIGHT;
    case VK_UP:     return HELIOSVIEW_KEY_UP;
    case VK_DOWN:   return HELIOSVIEW_KEY_DOWN;
    default:
        if (vk >= 'A' && vk <= 'Z')
            return static_cast<heliosview_keycode_t>(HELIOSVIEW_KEY_A + (vk - 'A'));
        if (vk >= '0' && vk <= '9')
            return static_cast<heliosview_keycode_t>(HELIOSVIEW_KEY_0 + (vk - '0'));
        if (vk >= VK_F1 && vk <= VK_F12)
            return static_cast<heliosview_keycode_t>(HELIOSVIEW_KEY_F1 + (vk - VK_F1));
        return HELIOSVIEW_KEY_UNKNOWN;
    }
}

/* ================= Routing subclasses (caller-registered ids) =================
 *
 * The public heliosview_window_add_item / _remove_item let callers register
 * their own routing ids: each id becomes one comctl32 window subclass whose
 * uIdSubclass IS the id and whose dwRefData is the userdata — the id → userdata
 * association lives in the subclass entry itself, no lookup table. The tray and
 * menu backends do NOT go through this anymore: each of them registers its own
 * dedicated callout (hv_tray_subclass_proc / hv_menu_subclass_proc, in their
 * own files). This generic callout only serves caller-registered ids:
 *   - a message whose number is the id (tray-style WM_APP callback messages),
 *   - a WM_COMMAND whose LOWORD(wParam) is the id (menu-style command ids).
 * Every other message is forwarded down the chain (DefSubclassProc). The
 * callout never dereferences dwRefData unless one of its own messages actually
 * arrived: WM_NCDESTROY and every other message can never match a routing id
 * (ids are >= WM_APP + 0x100) nor WM_COMMAND, so they are forwarded without
 * touching the item — safe even when the item was already destroyed. */
LRESULT CALLBACK hv_routing_subclass_proc(HWND hwnd, UINT message, WPARAM wparam,
                                          LPARAM lparam, UINT_PTR id, DWORD_PTR ref)
{
    heliosview_event_t ev{};
    ev.window_id = reinterpret_cast<uintptr_t>(hwnd);
    ev.timestamp_ms = hv::now_ms();

    if (message == WM_COMMAND && LOWORD(wparam) == static_cast<UINT>(id)) {
        /* Menu item selection: TrackPopupMenu posts WM_COMMAND with the item's
         * id in LOWORD(wParam). Only a registered routing id matches here;
         * WM_COMMAND for regular child controls/buttons never collides (their
         * ids are not registered). */
        ev.type = HELIOSVIEW_EVENT_MENU_SELECT;
        ev.menu_item = LOWORD(wparam);
        ev.userdata = reinterpret_cast<void*>(ref);
        hv::queue_push(ev);
        return 1; /* handled: stop the subclass chain, no DefWindowProc */
    }

    if (message == static_cast<UINT>(id)) {
        /* Tray icon callback messages (posted by Shell_NotifyIcon when an icon
         * is clicked). The message id IS the per-window routing id (>= WM_APP),
         * which is exactly what this callout is registered under. */
        switch (lparam) {
        case WM_LBUTTONUP:       ev.type = HELIOSVIEW_EVENT_TRAY_LEFT_CLICK; break;
        case WM_LBUTTONDBLCLK:   ev.type = HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK; break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:     ev.type = HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK; break;
        case WM_MBUTTONUP:       ev.type = HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK; break;
        default:
            return 1; /* consume; not a click we translate */
        }
        ev.userdata = reinterpret_cast<void*>(ref);
        hv::queue_push(ev);
        return 1; /* consumed: this callback message belongs to a registered id */
    }

    return DefSubclassProc(hwnd, message, wparam, lparam);
}

int default_native_convert(void* native_msg, uintptr_t window_id)
{
    const MSG* msg = static_cast<const MSG*>(native_msg);
    const int64_t ts = hv::now_ms();

    /* The owning window (see heliosview_window_from_id; GWLP_USERDATA is 0 once
     * the window is being destroyed, so lookups safely become no-ops). */
    auto* win = reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(msg->hwnd, GWLP_USERDATA));

    /* Every event this message produces is queued via hv::queue_push — the shared
     * primitive heliosview_post_event also routes through. One copy is inherent
     * (the queue is a value container); the emitter owns its event, so that is
     * the only copy on this path. window_id comes from the caller (the
     * WndProc), so routing never depends on the converter knowing the platform
     * handle. `emit` queues the scratch event and reports "handled". */
    heliosview_event_t ev{};
    ev.window_id = window_id;
    auto emit = [&]() {
        ev.timestamp_ms = ts;
        hv::queue_push(ev);
        return 1;
    };

    /* Tray callback messages and menu-item WM_COMMANDs never reach this
     * converter: each registered routing id handles its own messages in its
     * subclass callout (hv_routing_subclass_proc) and consumes them before the
     * window procedure runs, so there is no lookup here. */

    switch (msg->message) {
    case WM_CLOSE:
        ev.type = HELIOSVIEW_EVENT_WINDOW_CLOSE;
        return emit();
    case WM_SIZE: {
        /* WM_SIZE conflates several things and must be split by wParam:
         *   - minimize / maximize / restore are STATE changes, reported as
         *     discrete events (WINDOW_MINIMIZED / _MAXIMIZED / _RESTORED);
         *   - SIZE_MAXHIDE / SIZE_MAXSHOW notify owned windows that ANOTHER
         *     window was maximized/restored — not a change of this window;
         *   - everything else (plain resizes, the end of a resize drag) is a
         *     real size change (WINDOW_RESIZE).
         * Restores arrive as SIZE_RESTORED — the same code path that closes
         * every ordinary resize drag and fullscreen toggle — so last_state
         * tracks the reported state and RESTORED fires only on a real
         * transition from minimized/maximized. */
        if (msg->wParam == SIZE_MAXHIDE || msg->wParam == SIZE_MAXSHOW)
            return -1; /* another window's maximize/restore; not ours */

        if (win) {
            const heliosview_show_state_t new_state =
                msg->wParam == SIZE_MINIMIZED ? HELIOSVIEW_SHOW_MINIMIZED
                : msg->wParam == SIZE_MAXIMIZED ? HELIOSVIEW_SHOW_MAXIMIZED
                : HELIOSVIEW_SHOW_NORMAL; /* SIZE_RESTORED */
            if (new_state != win->last_state) {
                win->last_state = new_state;
                heliosview_event_t st{};
                st.type = new_state == HELIOSVIEW_SHOW_MINIMIZED ? HELIOSVIEW_EVENT_WINDOW_MINIMIZED
                        : new_state == HELIOSVIEW_SHOW_MAXIMIZED ? HELIOSVIEW_EVENT_WINDOW_MAXIMIZED
                        : HELIOSVIEW_EVENT_WINDOW_RESTORED;
                st.window_id = window_id;
                st.timestamp_ms = ts;
                hv::queue_push(st); /* queued before the RESIZE below */
            }
            if (msg->wParam == SIZE_MINIMIZED)
                return 0; /* consumed: lParam is the icon size, no RESIZE event */
        }

        ev.type = HELIOSVIEW_EVENT_WINDOW_RESIZE;
        ev.width = static_cast<int32_t>(LOWORD(msg->lParam));
        ev.height = static_cast<int32_t>(HIWORD(msg->lParam));
        return emit();
    }
    case WM_ACTIVATE:
        /* WA_INACTIVE (0) = lost focus, everything else = gained focus */
        ev.type = LOWORD(msg->wParam) == WA_INACTIVE
                        ? HELIOSVIEW_EVENT_WINDOW_BLUR
                        : HELIOSVIEW_EVENT_WINDOW_FOCUS;
        return emit();
    case WM_MOVE:
        /* final position (screen coords of the top-left corner) */
        ev.type = HELIOSVIEW_EVENT_WINDOW_MOVED;
        ev.x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(msg->lParam)));
        ev.y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(msg->lParam)));
        return emit();
    case WM_MOVING: {
        /* drag in progress: lParam points at the current window rect */
        const RECT* rc = reinterpret_cast<const RECT*>(msg->lParam);
        if (rc) {
            ev.type = HELIOSVIEW_EVENT_WINDOW_MOVING;
            ev.x = rc->left;
            ev.y = rc->top;
            return emit();
        }
        return 0;
    }
    case WM_SIZING: {
        /* resize drag in progress: lParam points at the proposed window rect */
        const RECT* rc = reinterpret_cast<const RECT*>(msg->lParam);
        if (rc) {
            ev.type = HELIOSVIEW_EVENT_WINDOW_SIZING;
            ev.width = rc->right - rc->left;
            ev.height = rc->bottom - rc->top;
            return emit();
        }
        return 0;
    }
    case WM_ENABLE:
        /* wParam: TRUE = being enabled, FALSE = being disabled */
        ev.type = msg->wParam ? HELIOSVIEW_EVENT_WINDOW_ENABLED
                              : HELIOSVIEW_EVENT_WINDOW_DISABLED;
        return emit();
    case WM_SHOWWINDOW:
        /* The first show of a window produces a WINDOW_FIRST_SHOWN event (the
         * C++ wrapper maps it to Window::firstShown) — dispatched from the
         * message pipeline like every other window event, once per window.
         * Every later show/hide (wParam = fShown) becomes WINDOW_SHOWN /
         * WINDOW_HIDDEN — the WS_VISIBLE bit is exactly what these report.
         * Minimize/maximize do NOT change WS_VISIBLE, so they never reach this
         * branch: they are state changes reported from WM_SIZE. */
        if (win && !win->shown) {
            if (!msg->wParam)
                return -1; /* hidden before ever shown (show() not called yet) */
            win->shown = true;
            ev.type = HELIOSVIEW_EVENT_WINDOW_FIRST_SHOWN;
            return emit();
        }
        if (win) {
            ev.type = msg->wParam ? HELIOSVIEW_EVENT_WINDOW_SHOWN
                                  : HELIOSVIEW_EVENT_WINDOW_HIDDEN;
            return emit();
        }
        return -1;
    case WM_KEYDOWN:
        if ((msg->lParam & 0x40000000) != 0)
            return 0; /* filter keyboard auto-repeat */
        ev.type = HELIOSVIEW_EVENT_KEY_DOWN;
        ev.key = map_vk(static_cast<UINT>(msg->wParam));
        return emit();
    case WM_KEYUP:
        ev.type = HELIOSVIEW_EVENT_KEY_UP;
        ev.key = map_vk(static_cast<UINT>(msg->wParam));
        return emit();
    case WM_MOUSEMOVE:
        ev.type = HELIOSVIEW_EVENT_MOUSE_MOVE;
        break;
    case WM_LBUTTONDOWN: ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; ev.mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONDOWN: ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; ev.mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONDOWN: ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; ev.mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    case WM_LBUTTONUP:   ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   ev.mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONUP:   ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   ev.mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONUP:   ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   ev.mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    default:
        return -1; /* unhandled → hand off to DefWindowProc */
    }

    ev.x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(msg->lParam)));
    ev.y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(msg->lParam)));
    return emit();
}

} // namespace

/* ================= Window procedure (per-style, template + if constexpr) =================
 *
 * One window procedure per style, generated from a single template. `if
 * constexpr` keeps each instantiation lean: only the code that style needs is
 * compiled in, and the per-style differences (custom title bar hit-testing,
 * control-button routing) are spelled out once here. heliosview_window_show
 * picks the right instantiation + class name at creation. */

template <heliosview_window_style_t Style>
LRESULT CALLBACK heliosview_wndproc_t(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    /* WM_CREATE is sent synchronously during CreateWindowExW: register the
     * window here so messages such as WM_SIZE delivered during creation find it
     * (window_id = this hwnd).
     * FRAMELESS styles: extend the DWM frame over the top strip so the native
     * caption buttons (min/max/close) render there. */
    if (message == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* win = static_cast<heliosview_window_t*>(cs->lpCreateParams);
        if (win) {
            win->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(win));
        }
        /* FRAMELESS: immersive dark title-bar + rounded corners. The system's
         * default WS_THICKFRAME border stays (DWM-drawn, modern look). */
        if constexpr (Style == HELIOSVIEW_WINDOW_FRAMELESS) {
            BOOL value = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
            DWORD corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    /* WM_DESTROY — DefWindowProc's default posts WM_QUIT here, ending the
     * message loop. Windows are now created in the constructor, so a never-
     * shown window can be destroyed while others are alive; only the last
     * window's destruction may quit the loop. (heliosview_window_destroy
     * decrements the count before DestroyWindow, so WM_DESTROY sees the
     * remaining live windows.) */
    if (message == WM_DESTROY) {
        if (tls_ui_state.window_count > 0)
            return 0; /* more windows alive: swallow DefWindowProc's WM_QUIT */
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    /* WM_NCCALCSIZE — no visible title bar / border:
     *   - BORDERLESS: no frame at all (fully custom drawing).
     *   - FRAMELESS: keep the system's default WS_THICKFRAME border — thin,
     *     DWM-drawn and themed. The border is non-client, so the system
     *     hit-tests it directly and resizing works natively (the WM_NCHITTEST
     *     below passes non-client points back to DefWindowProc). */
    if constexpr (Style == HELIOSVIEW_WINDOW_BORDERLESS) {
        if (message == WM_NCCALCSIZE)
            return 0;
    }
    if constexpr (Style == HELIOSVIEW_WINDOW_FRAMELESS) {
        if (message == WM_NCCALCSIZE) {
            /* Handle both wParam forms: the initial call at window creation
             * arrives with wParam == FALSE (lParam is a RECT*), later resizes /
             * frame changes with wParam == TRUE (NCCALCSIZE_PARAMS*). Skipping
             * the FALSE case is why the border only appeared after the first
             * resize. */
            RECT* rc = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                              : reinterpret_cast<RECT*>(lparam);
            const int border = 8;
            rc->left += border;
            rc->right -= border;
            rc->bottom -= border;
            /* top: 0 — no title bar, the client area starts at the window top */
            return 0;
        }
    }

    /* WM_NCHITTEST — custom chrome for the non-system-chrome styles:
     *   - BORDERLESS / FRAMELESS: the whole window is the client area, so the
     *     resize edges are mapped manually (there is no system caption).
     * NORMAL leaves everything to the system. */
    if constexpr (Style != HELIOSVIEW_WINDOW_NORMAL) {
        if (message == WM_NCHITTEST) {
            auto* win = reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!win)
                return DefWindowProcW(hwnd, message, wparam, lparam);

            const POINT screen{static_cast<LONG>(static_cast<int16_t>(LOWORD(lparam))),
                               static_cast<LONG>(static_cast<int16_t>(HIWORD(lparam)))};
            POINT client = screen;
            if (!ScreenToClient(hwnd, &client))
                return DefWindowProcW(hwnd, message, wparam, lparam);

            /* Non-client points (the system WS_THICKFRAME resize border that
             * FRAMELESS keeps) go back to DefWindowProc: it returns
             * HTLEFT/HTTOP/... there, so the system resizes natively. */
            RECT cr{};
            GetClientRect(hwnd, &cr);
            if (client.x < 0 || client.y < 0
                || client.x >= cr.right || client.y >= cr.bottom)
                return DefWindowProcW(hwnd, message, wparam, lparam);

            for (const RECT& r : win->drag_regions) {
                if (client.x >= r.left && client.x < r.right
                    && client.y >= r.top && client.y < r.bottom)
                    return HTCAPTION;
            }

            /* FRAMELESS: the top strip drags the window (like a title bar).
             * Only applies when no explicit drag region is registered — and only
             * when the strip is not covered by a child window: a full-bleed
             * WebView eats the hit-test, so those apps call startDrag() from the
             * page instead. */
            if constexpr (Style == HELIOSVIEW_WINDOW_FRAMELESS) {
                if (win->drag_regions.empty() && client.y >= 0
                    && client.y < hv_title_bar_height(hwnd))
                    return HTCAPTION;
            }
            return HTCLIENT;
        }
    }

    /* WM_GETMINMAXINFO: clamp the tracking size (min/max client size -> window
     * size incl. frame). Must return 0 (handled). */
    if (message == WM_GETMINMAXINFO) {
        auto* win = reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (win && (win->min_w || win->min_h || win->max_w || win->max_h)) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
            const DWORD st = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
            /* Frameless/borderless windows are entirely client area, so the
             * min/max client size is the track size directly. */
            const bool adjust = Style == HELIOSVIEW_WINDOW_NORMAL;
            if (win->min_w || win->min_h) {
                RECT rc{0, 0, win->min_w, win->min_h};
                if (adjust)
                    AdjustWindowRect(&rc, st, FALSE);
                mmi->ptMinTrackSize.x = rc.right - rc.left;
                mmi->ptMinTrackSize.y = rc.bottom - rc.top;
            }
            if (win->max_w || win->max_h) {
                RECT rc{0, 0, win->max_w, win->max_h};
                if (adjust)
                    AdjustWindowRect(&rc, st, FALSE);
                mmi->ptMaxTrackSize.x = rc.right - rc.left;
                mmi->ptMaxTrackSize.y = rc.bottom - rc.top;
            }
        }
        return 0;
    }

    /* WM_QUERYENDSESSION: run the session-end callback synchronously (save state,
     * veto via non-zero return). */
    if (message == WM_QUERYENDSESSION) {
        if (g_session_end_cb)
            return g_session_end_cb(g_session_end_userdata) ? FALSE : TRUE;
        return TRUE; /* allow the session to end */
    }

    MSG native{};
    native.hwnd = hwnd;
    native.message = message;
    native.wParam = wparam;
    native.lParam = lparam;

    /* The library's built-in conversion always runs first (window/keyboard/mouse;
     * tray/menu routing happens earlier still, in their per-id window subclasses,
     * which consume their messages before this procedure runs). If it does not
     * handle the message (-1), try each registered converter in order; the first
     * that returns 1 (posted events via heliosview_post_event) or 0 (consumed)
     * wins. Otherwise fall through to DefWindowProc. The native handle IS the
     * window_id: it is passed to every converter, which stamps it on the
     * events it posts, so routing needs no id registry here. */
    const uintptr_t window_id = reinterpret_cast<uintptr_t>(hwnd);
    int handled = default_native_convert(&native, window_id);
    if (handled == -1) {
        for (const auto& [id, h] : hv::g_native_handlers) {
            (void)id;
            if (!h)
                continue;
            handled = h(&native, window_id);
            if (handled != -1)
                break;
        }
    }
    return handled == 1 || handled == 0 ? 0 : DefWindowProcW(hwnd, message, wparam, lparam);
}

/* Explicit instantiations, one per style (the only WndProcs the library uses). */
template LRESULT CALLBACK heliosview_wndproc_t<HELIOSVIEW_WINDOW_NORMAL>(HWND, UINT, WPARAM, LPARAM);
template LRESULT CALLBACK heliosview_wndproc_t<HELIOSVIEW_WINDOW_BORDERLESS>(HWND, UINT, WPARAM, LPARAM);
template LRESULT CALLBACK heliosview_wndproc_t<HELIOSVIEW_WINDOW_FRAMELESS>(HWND, UINT, WPARAM, LPARAM);

/* ================= Message loop ================= */

void heliosview_pump_events(void)
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            heliosview_quit();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int heliosview_run(heliosview_loop_callback frame_callback, void* userdata)
{
    hv::g_quit = false;
    while (!hv::g_quit.load()) {
        heliosview_pump_events();
        if (hv::g_quit.load())
            break;
        if (frame_callback && frame_callback(userdata) != 0) {
            hv::g_quit = true;
            break;
        }
        /* Wait for a new native message or a wake-up from post_event/postTask/quit.
         * 10ms polling timeout: does not depend on the wake event / ResetEvent timing;
         * cross-thread tasks and events are delayed at most 10ms. */
        ResetEvent(g_wakeup_event);
        const DWORD result = MsgWaitForMultipleObjectsEx(1, &g_wakeup_event, 10,
                                                         QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED)
            break;
    }
    return 0;
}

/* ================= Window ================= */

heliosview_window_t* heliosview_window_create(int width, int height, const char* title)
{
    return heliosview_window_create_ex(width, height, title, HELIOSVIEW_WINDOW_NORMAL, nullptr);
}

heliosview_window_t* heliosview_window_create_ex(int width, int height, const char* title,
                                                 heliosview_window_style_t style, void* userdata)
{
    if (!title) {
        hv_fail(-1, "title is NULL");
        return nullptr;
    }
    auto* window = hv::hv_alloc<heliosview_window>();
    window->width = width;
    window->height = height;
    window->title = title;
    window->style = style;
    window->userdata = userdata;

    /* create the native window immediately (the constructor-created model: the
     * window exists as a native window from creation; show() only makes it
     * visible). Message-loop thread. */
    hv_ensure_common_controls_ctx(); /* v6 theming for this window's controls */

    /* Pick the window class + procedure for this style. Each style gets its own
     * class (distinct lpfnWndProc instantiation, see the template above). */
    const wchar_t* class_name = L"HeliosViewWindow";
    switch (window->style) {
    case HELIOSVIEW_WINDOW_BORDERLESS:
        class_name = L"HeliosViewWindowBorderless";
        break;
    case HELIOSVIEW_WINDOW_FRAMELESS:
        class_name = L"HeliosViewWindowFrameless";
        break;
    case HELIOSVIEW_WINDOW_NORMAL:
    default:
        class_name = L"HeliosViewWindow";
        break;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = class_name;
    switch (window->style) {
    case HELIOSVIEW_WINDOW_BORDERLESS:
        wc.lpfnWndProc = &heliosview_wndproc_t<HELIOSVIEW_WINDOW_BORDERLESS>;
        break;
    case HELIOSVIEW_WINDOW_FRAMELESS:
        wc.lpfnWndProc = &heliosview_wndproc_t<HELIOSVIEW_WINDOW_FRAMELESS>;
        break;
    case HELIOSVIEW_WINDOW_NORMAL:
    default:
        wc.lpfnWndProc = &heliosview_wndproc_t<HELIOSVIEW_WINDOW_NORMAL>;
        break;
    }
    RegisterClassExW(&wc); /* re-registering is harmless (silently fails if the class exists) */

    const std::wstring title_w = utf8_to_wide(window->title);

    /* compute the window size for the preset style: NORMAL uses AdjustWindowRect
     * (client + caption + frame); BORDERLESS and FRAMELESS are entirely client
     * area (WM_NCCALCSIZE), so the requested client size is the window size. */
    const DWORD win_style = map_win32_style(window);
    RECT rect{0, 0, window->width, window->height};
    if (window->style == HELIOSVIEW_WINDOW_NORMAL)
        AdjustWindowRect(&rect, win_style, FALSE);

    window->hwnd = CreateWindowExW(0, class_name, title_w.c_str(), win_style,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   rect.right - rect.left, rect.bottom - rect.top,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), window);
    if (!window->hwnd) {
        hv_fail_win32(GetLastError(), "CreateWindowExW failed");
        hv::hv_dealloc(window);
        return nullptr;
    }

    /* already registered in WM_CREATE; fall back here if that did not happen */
    if (!GetWindowLongPtrW(window->hwnd, GWLP_USERDATA))
        SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));

    tls_ui_state.window_count++;
    return window;
}

void* heliosview_window_userdata(const heliosview_window_t* window)
{
    return window ? window->userdata : nullptr;
}

void heliosview_window_set_userdata(heliosview_window_t* window, void* userdata)
{
    if (window)
        window->userdata = userdata;
}

/* Look a window up by its native handle (the window_id an event carries).
 * The handle must still exist and be one of this library's windows, so a
 * destroyed window safely resolves to NULL (stale queued events become no-ops)
 * and a foreign window that reused the handle never hands back a garbage
 * pointer. Message-loop thread. */
heliosview_window_t* heliosview_window_from_id(uintptr_t window_id)
{
    const HWND hwnd = reinterpret_cast<HWND>(window_id);
    if (!hwnd || !IsWindow(hwnd))
        return nullptr;
    /* Only this library's windows store a heliosview_window_t* in GWLP_USERDATA;
     * verify the window is one of ours before trusting it. The window CLASS is
     * the stable identity here - NOT GWLP_WNDPROC: a WebView attached to the
     * window installs a comctl32 subclass (SetWindowSubclass), which replaces
     * GWLP_WNDPROC with comctl32's master procedure, so the procedure comparison
     * would reject our own windows (and break all event routing for WebView
     * windows). A stale/reused HWND from another process or window class does
     * not carry one of these class names. */
    wchar_t cls[32];
    if (!GetClassNameW(hwnd, cls, 32))
        return nullptr;
    if (std::wcscmp(cls, L"HeliosViewWindow") != 0
        && std::wcscmp(cls, L"HeliosViewWindowBorderless") != 0
        && std::wcscmp(cls, L"HeliosViewWindowFrameless") != 0)
        return nullptr;
    return reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int heliosview_window_count(void)
{
    return static_cast<int>(tls_ui_state.window_count);
}

void heliosview_window_destroy(heliosview_window_t* window)
{
    if (!window)
        return;
    tls_ui_state.window_count--;
    if (window->hwnd) {
        SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(window->hwnd); /* triggers WM_DESTROY → PostQuitMessage → message loop exits */
    }
    if (window->icon)
        DestroyIcon(window->icon);
    hv::hv_dealloc(window);
}

int heliosview_window_show(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    ShowWindow(window->hwnd, SW_SHOW);
    return 0;
}

/* The window's native handle (HWND) — the window_id its events carry. 0 until
 * the native window is created (heliosview_window_create_ex). */
uintptr_t heliosview_window_id(const heliosview_window_t* window)
{
    return window ? reinterpret_cast<uintptr_t>(window->hwnd) : 0;
}

/* Allocate the window's next routing id (WM_APP range) WITHOUT registering
 * anything; 0 when the window or its native handle is missing. Internal: the
 * tray backend allocates its callback message id from this space, and
 * heliosview_window_add_item wraps it for caller-registered ids. */
uint32_t hv_window_next_routing_id(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return 0;
    return window->next_route_id++;
}

uint32_t heliosview_window_add_item(heliosview_window_t* window, void* userdata)
{
    /* A caller-registered routing id is one comctl32 window subclass: key = the
     * id (uIdSubclass, from the per-window id space), value = the userdata
     * (dwRefData); the generic callout (hv_routing_subclass_proc) above routes
     * the id's messages. */
    const uint32_t id = hv_window_next_routing_id(window);
    if (!id) {
        hv_fail(-1, "window is NULL or its native window is not created");
        return 0;
    }
    if (!SetWindowSubclass(window->hwnd, hv_routing_subclass_proc,
                           static_cast<UINT_PTR>(id), reinterpret_cast<DWORD_PTR>(userdata))) {
        window->next_route_id--; /* keep the id space contiguous on failure */
        hv_fail(-1, "SetWindowSubclass failed (routing id not registered)");
        return 0;
    }
    return id;
}

int heliosview_window_remove_item(heliosview_window_t* window, uint32_t id)
{
    if (!window || !window->hwnd)
        return hv_fail(-1, "window is NULL or its native window is not created");
    if (!RemoveWindowSubclass(window->hwnd, hv_routing_subclass_proc,
                              static_cast<UINT_PTR>(id)))
        return hv_fail(-1, "routing id is not registered (removed twice?)");
    return 0;
}

/* ================= Window operations (show state / close / focus) ================= */

int heliosview_window_show_state(heliosview_window_t* window, heliosview_show_state_t state)
{
    if (!window || !window->hwnd)
        return -1;
    switch (state) {
    case HELIOSVIEW_SHOW_NORMAL:
        ShowWindow(window->hwnd, SW_RESTORE);
        break;
    case HELIOSVIEW_SHOW_MINIMIZED:
        ShowWindow(window->hwnd, SW_MINIMIZE);
        break;
    case HELIOSVIEW_SHOW_MAXIMIZED:
        ShowWindow(window->hwnd, SW_MAXIMIZE);
        break;
    default:
        return -1;
    }
    return 0;
}

heliosview_show_state_t heliosview_window_state(const heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return HELIOSVIEW_SHOW_NORMAL;
    if (IsIconic(window->hwnd))
        return HELIOSVIEW_SHOW_MINIMIZED;
    if (IsZoomed(window->hwnd))
        return HELIOSVIEW_SHOW_MAXIMIZED;
    return HELIOSVIEW_SHOW_NORMAL;
}

int heliosview_window_close(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    /* go through the event pipeline: WndProc turns it into a WINDOW_CLOSE event, and the app decides whether to destroy */
    return PostMessageW(window->hwnd, WM_CLOSE, 0, 0) ? 0 : -1;
}

int heliosview_window_focus(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    SetForegroundWindow(window->hwnd);
    SetFocus(window->hwnd);
    return 0;
}

int heliosview_window_is_visible(const heliosview_window_t* window)
{
    return window && window->hwnd && IsWindowVisible(window->hwnd) ? 1 : 0;
}

/* ================= Window operations (position / size / title / centering / opacity) ================= */

int heliosview_window_set_position(heliosview_window_t* window, int32_t x, int32_t y)
{
    if (!window || !window->hwnd)
        return -1;
    return SetWindowPos(window->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER) ? 0 : -1;
}

int heliosview_window_position(const heliosview_window_t* window, int32_t* out_x, int32_t* out_y)
{
    if (!window || !window->hwnd || !out_x || !out_y)
        return -1;
    RECT rc{};
    if (!GetWindowRect(window->hwnd, &rc))
        return -1;
    *out_x = rc.left;
    *out_y = rc.top;
    return 0;
}

int heliosview_window_set_size(heliosview_window_t* window, int32_t width, int32_t height)
{
    if (!window || !window->hwnd)
        return -1;
    RECT rc{0, 0, width, height};
    if (window->style == HELIOSVIEW_WINDOW_NORMAL)
        AdjustWindowRect(&rc, map_win32_style(window), FALSE);
    window->width = width;
    window->height = height;
    return SetWindowPos(window->hwnd, nullptr, 0, 0,
                        rc.right - rc.left, rc.bottom - rc.top,
                        SWP_NOMOVE | SWP_NOZORDER) ? 0 : -1;
}

int heliosview_window_size(const heliosview_window_t* window, int32_t* out_width, int32_t* out_height)
{
    if (!window || !window->hwnd || !out_width || !out_height)
        return -1;
    RECT rc{};
    if (!GetClientRect(window->hwnd, &rc))
        return -1;
    *out_width = rc.right;
    *out_height = rc.bottom;
    return 0;
}

int heliosview_window_set_title(heliosview_window_t* window, const char* title)
{
    if (!window || !window->hwnd || !title)
        return -1;
    window->title = title;
    const std::wstring wt = utf8_to_wide(title);
    return SetWindowTextW(window->hwnd, wt.c_str()) ? 0 : -1;
}

int heliosview_window_center(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    RECT rc{};
    GetWindowRect(window->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    /* center in the current monitor's work area (multi-monitor aware) */
    HMONITOR monitor = MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);

    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    return SetWindowPos(window->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER) ? 0 : -1;
}

int heliosview_window_set_opacity(heliosview_window_t* window, float opacity)
{
    if (!window || !window->hwnd)
        return -1;
    if (opacity < 0.0f)
        opacity = 0.0f;
    if (opacity > 1.0f)
        opacity = 1.0f;
    const LONG_PTR ex = GetWindowLongPtrW(window->hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(window->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(window->hwnd, 0, static_cast<BYTE>(opacity * 255.0f), LWA_ALPHA);
    return 0;
}

int heliosview_window_hide(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    return ShowWindow(window->hwnd, SW_HIDE) ? 0 : -1;
}

int heliosview_window_set_topmost(heliosview_window_t* window, int on)
{
    if (!window || !window->hwnd)
        return -1;
    return SetWindowPos(window->hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE) ? 0 : -1;
}

int heliosview_window_set_icon(heliosview_window_t* window, const char* icon_path)
{
    if (!window || !window->hwnd)
        return -1;
    HICON new_icon = nullptr;
    if (icon_path && *icon_path) {
        const std::wstring wpath = utf8_to_wide(icon_path);
        new_icon = static_cast<HICON>(LoadImageW(nullptr, wpath.c_str(), IMAGE_ICON, 0, 0,
                                                 LR_LOADFROMFILE));
        if (!new_icon)
            return -1;
    }
    if (window->icon)
        DestroyIcon(window->icon);
    window->icon = new_icon;
    SendMessageW(window->hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(new_icon));
    SendMessageW(window->hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(new_icon));
    return 0;
}

int heliosview_window_minimize(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    return ShowWindow(window->hwnd, SW_MINIMIZE) ? 0 : -1;
}

int heliosview_window_maximize(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    return ShowWindow(window->hwnd, SW_MAXIMIZE) ? 0 : -1;
}

int heliosview_window_restore(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    return ShowWindow(window->hwnd, SW_RESTORE) ? 0 : -1;
}

int heliosview_window_toggle_maximize(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    return ShowWindow(window->hwnd, IsZoomed(window->hwnd) ? SW_RESTORE : SW_MAXIMIZE) ? 0 : -1;
}

int heliosview_window_set_resizable(heliosview_window_t* window, int resizable)
{
    if (!window)
        return -1;
    window->resizable = resizable != 0;
    if (!window->hwnd)
        return 0; /* applied at creation (map_win32_style reads the flag) */
    /* Toggle only the resize-related bits: preserve the current style as-is
     * (WS_VISIBLE, the caption, WS_VSYNC, ...). For BORDERLESS the thick frame
     * is not present; resizing there is manual (WM_NCHITTEST edge mapping), so
     * only the maximize box changes. */
    LONG_PTR style = GetWindowLongPtrW(window->hwnd, GWL_STYLE);
    if (window->resizable)
        style |= (window->style == HELIOSVIEW_WINDOW_BORDERLESS)
                     ? WS_MAXIMIZEBOX
                     : (WS_THICKFRAME | WS_MAXIMIZEBOX);
    else
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(window->hwnd, GWL_STYLE, style);
    /* Re-frame the window so the (possibly removed) thick frame is applied. */
    SetWindowPos(window->hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    return 0;
}

int heliosview_window_is_resizable(const heliosview_window_t* window)
{
    return window && window->resizable ? 1 : 0;
}

int heliosview_window_add_drag_region(heliosview_window_t* window, int32_t x, int32_t y,
                                      int32_t width, int32_t height)
{
    if (!window || width <= 0 || height <= 0)
        return -1;
    RECT r{x, y, x + width, y + height};
    window->drag_regions.push_back(r);
    return 0;
}

int heliosview_window_clear_drag_regions(heliosview_window_t* window)
{
    if (!window)
        return -1;
    window->drag_regions.clear();
    return 0;
}

int heliosview_window_start_drag(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    /* Standard programmatic title-bar drag: release any capture and replay a
     * non-client left-button-down on the caption, which makes the system run the
     * move loop. Needed when a full-bleed WebView covers the window (the WebView
     * child receives all mouse input, so WM_NCHITTEST never reaches the title
     * strip); the page calls this on mousedown over its own title bar. */
    ReleaseCapture();
    const DWORD pos = GetMessagePos();
    SendMessageW(window->hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                 MAKELPARAM(static_cast<int16_t>(LOWORD(pos)),
                            static_cast<int16_t>(HIWORD(pos))));
    return 0;
}

uint32_t heliosview_window_dpi(const heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return 0;
    return static_cast<uint32_t>(GetDpiForWindow(window->hwnd));
}

int32_t heliosview_window_title_bar_height(const heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return 0;
    if (window->style != HELIOSVIEW_WINDOW_FRAMELESS)
        return 0; /* only the frameless style reserves a title-bar strip */
    return hv_title_bar_height(window->hwnd);
}

int heliosview_window_set_min_size(heliosview_window_t* window, int32_t min_width, int32_t min_height)
{
    if (!window)
        return -1;
    window->min_w = min_width < 0 ? 0 : min_width;
    window->min_h = min_height < 0 ? 0 : min_height;
    return 0; /* WM_GETMINMAXINFO reads the fields on the next resize */
}

int heliosview_window_set_max_size(heliosview_window_t* window, int32_t max_width, int32_t max_height)
{
    if (!window)
        return -1;
    window->max_w = max_width < 0 ? 0 : max_width;
    window->max_h = max_height < 0 ? 0 : max_height;
    return 0;
}

int heliosview_window_flash(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = window->hwnd;
    fi.dwFlags = FLASHW_ALL;
    fi.uCount = 3; /* flash 3 times, then stop */
    fi.dwTimeout = 0; /* system default caret blink rate */
    FlashWindowEx(&fi);
    return 0;
}

int heliosview_window_flash_until_focus(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = window->hwnd;
    fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG; /* flash until the window is focused */
    fi.uCount = 0;
    fi.dwTimeout = 0;
    FlashWindowEx(&fi);
    return 0;
}

int heliosview_window_set_fullscreen(heliosview_window_t* window, int on)
{
    if (!window || !window->hwnd)
        return -1;
    if ((on != 0) == window->fullscreen)
        return 0; /* already in the requested state */

    if (on) {
        /* Save the current geometry + style so exiting fullscreen can restore them. */
        GetWindowRect(window->hwnd, &window->fs_restore_rect);
        window->fs_restore_style = static_cast<DWORD>(GetWindowLongPtrW(window->hwnd, GWL_STYLE));
        window->fs_restore_exstyle = static_cast<DWORD>(GetWindowLongPtrW(window->hwnd, GWL_EXSTYLE));

        /* Cover the whole monitor (the work area would leave the taskbar visible;
         * true fullscreen hides it). Drop the frame/caption so nothing is drawn. */
        HMONITOR hmon = MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hmon, &mi);

        SetWindowLongPtrW(window->hwnd, GWL_STYLE,
                          window->fs_restore_style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
        SetWindowPos(window->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        window->fullscreen = true;
    } else {
        /* Restore the saved geometry and window style. */
        SetWindowLongPtrW(window->hwnd, GWL_STYLE, window->fs_restore_style);
        SetWindowLongPtrW(window->hwnd, GWL_EXSTYLE, window->fs_restore_exstyle);
        SetWindowPos(window->hwnd, nullptr,
                     window->fs_restore_rect.left, window->fs_restore_rect.top,
                     window->fs_restore_rect.right - window->fs_restore_rect.left,
                     window->fs_restore_rect.bottom - window->fs_restore_rect.top,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        window->fullscreen = false;
    }
    return 0;
}

int heliosview_window_is_fullscreen(const heliosview_window_t* window)
{
    return window && window->fullscreen ? 1 : 0;
}

int heliosview_window_set_enabled(heliosview_window_t* window, int enabled)
{
    if (!window || !window->hwnd)
        return -1;
    EnableWindow(window->hwnd, enabled != 0);
    return 0;
}

int heliosview_window_is_enabled(const heliosview_window_t* window)
{
    return window && window->hwnd && IsWindowEnabled(window->hwnd) ? 1 : 0;
}

int heliosview_set_session_end_callback(heliosview_session_end_cb callback, void* userdata)
{
    g_session_end_cb = callback;
    g_session_end_userdata = userdata;
    return 0;
}

int heliosview_set_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return -1;
    /* SetProcessDpiAwarenessContext (Win10 1607+): per-monitor v2 is best.
     * The DPI_AWARENESS_CONTEXT handles are documented negative values; define
     * them explicitly so we don't depend on the SDK's _WIN32_WINNT gate. */
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(void*);
    const auto setCtx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setCtx) {
        const auto perMonitorV2 = reinterpret_cast<void*>((long long)-4);  /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        const auto systemAware = reinterpret_cast<void*>((long long)-2);   /* DPI_AWARENESS_CONTEXT_SYSTEM_AWARE */
        if (setCtx(perMonitorV2) || setCtx(systemAware))
            return 0;
        return -1;
    }
    /* Older fallback: SetProcessDPIAware (Win Vista+) */
    using SetProcessDPIAwareFn = BOOL(WINAPI*)(void);
    const auto setAware = reinterpret_cast<SetProcessDPIAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
    return (setAware && setAware()) ? 0 : -1;
}

/* ================= Screen / monitor geometry ================= */

namespace {

/* Fill a heliosview_rect_t from a MONITORINFO work area. Returns 0 on success. */
int fill_rect_work(RECT rc, heliosview_rect_t* out)
{
    if (!out)
        return -1;
    out->x = rc.left;
    out->y = rc.top;
    out->width = rc.right - rc.left;
    out->height = rc.bottom - rc.top;
    return 0;
}

/* Work area of the monitor selected by `hmon`. Returns 0 on success. */
int work_area_of(HMONITOR hmon, heliosview_rect_t* out_rect)
{
    if (!hmon)
        return -1;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi))
        return -1;
    return fill_rect_work(mi.rcWork, out_rect);
}

} // namespace

int heliosview_screen_work_area(int32_t x, int32_t y, heliosview_rect_t* out_rect)
{
    if (!out_rect)
        return -1;
    POINT pt{x, y};
    HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    return work_area_of(hmon, out_rect);
}

int heliosview_window_work_area(const heliosview_window_t* window, heliosview_rect_t* out_rect)
{
    if (!window || !window->hwnd || !out_rect)
        return -1;
    HMONITOR hmon = MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST);
    return work_area_of(hmon, out_rect);
}

int heliosview_primary_work_area(heliosview_rect_t* out_rect)
{
    if (!out_rect)
        return -1;
    return work_area_of(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), out_rect);
}

int heliosview_cursor_position(int32_t* out_x, int32_t* out_y)
{
    if (!out_x || !out_y)
        return -1;
    POINT pt{};
    if (!GetCursorPos(&pt))
        return -1;
    *out_x = pt.x;
    *out_y = pt.y;
    return 0;
}

/* ================= Taskbar progress (ITaskbarList3) ================= */

namespace {

/* Lazily created once on the message-loop thread; kept for the process lifetime. */
Microsoft::WRL::ComPtr<ITaskbarList3> hv_taskbar()
{
    static Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
    if (!taskbar) {
        /* the message-loop thread may already have COM initialized; ensure an
         * apartment so the instance survives the call */
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)co;
        CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
    }
    return taskbar;
}

} // namespace

int heliosview_window_set_progress(heliosview_window_t* window, uint32_t value, uint32_t max)
{
    if (!window || !window->hwnd || max == 0)
        return -1;
    auto taskbar = hv_taskbar();
    if (!taskbar)
        return -1;
    if (value > max)
        value = max;
    HRESULT hr = taskbar->SetProgressState(window->hwnd, TBPF_NORMAL);
    if (SUCCEEDED(hr))
        hr = taskbar->SetProgressValue(window->hwnd, value, max);
    return SUCCEEDED(hr) ? 0 : -1;
}

int heliosview_window_set_progress_state(heliosview_window_t* window,
                                         heliosview_progress_state_t state)
{
    if (!window || !window->hwnd)
        return -1;
    auto taskbar = hv_taskbar();
    if (!taskbar)
        return -1;
    TBPFLAG flag = TBPF_NOPROGRESS;
    switch (state) {
    case HELIOSVIEW_PROGRESS_NORMAL:       flag = TBPF_NORMAL; break;
    case HELIOSVIEW_PROGRESS_INDETERMINATE: flag = TBPF_INDETERMINATE; break;
    case HELIOSVIEW_PROGRESS_ERROR:         flag = TBPF_ERROR; break;
    case HELIOSVIEW_PROGRESS_PAUSED:        flag = TBPF_PAUSED; break;
    case HELIOSVIEW_PROGRESS_NONE:
    default:                                flag = TBPF_NOPROGRESS; break;
    }
    return SUCCEEDED(taskbar->SetProgressState(window->hwnd, flag)) ? 0 : -1;
}

int heliosview_window_clear_progress(heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return -1;
    auto taskbar = hv_taskbar();
    if (!taskbar)
        return -1;
    return SUCCEEDED(taskbar->SetProgressState(window->hwnd, TBPF_NOPROGRESS)) ? 0 : -1;
}

/* ================= Backdrop & dark mode (DWM) ================= */

/* DWM window attributes: DWMWA_USE_IMMERSIVE_DARK_MODE (20) and
 * DWMWA_SYSTEMBACKDROP_TYPE (38, Win11 22621+). Define fallbacks so the code
 * compiles against older Windows SDKs. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

/* DWMSBT_* backdrop types (from the Win11 SDK) */
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 0
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2 /* Mica */
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3 /* Acrylic */
#endif

int heliosview_window_set_backdrop(heliosview_window_t* window, heliosview_backdrop_t backdrop)
{
    if (!window || !window->hwnd)
        return -1;
    int type = DWMSBT_NONE;
    switch (backdrop) {
    case HELIOSVIEW_BACKDROP_MICA:    type = DWMSBT_MAINWINDOW; break;
    case HELIOSVIEW_BACKDROP_ACRYLIC: type = DWMSBT_TRANSIENTWINDOW; break;
    case HELIOSVIEW_BACKDROP_NONE:
    default:                          type = DWMSBT_NONE; break;
    }
    const HRESULT hr = DwmSetWindowAttribute(window->hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                             &type, sizeof(type));
    return SUCCEEDED(hr) ? 0
                         : hv_fail_hresult(hr, "DwmSetWindowAttribute (backdrop) failed");
}

int heliosview_window_set_dark_mode(heliosview_window_t* window, int on)
{
    if (!window || !window->hwnd)
        return -1;
    const BOOL enable = on != 0;
    const HRESULT hr = DwmSetWindowAttribute(window->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                             &enable, sizeof(enable));
    return SUCCEEDED(hr) ? 0
                         : hv_fail_hresult(hr, "DwmSetWindowAttribute (dark mode) failed");
}

/* ================= Native dialogs & system helpers ================= */

namespace {

/* Allocate raw bytes through the library allocator (freed with heliosview_free). */
char* hv_alloc_bytes(size_t n)
{
    if (hv::g_allocator.alloc)
        return static_cast<char*>(hv::g_allocator.alloc(n, hv::g_allocator.context));
    return static_cast<char*>(std::malloc(n));
}

/* Keep a CoInitialize'd apartment alive for the modal dialog (the message-loop
 * thread may or may not already have COM). We intentionally do NOT uninitialize:
 * the thread stays COM-enabled for the process lifetime. */
HRESULT hv_ensure_com()
{
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE)
        return co;
    return S_OK;
}

/* Parse a "Name1 (*.ext)|*.ext|Name2|..." filter string into COMDLG_FILTERSPEC
 * pairs. An empty/absent filter yields the "All files" default. */
void build_filterspec(const char* filter, std::vector<std::wstring>& owned,
                      std::vector<COMDLG_FILTERSPEC>& specs)
{
    std::vector<std::string> parts;
    if (filter && *filter) {
        std::string s(filter);
        size_t start = 0;
        for (size_t pos = 0; pos <= s.size(); ++pos) {
            if (pos == s.size() || s[pos] == '|') {
                parts.push_back(s.substr(start, pos - start));
                start = pos + 1;
            }
        }
    }
    if (parts.empty() || (parts.size() & 1) != 0) {
        parts = {"All files (*.*)", "*.*"};
    }
    specs.clear();
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        owned.push_back(utf8_to_wide(parts[i]));
        owned.push_back(utf8_to_wide(parts[i + 1]));
        specs.push_back({owned[owned.size() - 2].c_str(), owned.back().c_str()});
    }
}

/* Return a fresh library-allocated UTF-8 copy of a wide string. */
char* hv_strdup_utf8(const std::wstring& w)
{
    const std::string s = wide_to_utf8(w);
    if (s.empty())
        return nullptr;
    char* out = hv_alloc_bytes(s.size() + 1);
    if (!out)
        return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

} // namespace

int heliosview_select_folder(heliosview_window_t* window, const char* title, char** out_path)
{
    if (out_path)
        *out_path = nullptr;
    if (!out_path)
        return -1;
    hv_ensure_common_controls_ctx(); /* themed file dialog */
    if (FAILED(hv_ensure_com()))
        return -1;

    int result = -1;
    do {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog))))
            break;
        if (FAILED(dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM)))
            break;
        if (title && *title)
            dialog->SetTitle(utf8_to_wide(title).c_str());

        const HRESULT hr = dialog->Show(window && window->hwnd ? window->hwnd : nullptr);
        if (FAILED(hr)) {
            result = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : -static_cast<int>(hr);
            break;
        }

        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item)))
            break;
        LPWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            break;
        char* out = hv_strdup_utf8(path);
        CoTaskMemFree(path);
        if (!out)
            break;
        *out_path = out;
        result = 1;
    } while (false);
    return result;
}

int heliosview_open_files(heliosview_window_t* window, const char* title, const char* filter,
                          int multi, char*** out_paths)
{
    if (out_paths)
        *out_paths = nullptr;
    if (!out_paths)
        return -1;
    hv_ensure_common_controls_ctx(); /* themed file dialog */
    if (FAILED(hv_ensure_com()))
        return -1;

    int result = -1;
    std::vector<std::wstring> filter_owned;
    std::vector<COMDLG_FILTERSPEC> filter_specs;
    build_filterspec(filter, filter_owned, filter_specs);

    do {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog))))
            break;
        DWORD options = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        if (multi)
            options |= FOS_ALLOWMULTISELECT;
        if (FAILED(dialog->SetOptions(options)))
            break;
        if (title && *title)
            dialog->SetTitle(utf8_to_wide(title).c_str());
        if (!filter_specs.empty())
            dialog->SetFileTypes(static_cast<UINT>(filter_specs.size()), filter_specs.data());

        const HRESULT hr = dialog->Show(window && window->hwnd ? window->hwnd : nullptr);
        if (FAILED(hr)) {
            result = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : -static_cast<int>(hr);
            break;
        }

        std::vector<char*> paths;
        if (multi) {
            Microsoft::WRL::ComPtr<IShellItemArray> items;
            if (FAILED(dialog->GetResults(&items)))
                break;
            DWORD count = 0;
            if (FAILED(items->GetCount(&count)))
                break;
            for (DWORD i = 0; i < count; ++i) {
                Microsoft::WRL::ComPtr<IShellItem> item;
                if (FAILED(items->GetItemAt(i, &item)))
                    break;
                LPWSTR path = nullptr;
                if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
                    continue;
                char* utf8 = hv_strdup_utf8(path);
                CoTaskMemFree(path);
                if (utf8)
                    paths.push_back(utf8);
            }
        } else {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (FAILED(dialog->GetResult(&item)))
                break;
            LPWSTR path = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
                break;
            char* utf8 = hv_strdup_utf8(path);
            CoTaskMemFree(path);
            if (!utf8)
                break;
            paths.push_back(utf8);
        }

        if (paths.empty()) {
            result = 0;
            break;
        }
        char** arr = reinterpret_cast<char**>(hv_alloc_bytes((paths.size() + 1) * sizeof(char*)));
        if (!arr) {
            for (char* p : paths)
                heliosview_free(p);
            break;
        }
        std::memcpy(arr, paths.data(), paths.size() * sizeof(char*));
        arr[paths.size()] = nullptr;
        *out_paths = arr;
        result = static_cast<int>(paths.size());
    } while (false);
    return result;
}

void heliosview_free_paths(char** paths)
{
    if (!paths)
        return;
    for (char** p = paths; *p; ++p)
        heliosview_free(*p);
    heliosview_free(paths);
}

int heliosview_save_file(heliosview_window_t* window, const char* title, const char* filter,
                         const char* default_name, char** out_path)
{
    if (out_path)
        *out_path = nullptr;
    if (!out_path)
        return -1;
    hv_ensure_common_controls_ctx(); /* themed file dialog */
    if (FAILED(hv_ensure_com()))
        return -1;

    int result = -1;
    std::vector<std::wstring> filter_owned;
    std::vector<COMDLG_FILTERSPEC> filter_specs;
    build_filterspec(filter, filter_owned, filter_specs);

    do {
        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog))))
            break;
        if (FAILED(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT)))
            break;
        if (title && *title)
            dialog->SetTitle(utf8_to_wide(title).c_str());
        if (!filter_specs.empty())
            dialog->SetFileTypes(static_cast<UINT>(filter_specs.size()), filter_specs.data());
        if (default_name && *default_name)
            dialog->SetFileName(utf8_to_wide(default_name).c_str());

        const HRESULT hr = dialog->Show(window && window->hwnd ? window->hwnd : nullptr);
        if (FAILED(hr)) {
            result = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : -static_cast<int>(hr);
            break;
        }

        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item)))
            break;
        LPWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            break;
        char* out = hv_strdup_utf8(path);
        CoTaskMemFree(path);
        if (!out)
            break;
        *out_path = out;
        result = 1;
    } while (false);
    return result;
}

/* ================= Message box ================= */

int heliosview_message_box(heliosview_window_t* window, heliosview_message_type_t type,
                           heliosview_message_buttons_t buttons,
                           const char* title, const char* message)
{
    hv_ensure_common_controls_ctx(); /* themed (modern) message box, no exe manifest needed */
    UINT flags = 0;
    switch (type) {
    case HELIOSVIEW_MESSAGE_INFO:     flags |= MB_ICONINFORMATION; break;
    case HELIOSVIEW_MESSAGE_WARNING:  flags |= MB_ICONWARNING; break;
    case HELIOSVIEW_MESSAGE_ERROR:    flags |= MB_ICONERROR; break;
    case HELIOSVIEW_MESSAGE_QUESTION: flags |= MB_ICONQUESTION; break;
    default: break;
    }
    switch (buttons) {
    case HELIOSVIEW_MESSAGE_OK:          flags |= MB_OK; break;
    case HELIOSVIEW_MESSAGE_OK_CANCEL:   flags |= MB_OKCANCEL; break;
    case HELIOSVIEW_MESSAGE_YES_NO:      flags |= MB_YESNO; break;
    case HELIOSVIEW_MESSAGE_YES_NO_CANCEL: flags |= MB_YESNOCANCEL; break;
    case HELIOSVIEW_MESSAGE_RETRY_CANCEL: flags |= MB_RETRYCANCEL; break;
    case HELIOSVIEW_MESSAGE_ABORT_RETRY_IGNORE: flags |= MB_ABORTRETRYIGNORE; break;
    default:                             flags |= MB_OK; break;
    }
    const HWND hwnd = window && window->hwnd ? window->hwnd : nullptr;
    const std::wstring wt = utf8_to_wide(title ? title : "");
    const std::wstring wm = utf8_to_wide(message ? message : "");
    const int r = MessageBoxW(hwnd, wm.c_str(), wt.c_str(), flags);
    switch (r) {
    case IDOK:    return HELIOSVIEW_MESSAGE_RESULT_OK;
    case IDCANCEL: return HELIOSVIEW_MESSAGE_RESULT_CANCEL;
    case IDYES:   return HELIOSVIEW_MESSAGE_RESULT_YES;
    case IDNO:    return HELIOSVIEW_MESSAGE_RESULT_NO;
    case IDRETRY: return HELIOSVIEW_MESSAGE_RESULT_RETRY;
    case IDABORT: return HELIOSVIEW_MESSAGE_RESULT_ABORT;
    case IDIGNORE: return HELIOSVIEW_MESSAGE_RESULT_IGNORE;
    default:      return HELIOSVIEW_MESSAGE_RESULT_NONE;
    }
}

/* ================= System helpers ================= */

int heliosview_open_url(const char* url)
{
    if (!url || !*url)
        return -1;
    const std::wstring wurl = utf8_to_wide(url);
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wurl.c_str(),
                                                             nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32)
        return 0;
    /* ShellExecuteW failures are small positive SE_ERR_* values; format whatever
     * the system knows about them. */
    return hv_fail_win32(static_cast<DWORD>(r), "ShellExecuteW failed to open the URL");
}

int heliosview_show_in_folder(const char* path)
{
    if (!path || !*path)
        return -1;
    const std::wstring wpath = utf8_to_wide(path);
    /* /select,"<path>" (quoted: the path may contain spaces) */
    const std::wstring args = L"/select,\"" + wpath + L"\"";
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                                             args.c_str(), nullptr, SW_SHOWNORMAL));
    if (r > 32)
        return 0;
    /* ShellExecuteW failures are small positive SE_ERR_* values; format whatever
     * the system knows about them. */
    return hv_fail_win32(static_cast<DWORD>(r), "ShellExecuteW failed to open Explorer");
}

int heliosview_clipboard_set_text(const char* text)
{
    if (!text)
        return -1;
    if (!OpenClipboard(nullptr))
        return -1;
    const std::wstring w = utf8_to_wide(text);
    const SIZE_T bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    int result = -1;
    if (h) {
        /* Fill the memory BEFORE SetClipboardData: once that call succeeds the
         * system owns h and the application must not write to it again (the old
         * order — SetClipboardData first, memcpy after — left the clipboard
         * holding the uninitialized GlobalAlloc garbage). */
        void* dst = GlobalLock(h);
        if (dst) {
            std::memcpy(dst, w.c_str(), bytes);
            GlobalUnlock(h);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h) != nullptr)
                result = 0;
        }
        /* GlobalFree is not needed after a successful SetClipboardData (the
         * clipboard owns the memory); free only on failure. */
        if (result != 0)
            GlobalFree(h);
    }
    CloseClipboard();
    return result;
}

int heliosview_clipboard_get_text(char** out)
{
    if (out)
        *out = nullptr;
    if (!out)
        return -1;
    if (!OpenClipboard(nullptr))
        return -1;
    const HANDLE h = GetClipboardData(CF_UNICODETEXT);
    int result = -1;
    if (h) {
        const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
        if (w) {
            std::wstring ws(w);
            GlobalUnlock(h);
            char* utf8 = hv_strdup_utf8(ws);
            if (utf8) {
                *out = utf8;
                result = 1;
            } else if (ws.empty()) {
                result = 0;
            }
        }
    }
    CloseClipboard();
    return result;
}
