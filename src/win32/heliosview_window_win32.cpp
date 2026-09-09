#include <HeliosView/heliosview.h>
#include "../heliosview_backend.h"
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
#include <shellapi.h> /* ShellExecuteW (open URL / Explorer / runas) */
#include <shlobj.h>   /* SHGetKnownFolderPath (system_path) */
#include <knownfolders.h> /* FOLDERID_* (system_path) */
#include <winreg.h>   /* RegOpenKeyExW/RegQueryValueExW (os_version) */
#include <reason.h>   /* SHTDN_REASON_* (system_power) */
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
    uint32_t flags = 0;       /* heliosview_window_flag_t bits the window was created with */
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
    /* Explicit flag restrictions (see heliosview_window_flag_t): the three
     * affordance bits, when any of them is present, define the complete set. */
    const uint32_t affordances = HELIOSVIEW_WINDOW_FLAG_CLOSABLE
                               | HELIOSVIEW_WINDOW_FLAG_MINIMIZABLE
                               | HELIOSVIEW_WINDOW_FLAG_RESIZABLE;
    if (window->flags & affordances) {
        if (!(window->flags & HELIOSVIEW_WINDOW_FLAG_RESIZABLE))
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        if (!(window->flags & HELIOSVIEW_WINDOW_FLAG_MINIMIZABLE))
            style &= ~WS_MINIMIZEBOX;
        if (!(window->flags & HELIOSVIEW_WINDOW_FLAG_CLOSABLE))
            style &= ~WS_SYSMENU;
    }
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
    case VK_TAB:    return HELIOSVIEW_KEY_TAB;
    case VK_BACK:   return HELIOSVIEW_KEY_BACKSPACE;
    case VK_DELETE: return HELIOSVIEW_KEY_DELETE;
    case VK_INSERT: return HELIOSVIEW_KEY_INSERT;
    case VK_HOME:   return HELIOSVIEW_KEY_HOME;
    case VK_END:    return HELIOSVIEW_KEY_END;
    case VK_PRIOR:  return HELIOSVIEW_KEY_PAGE_UP;
    case VK_NEXT:   return HELIOSVIEW_KEY_PAGE_DOWN;
    case VK_LSHIFT: return HELIOSVIEW_KEY_LEFT_SHIFT;
    case VK_RSHIFT: return HELIOSVIEW_KEY_RIGHT_SHIFT;
    case VK_LCONTROL: return HELIOSVIEW_KEY_LEFT_CTRL;
    case VK_RCONTROL: return HELIOSVIEW_KEY_RIGHT_CTRL;
    case VK_LMENU:  return HELIOSVIEW_KEY_LEFT_ALT;
    case VK_RMENU:  return HELIOSVIEW_KEY_RIGHT_ALT;
    case VK_LWIN:   return HELIOSVIEW_KEY_LEFT_META;
    case VK_RWIN:   return HELIOSVIEW_KEY_RIGHT_META;
    case VK_OEM_MINUS:  return HELIOSVIEW_KEY_MINUS;
    case VK_OEM_PLUS:   return HELIOSVIEW_KEY_EQUAL;
    case VK_OEM_4:      return HELIOSVIEW_KEY_LEFT_BRACKET;
    case VK_OEM_6:      return HELIOSVIEW_KEY_RIGHT_BRACKET;
    case VK_OEM_5:      return HELIOSVIEW_KEY_BACKSLASH;
    case VK_OEM_1:      return HELIOSVIEW_KEY_SEMICOLON;
    case VK_OEM_7:      return HELIOSVIEW_KEY_APOSTROPHE;
    case VK_OEM_3:      return HELIOSVIEW_KEY_GRAVE;
    case VK_OEM_COMMA:  return HELIOSVIEW_KEY_COMMA;
    case VK_OEM_PERIOD: return HELIOSVIEW_KEY_PERIOD;
    case VK_OEM_2:      return HELIOSVIEW_KEY_SLASH;
    case VK_CAPITAL:    return HELIOSVIEW_KEY_CAPS_LOCK;
    case VK_NUMLOCK:    return HELIOSVIEW_KEY_NUM_LOCK;
    case VK_SCROLL:     return HELIOSVIEW_KEY_SCROLL_LOCK;
    case VK_SNAPSHOT:   return HELIOSVIEW_KEY_PRINT_SCREEN;
    case VK_PAUSE:      return HELIOSVIEW_KEY_PAUSE;
    case VK_APPS:       return HELIOSVIEW_KEY_MENU;
    case VK_NUMPAD0:    return HELIOSVIEW_KEY_NUMPAD_0;
    case VK_NUMPAD1:    return HELIOSVIEW_KEY_NUMPAD_1;
    case VK_NUMPAD2:    return HELIOSVIEW_KEY_NUMPAD_2;
    case VK_NUMPAD3:    return HELIOSVIEW_KEY_NUMPAD_3;
    case VK_NUMPAD4:    return HELIOSVIEW_KEY_NUMPAD_4;
    case VK_NUMPAD5:    return HELIOSVIEW_KEY_NUMPAD_5;
    case VK_NUMPAD6:    return HELIOSVIEW_KEY_NUMPAD_6;
    case VK_NUMPAD7:    return HELIOSVIEW_KEY_NUMPAD_7;
    case VK_NUMPAD8:    return HELIOSVIEW_KEY_NUMPAD_8;
    case VK_NUMPAD9:    return HELIOSVIEW_KEY_NUMPAD_9;
    case VK_DECIMAL:    return HELIOSVIEW_KEY_NUMPAD_DECIMAL;
    case VK_DIVIDE:     return HELIOSVIEW_KEY_NUMPAD_DIVIDE;
    case VK_MULTIPLY:   return HELIOSVIEW_KEY_NUMPAD_MULTIPLY;
    case VK_SUBTRACT:   return HELIOSVIEW_KEY_NUMPAD_SUBTRACT;
    case VK_ADD:        return HELIOSVIEW_KEY_NUMPAD_ADD;
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

/* Modifier state at the time of the message. GetKeyState (not GetAsyncKeyState)
 * is the right one here: it reflects the keyboard state the message was posted
 * with, and works on the message-loop thread. */
uint32_t map_modifiers()
{
    uint32_t m = HELIOSVIEW_MOD_NONE;
    auto down = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
    if (down(VK_LSHIFT))   m |= HELIOSVIEW_MOD_LEFT_SHIFT;
    if (down(VK_RSHIFT))   m |= HELIOSVIEW_MOD_RIGHT_SHIFT;
    if (down(VK_LCONTROL)) m |= HELIOSVIEW_MOD_LEFT_CTRL;
    if (down(VK_RCONTROL)) m |= HELIOSVIEW_MOD_RIGHT_CTRL;
    if (down(VK_LMENU))    m |= HELIOSVIEW_MOD_LEFT_ALT;
    if (down(VK_RMENU))    m |= HELIOSVIEW_MOD_RIGHT_ALT;
    if (down(VK_LWIN))     m |= HELIOSVIEW_MOD_LEFT_META;
    if (down(VK_RWIN))     m |= HELIOSVIEW_MOD_RIGHT_META;
    if (m & (HELIOSVIEW_MOD_LEFT_SHIFT | HELIOSVIEW_MOD_RIGHT_SHIFT))
        m |= HELIOSVIEW_MOD_SHIFT;
    if (m & (HELIOSVIEW_MOD_LEFT_CTRL | HELIOSVIEW_MOD_RIGHT_CTRL))
        m |= HELIOSVIEW_MOD_CTRL;
    if (m & (HELIOSVIEW_MOD_LEFT_ALT | HELIOSVIEW_MOD_RIGHT_ALT))
        m |= HELIOSVIEW_MOD_ALT;
    if (m & (HELIOSVIEW_MOD_LEFT_META | HELIOSVIEW_MOD_RIGHT_META))
        m |= HELIOSVIEW_MOD_META;
    if (GetKeyState(VK_CAPITAL) & 0x0001)
        m |= HELIOSVIEW_MOD_CAPS_LOCK;
    if (GetKeyState(VK_NUMLOCK) & 0x0001)
        m |= HELIOSVIEW_MOD_NUM_LOCK;
    return m;
}

/* UTF-8 encode one codepoint; returns the byte count (1..4). */
int utf8_encode_cp(uint32_t cp, char* out)
{
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
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
    case WM_SYSKEYDOWN: /* Alt+key is still a key press; the system menu is separate */
        ev.type = HELIOSVIEW_EVENT_KEY_DOWN;
        ev.key = map_vk(static_cast<UINT>(msg->wParam));
        ev.modifiers = map_modifiers();
        /* bit 30 = the key was already down: OS auto-repeat. Reported through the
         * flag instead of being dropped, so callers decide whether to act on it. */
        if ((msg->lParam & 0x40000000) != 0)
            ev.flags |= HELIOSVIEW_EVENT_FLAG_KEY_REPEAT;
        return emit();
    case WM_KEYUP:
    case WM_SYSKEYUP:
        ev.type = HELIOSVIEW_EVENT_KEY_UP;
        ev.key = map_vk(static_cast<UINT>(msg->wParam));
        ev.modifiers = map_modifiers();
        return emit();
    case WM_CHAR: {
        /* Text entry. TranslateMessage turns WM_KEYDOWN into WM_CHAR using the
         * active keyboard layout, and IME commits arrive here as WM_CHAR too, so
         * this is the single text-input path. wParam is one UTF-16 code unit; a
         * character outside the BMP arrives as two (surrogate pair), so the high
         * half is buffered until its low half shows up. */
        static thread_local wchar_t pending_high_surrogate = 0;
        const wchar_t unit = static_cast<wchar_t>(msg->wParam);
        if (unit < 0x20 || unit == 0x7F)
            return 1; /* backspace/tab/CR/LF/ESC are key events, not text */

        auto emit_codepoint = [&](uint32_t cp) {
            ev.type = HELIOSVIEW_EVENT_TEXT_INPUT;
            ev.modifiers = map_modifiers();
            ev.text_len = static_cast<uint32_t>(utf8_encode_cp(cp, ev.text));
            ev.text[ev.text_len] = '\0';
            return emit();
        };

        if (pending_high_surrogate != 0) {
            const wchar_t high = pending_high_surrogate;
            pending_high_surrogate = 0;
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                return emit_codepoint(0x10000u + ((static_cast<uint32_t>(high) - 0xD800u) << 10)
                                                + (static_cast<uint32_t>(unit) - 0xDC00u));
            }
            /* Orphan high surrogate: report one replacement character, then fall
             * through so the unit that arrived instead is not lost. */
            emit_codepoint(0xFFFD);
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pending_high_surrogate = unit;
            return 1; /* wait for the low half */
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF)
            return emit_codepoint(0xFFFD); /* lone low surrogate */
        return emit_codepoint(static_cast<uint32_t>(unit));
    }
    case WM_MOUSEMOVE:
        ev.type = HELIOSVIEW_EVENT_MOUSE_MOVE;
        break;
    case WM_LBUTTONDOWN: ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; ev.mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONDOWN: ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; ev.mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONDOWN: ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; ev.mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    case WM_LBUTTONUP:   ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   ev.mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONUP:   ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   ev.mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONUP:   ev.type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   ev.mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        ev.type = (msg->message == WM_XBUTTONDOWN) ? HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN
                                                   : HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;
        ev.mouse_button = (HIWORD(msg->wParam) == XBUTTON1) ? HELIOSVIEW_MOUSE_X1
                                                           : HELIOSVIEW_MOUSE_X2;
        break;
    default:
        return -1; /* unhandled → hand off to DefWindowProc */
    }

    ev.x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(msg->lParam)));
    ev.y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(msg->lParam)));
    ev.modifiers = map_modifiers();
    return emit();
}

/* ================= Native filter pipeline (middleware) =================
 *
 * The filter chain runs BEFORE the built-in conversion: each registered filter
 * may pre-process, call next() to forward downstream, and post-process. The
 * terminal stage is the built-in conversion -> legacy converters -> DefWindowProcW.
 * File scope on purpose: a namespace cannot be declared inside a function. */

struct FilterChainState {
    const hv::heliosview_filter_entry* filters;
    size_t count;
    size_t current_index;
    HWND hwnd;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
};

void run_next_filter(heliosview_native_context_t* ctx, void* next_ud)
{
    auto* state = static_cast<FilterChainState*>(next_ud);
    if (state->current_index < state->count) {
        const auto& entry = state->filters[state->current_index++];
        entry.filter(ctx, run_next_filter, state, entry.userdata);
    } else {
        /* Terminal stage: default library conversion -> legacy handlers -> DefWindowProcW */
        int handled = default_native_convert(static_cast<MSG*>(ctx->native_msg), ctx->window_id);
        if (handled == -1) {
            for (const auto& [id, h] : hv::g_native_handlers) {
                if (!h)
                    continue;
                handled = h(ctx->native_msg, ctx->window_id);
                if (handled != -1)
                    break;
            }
        }
        if (handled == 1 || handled == 0) {
            ctx->is_handled = 1;
            ctx->result = 0;
        } else {
            ctx->result = DefWindowProcW(state->hwnd, state->message, state->wparam, state->lparam);
        }
    }
}

} // namespace

/* ================= Backend entry points (see heliosview_backend.h) =================
 *
 * File scope (outside the anonymous namespace) because the core links against
 * them. Window bookkeeping — live count and id → window lookup — lives in the
 * core's thread-local registry (hv::tls_windows, heliosview_internal.h): this
 * backend registers a window once its HWND exists, unregisters it before
 * destroying it, and validates a handle on demand below. The core exposes that
 * as heliosview_window_count / heliosview_window_from_id. */

const char* hv_backend_name()
{
    return "win32";
}

bool hv_backend_window_alive(uintptr_t window_id)
{
    const HWND hwnd = reinterpret_cast<HWND>(window_id);
    if (!hwnd || !IsWindow(hwnd))
        return false;
    /* Only this library's windows carry one of these class names; a stale or
     * reused HWND from another process/window class is rejected here. The CLASS
     * is the stable identity — NOT GWLP_WNDPROC: a WebView attached to the
     * window installs a comctl32 subclass (SetWindowSubclass), which replaces
     * GWLP_WNDPROC with comctl32's master procedure, so comparing procedures
     * would reject our own windows and break all event routing. */
    wchar_t cls[32];
    if (!GetClassNameW(hwnd, cls, 32))
        return false;
    return std::wcscmp(cls, L"HeliosViewWindow") == 0
        || std::wcscmp(cls, L"HeliosViewWindowBorderless") == 0
        || std::wcscmp(cls, L"HeliosViewWindowFrameless") == 0;
}

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
     * unregisters the window first, so WM_DESTROY sees the remaining live
     * windows.) */
    if (message == WM_DESTROY) {
        if (hv::hv_window_count() > 0)
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

    /* Menu routing: WM_MENUCOMMAND (a selection in one of this window's menus)
     * and WM_INITMENUPOPUP (refresh action state before a menu pops up). Handled
     * here rather than in a comctl32 subclass because WM_MENUCOMMAND is not
     * delivered through the subclass chain. */
    if (hv_menu_handle_message(hwnd, message, wparam, lparam))
        return 0;

    MSG native{};
    native.hwnd = hwnd;
    native.message = message;
    native.wParam = wparam;
    native.lParam = lparam;

    const uintptr_t window_id = reinterpret_cast<uintptr_t>(hwnd);

    /* Fast path when no middleware filters are registered */
    if (hv::g_native_filters.empty()) {
        int handled = default_native_convert(&native, window_id);
        if (handled == -1) {
            for (const auto& [id, h] : hv::g_native_handlers) {
                if (!h)
                    continue;
                handled = h(&native, window_id);
                if (handled != -1)
                    break;
            }
        }
        return handled == 1 || handled == 0 ? 0 : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    /* Pipeline filter dispatch (middleware / onion model). The snapshot is owned
     * by the core and only rebuilt when the registry changes, so this path does
     * not allocate per message. */
    const std::vector<hv::heliosview_filter_entry>& active_filters = hv::native_filters_snapshot();

    heliosview_native_context_t ctx{};
    ctx.window_id = window_id;
    ctx.native_msg = &native;
    ctx.result = 0;
    ctx.is_handled = 0;

    FilterChainState state{
        active_filters.data(),
        active_filters.size(),
        0,
        hwnd,
        message,
        wparam,
        lparam
    };

    run_next_filter(&ctx, &state);
    return static_cast<LRESULT>(ctx.result);
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
        /* action shortcuts (menu accelerators): TranslateAccelerator turns a
         * matching key message into WM_COMMAND, which the window procedure
         * routes to the action. Must run before TranslateMessage/DispatchMessage. */
        if (hv_menu_translate_accelerator(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int heliosview_run(heliosview_loop_callback frame_callback, void* userdata)
{
    hv::g_quit = false;
    while (!hv::g_quit.load()) {
        heliosview_pump_events();
        /* Fire any loop timers (delay/interval) that have come due. */
        hv::run_due_timers();
        if (hv::g_quit.load())
            break;
        if (frame_callback && frame_callback(userdata) != 0) {
            hv::g_quit = true;
            break;
        }
        /* Wait for a new native message, a wake-up from post_event/postTask/quit,
         * or the next due loop timer — sleeping exactly until the next task
         * instead of polling. When no timer is scheduled, fall back to a 10ms
         * poll so the loop does not depend on wake-event / ResetEvent timing
         * (cross-thread tasks and events are then delayed at most 10ms). */
        ResetEvent(g_wakeup_event);
        const int64_t timer_wait = hv::next_timer_wait_ms();
        const DWORD timeout = (timer_wait < 0)
                                  ? 10
                                  : static_cast<DWORD>(std::min<int64_t>(timer_wait, 0x7FFFFFFF));
        const DWORD result = MsgWaitForMultipleObjectsEx(1, &g_wakeup_event, timeout,
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
    return heliosview_window_create_ex2(width, height, title, style,
                                        HELIOSVIEW_WINDOW_FLAG_NONE, userdata);
}

heliosview_window_t* heliosview_window_create_ex2(int width, int height, const char* title,
                                                  heliosview_window_style_t style, uint32_t flags,
                                                  void* userdata)
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
    window->flags = flags;
    window->userdata = userdata;
    /* On Windows the three title-bar flags all mean "no native caption; the
     * client area is the whole window" — i.e. the FRAMELESS layout, which is
     * what WM_NCCALCSIZE in that window procedure implements. */
    if (style == HELIOSVIEW_WINDOW_NORMAL
        && (flags & (HELIOSVIEW_WINDOW_FLAG_TITLEBAR_HIDDEN
                     | HELIOSVIEW_WINDOW_FLAG_TITLEBAR_TRANSPARENT
                     | HELIOSVIEW_WINDOW_FLAG_FULL_SIZE_CONTENT)))
        window->style = HELIOSVIEW_WINDOW_FRAMELESS;

    /* create the native window immediately (the constructor-created model: the
     * window exists as a native window from creation; show() only makes it
     * visible). Message-loop thread. */
    static std::once_flag s_dpi_init_once;
    std::call_once(s_dpi_init_once, [] {
        heliosview_set_dpi_awareness();
    });
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
    const DWORD ex_style = (window->flags & HELIOSVIEW_WINDOW_FLAG_TOOLWINDOW) ? WS_EX_TOOLWINDOW : 0;
    RECT rect{0, 0, window->width, window->height};
    if (window->style == HELIOSVIEW_WINDOW_NORMAL)
        AdjustWindowRect(&rect, win_style, FALSE);

    window->hwnd = CreateWindowExW(ex_style, class_name, title_w.c_str(), win_style,
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

    /* core-owned registry: heliosview_window_count / _from_id read this */
    hv::hv_register_window(reinterpret_cast<uintptr_t>(window->hwnd), window);
    /* a window created after heliosview_menu_set_app_menu still gets the bar */
    hv_menu_apply_app_menu(window->hwnd);
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

/* Look a window up by its native handle (the window_id an event carries):
 * heliosview_window_from_id / heliosview_window_count live in the core
 * (src/heliosview.cpp) and read the registry this backend fills; the backend
 * contributes only the handle validation (hv_backend_window_alive above), so a
 * destroyed or reused HWND resolves to NULL and a stale queued event is a no-op.
 * Message-loop thread. */

void heliosview_window_destroy(heliosview_window_t* window)
{
    if (!window)
        return;
    /* Unregister BEFORE DestroyWindow: the WM_DESTROY handler decides whether
     * the message loop may quit by looking at the remaining live windows. */
    if (window->hwnd) {
        hv::hv_unregister_window(reinterpret_cast<uintptr_t>(window->hwnd));
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
    return heliosview_window_set_icon_ex(window, icon_path, HELIOSVIEW_ICON_FLAG_NONE);
}

int heliosview_window_set_icon_ex(heliosview_window_t* window, const char* icon_path, uint32_t flags)
{
    (void)flags; /* HELIOSVIEW_ICON_FLAG_TEMPLATE is a macOS concept */
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

uint32_t heliosview_window_flags(const heliosview_window_t* window)
{
    return window ? window->flags : 0;
}

float heliosview_window_scale_factor(const heliosview_window_t* window)
{
    if (!window || !window->hwnd)
        return 1.0f;
    const UINT dpi = GetDpiForWindow(window->hwnd);
    return dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
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
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "window is NULL or its native window is not created");
    /* DWMWA_SYSTEMBACKDROP_TYPE exists on Windows 11 22H2 (build 22621) and
     * later only. On Windows 10 DwmSetWindowAttribute fails with E_INVALIDARG,
     * which would surface as an opaque negated HRESULT; report the honest
     * "unsupported here" instead so callers can degrade. NONE is already the
     * Windows 10 default, so it stays a successful no-op there. */
    if (hv_os_build() < 22621) {
        if (backdrop == HELIOSVIEW_BACKDROP_NONE)
            return 0;
        return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED,
                       "system backdrop (Mica/Acrylic) requires Windows 11 22H2 (build 22621) or later");
    }
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
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "window is NULL or its native window is not created");
    const BOOL enable = on != 0;
    /* Attribute 20 is Windows 10 20H1 (build 19041)+; 1809-1909 only accept the
     * pre-release attribute 19, so fall back to it on E_INVALIDARG. */
    HRESULT hr = DwmSetWindowAttribute(window->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                       &enable, sizeof(enable));
    if (hr == E_INVALIDARG) {
        constexpr DWORD kDarkModeBefore20H1 = 19;
        hr = DwmSetWindowAttribute(window->hwnd, kDarkModeBefore20H1, &enable, sizeof(enable));
    }
    if (SUCCEEDED(hr))
        return 0;
    if (hr == E_INVALIDARG && hv_os_build() < 17763)
        return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED,
                       "dark-mode title bar requires Windows 10 1809 (build 17763) or later");
    return hv_fail_hresult(hr, "DwmSetWindowAttribute (dark mode) failed");
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

/* Convert heliosview_file_filter_t array into COMDLG_FILTERSPEC pairs.
 * An empty/absent filter array yields the "All files" default.
 *
 * Two passes on purpose: COMDLG_FILTERSPEC stores raw pointers into `owned`, so
 * every std::wstring must exist before the pointers are taken. Filling `owned`
 * while storing pointers into it would dangle them as soon as the vector grows
 * (a move relocates the wstring objects, and short strings live in their SSO
 * buffer). */
void build_filterspec(const heliosview_file_filter_t* filters, size_t filter_count,
                      std::vector<std::wstring>& owned,
                      std::vector<COMDLG_FILTERSPEC>& specs)
{
    /* ---- pass 1: all wide strings ---- */
    specs.clear();
    owned.clear();

    if (!filters || filter_count == 0) {
        owned.push_back(L"All files (*.*)");
        owned.push_back(L"*.*");
    } else {
        for (size_t i = 0; i < filter_count; ++i) {
            const auto& f = filters[i];
            std::string ext_spec;
            if (f.extensions && *f.extensions) {
                /* Semicolon-separated: "png;jpg" or "*.png;*.jpg" -> "*.png;*.jpg" */
                std::string exts(f.extensions);
                size_t start = 0;
                while (start < exts.size()) {
                    size_t pos = exts.find(';', start);
                    if (pos == std::string::npos)
                        pos = exts.size();
                    std::string item = exts.substr(start, pos - start);
                    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
                    while (!item.empty() && item.back() == ' ') item.pop_back();
                    if (!item.empty()) {
                        if (!ext_spec.empty())
                            ext_spec += ';';
                        if (item.front() != '*') {
                            if (item.front() != '.')
                                ext_spec += "*.";
                            else
                                ext_spec += '*';
                        }
                        ext_spec += item;
                    }
                    start = pos + 1;
                }
            }
            if (ext_spec.empty())
                ext_spec = "*.*";

            std::string name_spec = f.name && *f.name ? f.name : ext_spec;
            if (name_spec.find('(') == std::string::npos) {
                name_spec += " (" + ext_spec + ")";
            }

            owned.push_back(utf8_to_wide(name_spec));
            owned.push_back(utf8_to_wide(ext_spec));
        }
    }

    /* ---- pass 2: point the COM filter specs at the now-final strings ---- */
    specs.reserve(owned.size() / 2);
    for (size_t i = 0; i + 1 < owned.size(); i += 2)
        specs.push_back({owned[i].c_str(), owned[i + 1].c_str()});
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

int heliosview_open_files(heliosview_window_t* window, const char* title,
                          const heliosview_file_filter_t* filters, size_t filter_count,
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
    build_filterspec(filters, filter_count, filter_owned, filter_specs);

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

int heliosview_save_file(heliosview_window_t* window, const char* title,
                         const heliosview_file_filter_t* filters, size_t filter_count,
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
    build_filterspec(filters, filter_count, filter_owned, filter_specs);

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

int heliosview_open_path(const char* path)
{
    if (!path || !*path)
        return -1;
    const std::wstring wpath = utf8_to_wide(path);
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wpath.c_str(),
                                                             nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32)
        return 0;
    return hv_fail_win32(static_cast<DWORD>(r), "ShellExecuteW failed to open the path");
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

int heliosview_run_program(const char* exe, const char* args, heliosview_program_show_t show)
{
    if (!exe || !*exe)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "exe is NULL or empty");
    /* CreateProcessW (the process launcher; ShellExecuteW stays for association
     * opening) may scribble on the command-line buffer, so keep a mutable wide
     * copy. A bare name (no path separator) is resolved via the PATH search —
     * matching posix_spawnp on POSIX — by leaving lpApplicationName NULL so the
     * first command-line token becomes the executable; a path with a separator
     * is pinned exactly through lpApplicationName. */
    const std::wstring wexe = utf8_to_wide(exe);
    const bool has_sep = wexe.find(L'\\') != std::wstring::npos ||
                         wexe.find(L'/') != std::wstring::npos;
    const wchar_t* app = has_sep ? wexe.c_str() : nullptr;
    std::wstring cmdline = wexe;
    if (args && *args) {
        cmdline += L' ';
        cmdline += utf8_to_wide(args);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = (show == HELIOSVIEW_PROGRAM_SHOW_HIDDEN) ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi))
        return hv_fail_win32(GetLastError(), "CreateProcessW failed to run the program");
    /* Fire-and-forget: the handles only matter for waiting on the process. */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

int heliosview_run_program_wait(const char* exe, const char* args,
                                heliosview_program_show_t show, int* out_exit_code)
{
    if (out_exit_code)
        *out_exit_code = -1; /* exit code 0 is valid; never report it on failure */
    if (!exe || !*exe)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "exe is NULL or empty");
    const std::wstring wexe = utf8_to_wide(exe);
    const bool has_sep = wexe.find(L'\\') != std::wstring::npos ||
                         wexe.find(L'/') != std::wstring::npos;
    const wchar_t* app = has_sep ? wexe.c_str() : nullptr;
    std::wstring cmdline = wexe;
    if (args && *args) {
        cmdline += L' ';
        cmdline += utf8_to_wide(args);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = (show == HELIOSVIEW_PROGRAM_SHOW_HIDDEN) ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi))
        return hv_fail_win32(GetLastError(), "CreateProcessW failed to run the program");
    CloseHandle(pi.hThread);
    /* Block until the child exits — call from a worker thread, not the loop. */
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (out_exit_code)
        *out_exit_code = static_cast<int>(code);
    return 0;
}

int heliosview_run_program_elevated(const char* exe, const char* args)
{
    if (!exe || !*exe)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "exe is NULL or empty");
    const std::wstring wexe = utf8_to_wide(exe);
    const std::wstring wargs = args ? utf8_to_wide(args) : std::wstring();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS; /* useful if a wait variant is added */
    sei.lpVerb = L"runas";               /* UAC elevation prompt */
    sei.lpFile = wexe.c_str();
    sei.lpParameters = wargs.empty() ? nullptr : wargs.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        /* On failure GetLastError carries a small SE_ERR_* value (e.g.
         * SE_ERR_ACCESSDENIED when the UAC prompt is cancelled), which
         * hv_fail_win32 formats from its SE_ERR table. */
        return hv_fail_win32(GetLastError(), "ShellExecuteExW failed to run elevated");
    }
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return 0;
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

int heliosview_system_path(heliosview_system_path_kind_t kind, char** out)
{
    if (!out)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "out is NULL");
    *out = nullptr;

    if (kind == HELIOSVIEW_SYSTEM_PATH_TEMP) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, buf);
        if (n == 0)
            return hv_fail_win32(GetLastError(), "GetTempPathW failed");
        if (n >= MAX_PATH)
            return hv_fail(HELIOSVIEW_ERROR_GENERIC, "system temp path is too long");
        *out = hv_strdup_utf8(buf);
        return *out ? 1 : hv_fail(HELIOSVIEW_ERROR_GENERIC, "allocation failed");
    }

    const KNOWNFOLDERID* fid = nullptr;
    switch (kind) {
    case HELIOSVIEW_SYSTEM_PATH_HOME:       fid = &FOLDERID_Profile; break;
    case HELIOSVIEW_SYSTEM_PATH_DOCUMENTS:  fid = &FOLDERID_Documents; break;
    case HELIOSVIEW_SYSTEM_PATH_DOWNLOADS:  fid = &FOLDERID_Downloads; break;
    case HELIOSVIEW_SYSTEM_PATH_DESKTOP:    fid = &FOLDERID_Desktop; break;
    case HELIOSVIEW_SYSTEM_PATH_APPDATA:    fid = &FOLDERID_RoamingAppData; break;
    case HELIOSVIEW_SYSTEM_PATH_LOCAL_DATA: fid = &FOLDERID_LocalAppData; break;
    case HELIOSVIEW_SYSTEM_PATH_CACHE:      fid = &FOLDERID_LocalAppData; break;
    case HELIOSVIEW_SYSTEM_PATH_TEMP:       /* handled above */ break;
    default:
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "unknown system path kind");
    }
    /* FOLDERID can be unset for the current user in rare configurations; treat
     * E_FAIL as "no such folder" (0) rather than an error. */
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(*fid, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr)) {
        if (hr == E_FAIL)
            return 0;
        return hv_fail_hresult(hr, "SHGetKnownFolderPath failed");
    }
    const std::wstring w(raw);
    CoTaskMemFree(raw);
    *out = hv_strdup_utf8(w);
    return *out ? 1 : hv_fail(HELIOSVIEW_ERROR_GENERIC, "allocation failed");
}

int heliosview_os_version(char* buf, size_t size)
{
    if (!buf || size == 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "buf is NULL or size is 0");
    buf[0] = '\0';

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0; /* cannot determine: report "" */
    const auto read = [key](const wchar_t* name, std::wstring& val) {
        wchar_t raw[128];
        DWORD len = sizeof(raw);
        if (RegQueryValueExW(key, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(raw), &len) == ERROR_SUCCESS &&
            len >= sizeof(wchar_t))
            val.assign(raw, len / sizeof(wchar_t) - 1); /* len includes the NUL */
    };
    std::wstring product, display, build;
    read(L"ProductName", product);
    read(L"DisplayVersion", display);
    read(L"CurrentBuildNumber", build);
    RegCloseKey(key);

    /* e.g. "Windows 11 Pro 24H2 (build 26100)" */
    std::wstring v;
    if (!product.empty()) v = product;
    if (!display.empty()) { if (!v.empty()) v += L' '; v += display; }
    if (!build.empty())  { if (!v.empty()) v += L' '; v += L"(build " + build + L")"; }
    if (!v.empty()) {
        const std::string utf8 = wide_to_utf8(v);
        std::snprintf(buf, size, "%s", utf8.c_str());
    }
    return 0;
}

int heliosview_system_power(heliosview_power_action_t action)
{
    UINT flags = 0;
    switch (action) {
    case HELIOSVIEW_POWER_SHUTDOWN: flags = EWX_SHUTDOWN | EWX_POWEROFF; break;
    case HELIOSVIEW_POWER_REBOOT:   flags = EWX_REBOOT; break;
    case HELIOSVIEW_POWER_LOGOFF:   flags = EWX_LOGOFF; break;
    default:
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "unknown power action");
    }
    if (!ExitWindowsEx(flags, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED))
        return hv_fail_win32(GetLastError(), "ExitWindowsEx failed");
    return 0;
}

/* ================= Global hotkeys =================
 *
 * Reuses the library's hidden per-thread host window (hv_host_window) instead of
 * creating another one: each hotkey is registered against that HWND, and its
 * window procedure already walks the registered message handlers, so WM_HOTKEY
 * reaches us through hv_hotkey_handle_host_message. Everything runs on the
 * message-loop thread (the host window's thread) so WM_HOTKEY is pumped and
 * dispatched where the loop lives.
 * macOS: RegisterEventHotKey; Linux: no universal API — the stub reports
 * HELIOSVIEW_ERROR_UNSUPPORTED. */

namespace {

struct HotkeyEntry {
    heliosview_hotkey_cb cb = nullptr;
    void* userdata = nullptr;
};

std::mutex g_hotkey_mutex;
std::map<uint32_t, HotkeyEntry> g_hotkey_table;
std::atomic<uint32_t> g_hotkey_next_id{1};

/* Host-window message handler: WM_HOTKEY arrives here against the host window we
 * registered the hotkeys on. Runs on the message-loop thread. */
bool hv_hotkey_handle_host_message(HWND, UINT message, WPARAM wparam, LPARAM)
{
    if (message != WM_HOTKEY)
        return false;
    const uint32_t id = static_cast<uint32_t>(wparam); /* = the RegisterHotKey id */
    HotkeyEntry e;
    {
        std::lock_guard<std::mutex> lock(g_hotkey_mutex);
        const auto it = g_hotkey_table.find(id);
        if (it == g_hotkey_table.end())
            return false;
        e = it->second; /* copy so the callback may register/unregister freely */
    }
    e.cb(id, e.userdata);
    return true;
}

} // namespace

uint32_t heliosview_hotkey_register(const char* shortcut, heliosview_hotkey_cb cb, void* userdata)
{
    if (!shortcut || !*shortcut) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "shortcut is NULL or empty");
        return 0;
    }
    if (!cb) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "cb is NULL");
        return 0;
    }
    const hv_shortcut sc = hv_parse_shortcut(shortcut);
    if (sc.vk == 0) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "unrecognized shortcut key");
        return 0;
    }

    const HWND host = hv_host_window(&hv_hotkey_handle_host_message);
    if (!host) {
        hv_fail_win32(GetLastError(), "failed to create the hotkey host window");
        return 0;
    }

    const uint32_t id = g_hotkey_next_id.fetch_add(1);
    if (id == 0 || id >= 0xBFFF) { /* RegisterHotKey ids must stay in 0x0000..0xBFFF */
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "hotkey id space exhausted");
        return 0;
    }
    /* MOD_NOREPEAT: fire once per physical press rather than on key auto-repeat. */
    const DWORD mods = static_cast<DWORD>(sc.mods) | MOD_NOREPEAT;
    if (!RegisterHotKey(host, static_cast<int>(id), mods, sc.vk)) {
        hv_fail_win32(GetLastError(), "RegisterHotKey failed (combination already in use?)");
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_hotkey_mutex);
        g_hotkey_table.emplace(id, HotkeyEntry{cb, userdata});
    }
    return id;
}

void heliosview_hotkey_unregister(uint32_t hotkey_id)
{
    if (hotkey_id == 0)
        return;
    const HWND host = hv_host_window(); /* the host already exists; no handler to add */
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_hotkey_mutex);
        found = g_hotkey_table.erase(hotkey_id) != 0;
    }
    if (found && host)
        UnregisterHotKey(host, static_cast<int>(hotkey_id));
}
