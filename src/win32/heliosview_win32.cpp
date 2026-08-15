#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"
// HeliosView.dll — Windows implementation: windows, message loop, native-message → event conversion.
// The cross-platform interface is in heliosview.h; this file implements only the win32 side.

/* Enable modern (visual-styled) common controls for the whole process, from the
 * library itself: HeliosView.dll embeds a Common-Controls v6 manifest resource
 * (see hv_resources.rc / hv_common_controls.manifest). The loader merges a
 * DLL's manifest dependencies into the hosting process's activation context, so
 * any app that loads HeliosView.dll gets the themed ComCtl32 v6 (instead of the
 * legacy v5 look) for message boxes and common controls - without needing its
 * own manifest. This is the library-side equivalent of the classic application
 * manifest, and it keeps the examples and consumers manifest-free. */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00 /* Windows 10: ActivateActivatedContext / CreateActivationContext */
#include <windows.h>
#include <commctrl.h>  /* InitCommonControlsEx (common controls v6 init) */
#include <shellapi.h> /* Shell_NotifyIcon / NOTIFYICONDATA (tray icon) */
#include <objbase.h>  /* CoCreateInstance */
#include <shobjidl.h> /* IFileOpenDialog / IShellItem (file pickers) / ITaskbarList3 */
#include <dwmapi.h>   /* DwmSetWindowAttribute (backdrop / dark mode) */

#include <wrl/client.h> /* ComPtr */
#include <WebView2.h>

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

/* flat_map backed by std::pmr::vector, so a WebView's binding/subscription tables
 * allocate through the default PMR resource (set in main, after these runtime
 * objects are created). The window registry keeps the plain std::vector flavor on
 * purpose: it stays independent of whatever PMR default the app sets. */
template <class K, class V>
using hv_pmr_flat_map = std::flat_map<K, V, std::less<K>,
                                      std::pmr::vector<K>, std::pmr::vector<V>>;

/* ================= Window (completes the header's opaque declaration; must be at global scope) ================= */

/* A WebView attached to a window; the full struct is defined below. The window
 * keeps the list of its attached WebViews (resize together with it, and WM_APP
 * task-drain dispatch), so no separate hwnd → webview registry is needed. */
struct heliosview_webview;

struct heliosview_window {
    int width = 0;
    int height = 0;
    std::string title; /* UTF-8 */
    heliosview_window_style_t style = HELIOSVIEW_WINDOW_NORMAL;
    HWND hwnd = nullptr;
    void* userdata = nullptr; /* caller data (the C++ wrapper stores an object pointer) */
    HICON icon = nullptr;     /* custom window icon (owned; NULL = default) */
    bool resizable = true;    /* whether the user can resize / maximize the window */
    int32_t min_w = 0, min_h = 0; /* minimum client size (0 = unconstrained) */
    int32_t max_w = 0, max_h = 0; /* maximum client size (0 = unconstrained) */
    bool fullscreen = false;       /* whether the window covers the whole monitor */
    RECT fs_restore_rect{};        /* pre-fullscreen window rect (restored on exit) */
    DWORD fs_restore_style = 0;    /* pre-fullscreen window style */
    DWORD fs_restore_exstyle = 0;  /* pre-fullscreen extended style */
    std::vector<RECT> drag_regions; /* client-area move regions (WM_NCHITTEST -> HTCAPTION) */

    /* Attached WebViews (UI thread). The window owns the registry: WebViews are
     * resized together with their window (WM_SIZE) and found for task-drain
     * dispatch (WM_HV_WEBVIEW_MSG); heliosview_webview_destroy detaches first, so
     * a queued message for a destroyed WebView is a no-op. The app destroys
     * WebViews before the window (the window does not own them). */
    std::vector<heliosview_webview_t*> webviews;

    /* Routing registry (type-erased): routing id -> userdata (the C++ object).
     * Tray icons (keyed by their WM_APP callback message id), menu items (keyed by
     * the item id) and caller-registered ids all share one id space allocated from
     * next_route_id (WM_APP range, so every id is a valid WM_APP message / fits a
     * WM_COMMAND LOWORD). default_native_convert reads it to route tray / menu
     * events back to the owning object. */
    uint32_t next_route_id = WM_APP + 0x100;       /* per-window routing id allocator */
    std::flat_map<uint32_t, void*> registry;        /* routing id -> userdata */
};

/* ================= Tray icon (notification area icon; Windows Shell_NotifyIcon) ================= */

struct heliosview_tray {
    HWND hwnd = nullptr;       /* owning window (receives the callback messages) */
    UINT callback_msg = 0;     /* WM_APP-based callback message id, unique per tray */
    UINT uid = 0;              /* tray icon id */
    HICON icon = nullptr;      /* current icon (owned; NULL = default) */
    std::string tooltip;       /* UTF-8 */
    bool added = false;        /* NIM_ADD succeeded (so NIM_DELETE is safe) */
    void* userdata = nullptr;  /* caller data (the C++ wrapper stores an object pointer) */
};

/* ================= Menu (popup / context menu) ================= */

struct heliosview_menu {
    heliosview_window_t* window = nullptr;   /* owner window (items registered here) */
    HMENU hmenu = nullptr;                   /* Win32 popup menu */
    void* userdata = nullptr;                /* caller data (the C++ wrapper stores an object pointer) */
    std::vector<heliosview_menu_t*> submenus; /* owned submenus (destroyed with parent) */
};

/* ================= WebView (independent handle; Win32: WebView2) ================= */

/* A native function registered for JS calls (window.helios.call) */
struct hv_webview_binding {
    heliosview_webview_bind_cb callback = nullptr;
    void* userdata = nullptr;
    heliosview_webview_userdata_dtor dtor = nullptr;
};

/* A native subscription to JS BroadcastChannel(name) posts */
struct hv_webview_subscription {
    heliosview_webview_subscribe_cb callback = nullptr;
    void* userdata = nullptr;
    heliosview_webview_userdata_dtor dtor = nullptr;
};

/* UTF-8 → UTF-16 */
std::wstring utf8_to_wide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct heliosview_webview {
    HWND parent = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    bool creating = false;
    bool ready = false;
    bool has_pending = false;   /* navigation queued before init completes (only the last one runs) */
    bool pending_html = false;  /* true = NavigateToString, false = Navigate */
    std::string pending_text;   /* UTF-8 */

    /* WebView placement within the parent client area (client pixels); set via
     * heliosview_webview_set_insets. The WebView fills the client area minus
     * these insets, so a strip (e.g. a title bar with the DWM caption buttons)
     * can stay visible around it. */
    bool has_insets = false;
    int32_t inset_top = 0, inset_right = 0, inset_bottom = 0, inset_left = 0;

    /* JS <-> native bridge */
    DWORD ui_thread = GetCurrentThreadId();            /* thread that created the webview */
    EventRegistrationToken message_token{};            /* JS -> native messages */
    hv_pmr_flat_map<std::string, hv_webview_binding> bindings; /* name -> binding (UI thread only) */
    hv_pmr_flat_map<std::string, hv_webview_subscription> subscriptions; /* BroadcastChannel name -> subscription (UI thread only) */

    /* navigation-completed callback (UI thread only) */
    heliosview_webview_navigation_cb nav_cb = nullptr;
    void* nav_userdata = nullptr;
    heliosview_webview_userdata_dtor nav_dtor = nullptr;
    EventRegistrationToken nav_token{};                /* NavigationCompleted handler */

    /* navigation-starting callback (may veto); UI thread only */
    heliosview_webview_navigation_starting_cb nav_start_cb = nullptr;
    void* nav_start_userdata = nullptr;
    heliosview_webview_userdata_dtor nav_start_dtor = nullptr;
    EventRegistrationToken nav_start_token{};          /* NavigationStarting handler */

    /* source-changed (URL-changed) callback; UI thread only */
    heliosview_webview_source_changed_cb source_cb = nullptr;
    void* source_userdata = nullptr;
    heliosview_webview_userdata_dtor source_dtor = nullptr;
    EventRegistrationToken source_token{};             /* SourceChanged handler */

    /* document-title-changed callback; UI thread only */
    heliosview_webview_title_changed_cb title_cb = nullptr;
    void* title_userdata = nullptr;
    heliosview_webview_userdata_dtor title_dtor = nullptr;
    EventRegistrationToken title_token{};              /* DocumentTitleChanged handler (ICoreWebView2_2) */

    /* eval / eval_async queued while the WebView was still initializing */
    struct pending_op {
        std::string script;
        bool async = false;
        heliosview_webview_eval_cb callback = nullptr;
        void* userdata = nullptr;
    };
    std::deque<pending_op> pending_ops;

    std::deque<std::function<void()>> ui_tasks;         /* marshalled tasks (drained on the UI thread) */
    std::mutex ui_mutex;                               /* guards ui_tasks (cross-thread marshalling) */
};

/* The WebView's bounds within the parent client area: the full client rect
 * shrunk by the registered insets. Used wherever the WebView is placed (at
 * creation and on every parent resize), so a native title-bar strip carrying
 * the DWM caption buttons can stay visible above the WebView. */
RECT hv_webview_rect(const heliosview_webview_t* webview)
{
    RECT rc{};
    if (webview->parent)
        GetClientRect(webview->parent, &rc);
    if (webview->has_insets) {
        rc.left += webview->inset_left;
        rc.top += webview->inset_top;
        rc.right -= webview->inset_right;
        rc.bottom -= webview->inset_bottom;
        if (rc.right < rc.left)
            rc.right = rc.left;
        if (rc.bottom < rc.top)
            rc.bottom = rc.top;
    }
    return rc;
}

namespace {

inline const UINT WM_HV_WEBVIEW_MSG = WM_APP + 0x40; /* posted to the parent HWND to drain ui_tasks */

/* UTF-16 -> UTF-8 */
std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/* Map a negated Win32 error code to a human-readable message */
std::string hv_strerror(int code)
{
    if (code == 0)
        return "unknown error";
    wchar_t* buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageW(flags, nullptr, static_cast<DWORD>(-code), 0,
                                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg = "error " + std::to_string(code);
    if (n && buf) {
        std::wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' '))
            w.pop_back();
        msg = wide_to_utf8(w);
    }
    if (buf)
        LocalFree(buf);
    return msg;
}

/* Bridge envelope wire format:
 *
 *   HV\t<kind>\t<fields>\r\n\r\n<payload>
 *
 * The envelope header is a small, tab-separated field list (magic "HV",
 * a kind, then the kind's fields). Registered names are validated as C
 * identifiers ([A-Za-z_][A-Za-z0-9_]*), so a tab or CR/LF can never appear in a
 * header field. The payload (`args`/`result`/`data`/`error`) is an arbitrary
 * byte string fenced by the HTTP-style "\r\n\r\n" separator and passed through
 * verbatim; for calls/results it is JSON text, but the C side treats it as
 * opaque and never parses it. Sharing this comment between the C side and the
 * JS shim (kWebView2BridgeScript) keeps the two ends in sync.
 *
 *   up   call:      HV\tcall\t<id>\t<name>\r\n\r\n<argsJson>
 *   up   broadcast: HV\tbroadcast\t<name>\r\n\r\n<dataJson>
 *   down resolve:   HV\tresolve\t<id>\r\n\r\n<resultJson>
 *   down reject:    HV\treject\t<id>\r\n\r\n<errorJson>
 *
 * A registered name must follow the C identifier rules (matching how native
 * functions are named), which keeps the header free of tab / CR / LF.
 */
bool hv_valid_name(const char* s)
{
    if (!s || !s[0])
        return false;
    const char c0 = s[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
        return false;
    for (const char* p = s + 1; *p; ++p) {
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '_'))
            return false;
    }
    return true;
}

/* Names beginning with "__hv." are the library's internal bridge methods (the
 * injected <helios-window-controls> / <helios-window-title-bar> components call
 * __hv.control / __hv.state / __hv.drag). The dot makes them invalid C
 * identifiers, so applications cannot bind or subscribe them through the public
 * API (which requires valid identifiers) — only the library's internal
 * whitelist (hv_bind_builtin) registers them. The wire parser still accepts
 * them (the header stays separator-safe: dots carry no tab / CR / LF). */
bool hv_internal_name(const char* s)
{
    static constexpr char kPrefix[] = "__hv.";
    const size_t n = sizeof(kPrefix) - 1;
    return s && std::strncmp(s, kPrefix, n) == 0;
}

/* Post a raw envelope string to the JS page (must run on the UI thread). */
void hv_post_string(heliosview_webview_t* wv, const std::string& s)
{
    if (wv && wv->webview && wv->ready) {
        const std::wstring w = utf8_to_wide(s);
        wv->webview->PostWebMessageAsString(w.c_str());
    }
}

/* Marshal fn to the webview's UI thread: runs immediately if already there,
 * otherwise queued behind the window's posted message. Safe from any thread. */
void hv_ui(heliosview_webview_t* wv, std::function<void()> fn)
{
    if (!wv)
        return;
    if (GetCurrentThreadId() == wv->ui_thread) {
        fn();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wv->ui_mutex);
        wv->ui_tasks.push_back(std::move(fn));
    }
    PostMessageW(wv->parent, WM_HV_WEBVIEW_MSG, reinterpret_cast<WPARAM>(wv), 0);
}

/* Drain tasks marshalled to this webview's UI thread (called from the WndProc) */
void hv_drain_ui_tasks(heliosview_webview_t* wv)
{
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(wv->ui_mutex);
        tasks.swap(wv->ui_tasks);
    }
    for (auto& fn : tasks)
        if (fn)
            fn();
}

/* The JS bridge shim, injected into every document (AddScriptToExecuteOnDocumentCreated
 * takes raw script text). window.helios.call invokes native functions; BroadcastChannel
 * is subclassed so native broadcasts dispatch synthetic message events and page
 * postMessage()s are forwarded to native subscriptions; <helios-window-controls> is the
 * built-in title-bar button web component. Embedded from webview_bridge.js with C++23
 * #embed, so the shim lives in a real .js file (editable/versionable/lintable on its
 * own) instead of a raw string literal. */
#include "webview_bridge.inc" /* generated from win32/webview_bridge.js (see src/CMakeLists.txt): kWebView2BridgeScript */

} // namespace

/* ================= WebView2 callbacks (hand-rolled COM: the new SDK's WRL no longer has the Callback helper) ================= */

/* Minimal COM callback base: the IUnknown trio + refcount (created with hv::hv_alloc; Release to zero self-deletes) */
template <typename Interface>
struct com_callback_base : public Interface {
    virtual ~com_callback_base() = default; /* Release does hv::hv_dealloc(this) through the virtual dtor */
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() noexcept override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() noexcept override
    {
        const ULONG refs = --m_refs;
        if (refs == 0)
            hv::hv_dealloc(this);
        return refs;
    }
    std::atomic<ULONG> m_refs{1};
};

struct env_completed_handler : com_callback_base<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    using Fn = std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>;
    explicit env_completed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment* env) noexcept override
    {
        return m_fn(result, env);
    }
    Fn m_fn;
};

struct controller_completed_handler : com_callback_base<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    using Fn = std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>;
    explicit controller_completed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Controller* controller) noexcept override
    {
        return m_fn(result, controller);
    }
    Fn m_fn;
};

/* JS -> native messages (window.chrome.webview.postMessage) */
struct web_message_received_handler : com_callback_base<ICoreWebView2WebMessageReceivedEventHandler> {
    using Fn = std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>;
    explicit web_message_received_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) noexcept override
    {
        return m_fn(sender, args);
    }
    Fn m_fn;
};

/* Navigation completed: re-inject the shim after every page load */
struct navigation_completed_handler : com_callback_base<ICoreWebView2NavigationCompletedEventHandler> {
    using Fn = std::function<HRESULT(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)>;
    explicit navigation_completed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) noexcept override
    {
        return m_fn(sender, args);
    }
    Fn m_fn;
};

/* Navigation starting: fires before a navigation begins; may cancel it (return
 * nonzero from the registered callback) */
struct navigation_starting_handler : com_callback_base<ICoreWebView2NavigationStartingEventHandler> {
    using Fn = std::function<HRESULT(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*)>;
    explicit navigation_starting_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) noexcept override
    {
        return m_fn(sender, args);
    }
    Fn m_fn;
};

/* Source changed (webview URL changed) */
struct source_changed_handler : com_callback_base<ICoreWebView2SourceChangedEventHandler> {
    using Fn = std::function<HRESULT(ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*)>;
    explicit source_changed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) noexcept override
    {
        return m_fn(sender, args);
    }
    Fn m_fn;
};

/* Document title changed (requires ICoreWebView2_2). The event args are passed as
 * IUnknown* with no typed args interface; the title is read from the sender
 * (ICoreWebView2::get_DocumentTitle) when the event fires. */
struct title_changed_handler : com_callback_base<ICoreWebView2DocumentTitleChangedEventHandler> {
    using Fn = std::function<HRESULT(ICoreWebView2*)>;
    explicit title_changed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(ICoreWebView2* sender, IUnknown* args) noexcept override
    {
        (void)args;
        return m_fn(sender);
    }
    Fn m_fn;
};

/* AddScriptToExecuteOnDocumentCreated completed: carry the script id (ignored) */
struct add_script_completed_handler : com_callback_base<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> {
    using Fn = std::function<HRESULT(HRESULT, LPCWSTR)>;
    explicit add_script_completed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(HRESULT errorCode, LPCWSTR result) noexcept override
    {
        return m_fn(errorCode, result);
    }
    Fn m_fn;
};

/* ExecuteScript completed: deliver the JSON result */
struct execute_script_completed_handler : com_callback_base<ICoreWebView2ExecuteScriptCompletedHandler> {
    using Fn = std::function<HRESULT(HRESULT, LPCWSTR)>;
    explicit execute_script_completed_handler(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(HRESULT errorCode, LPCWSTR result) noexcept override
    {
        return m_fn(errorCode, result);
    }
    Fn m_fn;
};

/* Preset style → Win32 window style (honors the window's resizable flag) */
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
 * design — the same model as the cross-platform event queue (hv::g_queue in
 * heliosview_internal.h): windows are created, looked up and destroyed on the
 * thread that runs the message loop, so the live-window count is thread-local
 * state, not a process-global variable. There is no id → window registry at
 * all: a window's identity is its native handle (HWND), which events carry and
 * heliosview_window_from_id resolves via IsWindow + GWLP_USERDATA — nothing to
 * keep in sync with destroy, and a stale queued event resolves to NULL. */
struct hv_ui_state {
    int32_t window_count = 0; /* live windows on this thread (create/destroy) */
};
thread_local hv_ui_state g_ui_state;

/* Session-end (WM_QUERYENDSESSION) callback: runs synchronously on the message-loop
 * thread before shutdown/logoff; return non-zero to veto. */
heliosview_session_end_cb g_session_end_cb = nullptr;
void* g_session_end_userdata = nullptr;

/* Routing ids (tray callback messages, menu item ids, caller ids) are allocated
 * from each window's own id space (window->next_route_id); the tray uid is the
 * only remaining process-wide counter (unique per process for Shell_NotifyIcon). */
std::atomic<UINT> g_next_tray_uid{1};

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

int default_native_convert(void* native_msg, heliosview_event_t* out)
{
    const MSG* msg = static_cast<const MSG*>(native_msg);
    const int64_t ts = hv::now_ms();

    /* The owning window: tray / menu routing ids live on it (the registry). */
    auto* win = reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(msg->hwnd, GWLP_USERDATA));

    /* Tray icon callback messages (posted by Shell_NotifyIcon when an icon is
     * clicked). The message id IS the per-window WM_APP callback id (>= WM_APP),
     * which also keys the registry entry. */
    if (win && msg->message >= WM_APP) {
        if (auto it = win->registry.find(msg->message); it != win->registry.end()) {
            switch (msg->lParam) {
            case WM_LBUTTONUP:       out->type = HELIOSVIEW_EVENT_TRAY_LEFT_CLICK; break;
            case WM_LBUTTONDBLCLK:   out->type = HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK; break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:     out->type = HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK; break;
            case WM_MBUTTONUP:       out->type = HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK; break;
            default:
                return 0; /* consume; not a click we translate */
            }
            out->userdata = it->second;
            out->timestamp_ms = ts;
            return 1;
        }
    }

    switch (msg->message) {
    case WM_HV_WEBVIEW_MSG:
        /* Marshalled webview tasks (cross-thread resolve/reject/broadcast). The
         * message carries the posting WebView in wParam; it is drained only if
         * that WebView is still attached to the window — destroy detaches it
         * from the window's list first, so a queued message for a destroyed
         * WebView is a no-op (never touches a freed object). */
        if (win) {
            const auto* wv = reinterpret_cast<heliosview_webview_t*>(msg->wParam);
            for (auto* attached : win->webviews)
                if (attached == wv) {
                    hv_drain_ui_tasks(attached);
                    break;
                }
        }
        return 0; /* consumed; not a user-facing event */
    case WM_COMMAND: {
        /* Menu item selection: TrackPopupMenu posts WM_COMMAND with the item's id
         * in LOWORD(wParam). Only route ids that belong to a tracked popup menu on
         * this window; WM_COMMAND also goes to regular child controls/buttons. */
        const UINT id = LOWORD(msg->wParam);
        void* ud = nullptr;
        if (win) {
            auto it = win->registry.find(id);
            if (it != win->registry.end())
                ud = it->second;
        }
        if (!ud)
            return -1; /* not one of ours → hand off to DefWindowProc */
        out->type = HELIOSVIEW_EVENT_MENU_SELECT;
        out->menu_item = id;
        out->userdata = ud;
        out->timestamp_ms = ts;
        return 1;
    }
    case WM_CLOSE:
        out->type = HELIOSVIEW_EVENT_WINDOW_CLOSE;
        out->timestamp_ms = ts;
        return 1;
    case WM_SIZE: {
        /* WebViews attached to this window resize along with it (honoring any
         * registered insets, so e.g. a title-bar strip stays uncovered) */
        if (win)
            for (auto* wv : win->webviews)
                if (auto* controller = wv->controller.Get())
                    controller->put_Bounds(hv_webview_rect(wv));
        out->type = HELIOSVIEW_EVENT_WINDOW_RESIZE;
        out->width = static_cast<int32_t>(LOWORD(msg->lParam));
        out->height = static_cast<int32_t>(HIWORD(msg->lParam));
        out->timestamp_ms = ts;
        return 1;
    }
    case WM_ACTIVATE: {
        /* WA_INACTIVE (0) = lost focus, everything else = gained focus */
        out->type = LOWORD(msg->wParam) == WA_INACTIVE
                        ? HELIOSVIEW_EVENT_WINDOW_BLUR
                        : HELIOSVIEW_EVENT_WINDOW_FOCUS;
        out->timestamp_ms = ts;
        return 1;
    }
    case WM_MOVE:
        /* final position (screen coords of the top-left corner) */
        out->type = HELIOSVIEW_EVENT_WINDOW_MOVED;
        out->x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(msg->lParam)));
        out->y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(msg->lParam)));
        out->timestamp_ms = ts;
        return 1;
    case WM_MOVING: {
        /* drag in progress: lParam points at the current window rect */
        const RECT* rc = reinterpret_cast<const RECT*>(msg->lParam);
        if (rc) {
            out->type = HELIOSVIEW_EVENT_WINDOW_MOVING;
            out->x = rc->left;
            out->y = rc->top;
            out->timestamp_ms = ts;
            return 1;
        }
        return 0;
    }
    case WM_SIZING: {
        /* resize drag in progress: lParam points at the proposed window rect */
        const RECT* rc = reinterpret_cast<const RECT*>(msg->lParam);
        if (rc) {
            out->type = HELIOSVIEW_EVENT_WINDOW_SIZING;
            out->width = rc->right - rc->left;
            out->height = rc->bottom - rc->top;
            out->timestamp_ms = ts;
            return 1;
        }
        return 0;
    }
    case WM_ENABLE:
        /* wParam: TRUE = being enabled, FALSE = being disabled */
        out->type = msg->wParam ? HELIOSVIEW_EVENT_WINDOW_ENABLED
                                : HELIOSVIEW_EVENT_WINDOW_DISABLED;
        out->timestamp_ms = ts;
        return 1;
    case WM_KEYDOWN:
        if ((msg->lParam & 0x40000000) != 0)
            return 0; /* filter keyboard auto-repeat */
        out->type = HELIOSVIEW_EVENT_KEY_DOWN;
        out->key = map_vk(static_cast<UINT>(msg->wParam));
        out->timestamp_ms = ts;
        return 1;
    case WM_KEYUP:
        out->type = HELIOSVIEW_EVENT_KEY_UP;
        out->key = map_vk(static_cast<UINT>(msg->wParam));
        out->timestamp_ms = ts;
        return 1;
    case WM_MOUSEMOVE:
        out->type = HELIOSVIEW_EVENT_MOUSE_MOVE;
        break;
    case WM_LBUTTONDOWN: out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; out->mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONDOWN: out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; out->mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONDOWN: out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; out->mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    case WM_LBUTTONUP:   out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   out->mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONUP:   out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   out->mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONUP:   out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   out->mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    default:
        return -1; /* unhandled → hand off to DefWindowProc */
    }

    out->x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(msg->lParam)));
    out->y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(msg->lParam)));
    out->timestamp_ms = ts;
    return 1;
}

/* ================= Title-bar strip metric =================
 *
 * Frameless windows draw all their chrome in the page (optionally with the
 * injected <helios-window-controls> web component for the buttons). This is the
 * 96-DPI height of the drag strip, scaled with the DPI. (Resizing needs no
 * metric: FRAMELESS keeps the system WS_THICKFRAME border, which the system
 * hit-tests natively.) */

/* Height of the title-bar strip the drag area occupies, in 96-DPI pixels. */
constexpr int kTitleBarHeight = 48;

int hv_title_bar_height(HWND hwnd)
{
    return MulDiv(kTitleBarHeight, GetDpiForWindow(hwnd), 96);
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

    /* The library's built-in conversion always runs first (tray/menu/webview
     * side effects, window/keyboard/mouse). If it does not handle the message
     * (-1), try each registered converter in order; the first that returns 1
     * (queued) or 0 (consumed) wins. Otherwise fall through to DefWindowProc. */
    heliosview_event_t event{};
    int handled = default_native_convert(&native, &event);
    if (handled == -1) {
        for (const auto& [id, h] : hv::g_native_handlers) {
            (void)id;
            if (!h)
                continue;
            handled = h(&native, &event);
            if (handled != -1)
                break;
        }
    }

    if (handled == 1) {
        /* The native handle IS the window id: events carry it, and the consumer
         * looks the window back up via heliosview_window_from_id (IsWindow +
         * GWLP_USERDATA), so no id registry is involved. */
        event.window_id = reinterpret_cast<uintptr_t>(hwnd);
        hv::queue_push(event);
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
    if (!title)
        return nullptr;
    auto* window = hv::hv_alloc<heliosview_window>();
    window->width = width;
    window->height = height;
    window->title = title;
    window->style = style;
    window->userdata = userdata;
    g_ui_state.window_count++;
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
     * verify the window procedure is one of ours before trusting it. */
    const auto proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (proc != &heliosview_wndproc_t<HELIOSVIEW_WINDOW_NORMAL>
        && proc != &heliosview_wndproc_t<HELIOSVIEW_WINDOW_BORDERLESS>
        && proc != &heliosview_wndproc_t<HELIOSVIEW_WINDOW_FRAMELESS>)
        return nullptr;
    return reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int heliosview_window_count(void)
{
    return static_cast<int>(g_ui_state.window_count);
}

void heliosview_window_destroy(heliosview_window_t* window)
{
    if (!window)
        return;
    g_ui_state.window_count--;
    if (window->hwnd) {
        /* The window is a passive registry only: it does not own trays/menus.
         * Their C++ wrappers (Tray/Menu) must be destroyed before the window. */
        SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(window->hwnd); /* triggers WM_DESTROY → PostQuitMessage → message loop exits */
    }
    if (window->icon)
        DestroyIcon(window->icon);
    hv::hv_dealloc(window);
}

int heliosview_window_show(heliosview_window_t* window)
{
    if (!window)
        return -1;
    hv_ensure_common_controls_ctx(); /* v6 theming for this window's controls */
    if (window->hwnd) {
        ShowWindow(window->hwnd, SW_SHOW);
        return 0;
    }

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

    const std::wstring title = utf8_to_wide(window->title);

    /* compute the window size for the preset style: NORMAL uses AdjustWindowRect
     * (client + caption + frame); BORDERLESS and FRAMELESS are entirely client
     * area (WM_NCCALCSIZE), so the requested client size is the window size. */
    const DWORD style = map_win32_style(window);
    RECT rect{0, 0, window->width, window->height};
    if (window->style == HELIOSVIEW_WINDOW_NORMAL)
        AdjustWindowRect(&rect, style, FALSE);

    window->hwnd = CreateWindowExW(0, class_name, title.c_str(), style,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   rect.right - rect.left, rect.bottom - rect.top,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), window);
    if (!window->hwnd)
        return -2;

    /* already registered in WM_CREATE; fall back here if that did not happen */
    if (!GetWindowLongPtrW(window->hwnd, GWLP_USERDATA))
        SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    ShowWindow(window->hwnd, SW_SHOW);
    return 0;
}

/* The window's native handle (HWND) — the window_id its events carry. 0 until
 * the window is shown (the native window is created on show()). */
uintptr_t heliosview_window_id(const heliosview_window_t* window)
{
    return window ? reinterpret_cast<uintptr_t>(window->hwnd) : 0;
}

uint32_t heliosview_window_add_item(heliosview_window_t* window, void* userdata)
{
    if (!window)
        return 0;
    const uint32_t id = window->next_route_id++;
    window->registry[id] = userdata;
    return id;
}

int heliosview_window_remove_item(heliosview_window_t* window, uint32_t id)
{
    if (!window)
        return -1;
    return window->registry.erase(id) ? 0 : -1;
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
    return SUCCEEDED(hr) ? 0 : -static_cast<int>(hr);
}

int heliosview_window_set_dark_mode(heliosview_window_t* window, int on)
{
    if (!window || !window->hwnd)
        return -1;
    const BOOL enable = on != 0;
    const HRESULT hr = DwmSetWindowAttribute(window->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                             &enable, sizeof(enable));
    return SUCCEEDED(hr) ? 0 : -static_cast<int>(hr);
}

/* ================= Tray icon (Shell_NotifyIcon) ================= */

namespace {

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
    nid.uCallbackMessage = tray->callback_msg;
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
    if (!window || !window->hwnd) /* the window must exist to receive callback messages */
        return nullptr;

    auto* tray = hv::hv_alloc<heliosview_tray>();
    tray->hwnd = window->hwnd;
    tray->callback_msg = heliosview_window_add_item(window, userdata); /* register routing id */
    tray->uid = g_next_tray_uid.fetch_add(1);
    tray->tooltip = tooltip ? tooltip : "";
    tray->icon = load_tray_icon(icon_path);
    tray->userdata = userdata;

    if (tray_apply(tray, NIM_ADD) != 0) {
        heliosview_window_remove_item(window, tray->callback_msg);
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
        return -1;
    tray->tooltip = tooltip ? tooltip : "";
    return tray->added ? tray_apply(tray, NIM_MODIFY) : 0;
}

int heliosview_tray_set_icon(heliosview_tray_t* tray, const char* icon_path)
{
    if (!tray)
        return -1;
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
    if (tray->hwnd) {
        if (auto* win = reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(tray->hwnd, GWLP_USERDATA)))
            heliosview_window_remove_item(win, tray->callback_msg);
    }
    if (tray->icon)
        DestroyIcon(tray->icon);
    hv::hv_dealloc(tray);
}

/* ================= Tray balloon notification (NIF_INFO) ================= */

int heliosview_tray_notify(heliosview_tray_t* tray, const char* title, const char* message,
                           heliosview_tray_notify_icon_t icon_type, uint32_t timeout_ms)
{
    if (!tray || !tray->added)
        return -1;
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
    return Shell_NotifyIconW(NIM_MODIFY, &nid) ? 0 : -1;
}

/* ================= Menu (CreatePopupMenu / TrackPopupMenu) ================= */

heliosview_menu_t* heliosview_menu_create(heliosview_window_t* window, void* userdata)
{
    auto* menu = hv::hv_alloc<heliosview_menu>();
    menu->window = window;
    menu->userdata = userdata;
    menu->hmenu = CreatePopupMenu();
    if (!menu->hmenu) {
        hv::hv_dealloc(menu);
        return nullptr;
    }
    return menu;
}

void heliosview_menu_destroy(heliosview_menu_t* menu)
{
    if (!menu)
        return;
    /* drop this menu's item ids (keyed by this menu's userdata) from the owner
     * window's routing registry */
    if (menu->window) {
        auto& reg = menu->window->registry;
        for (auto it = reg.begin(); it != reg.end();)
            it = it->second == menu->userdata ? reg.erase(it) : ++it;
    }
    /* destroy owned submenus (each recurses and removes its own item ids) */
    while (!menu->submenus.empty()) {
        heliosview_menu_t* sub = menu->submenus.back();
        menu->submenus.pop_back();
        heliosview_menu_destroy(sub);
    }
    if (menu->hmenu)
        DestroyMenu(menu->hmenu);
    hv::hv_dealloc(menu);
}

int heliosview_menu_add_item(heliosview_menu_t* menu, const char* text, uint32_t* out_id)
{
    if (!menu || !menu->hmenu || !menu->window)
        return -1;
    const UINT id = heliosview_window_add_item(menu->window, menu->userdata);
    if (id == 0)
        return -1;
    const std::wstring wtext = utf8_to_wide(text ? text : "");
    if (!AppendMenuW(menu->hmenu, MF_STRING, id, wtext.c_str())) {
        heliosview_window_remove_item(menu->window, id);
        return -1;
    }
    if (out_id)
        *out_id = id;
    return 0;
}

int heliosview_menu_add_separator(heliosview_menu_t* menu)
{
    if (!menu || !menu->hmenu)
        return -1;
    return AppendMenuW(menu->hmenu, MF_SEPARATOR, 0, nullptr) ? 0 : -1;
}

int heliosview_menu_add_submenu(heliosview_menu_t* menu, const char* text,
                                heliosview_menu_t* submenu)
{
    if (!menu || !menu->hmenu || !submenu || !submenu->hmenu)
        return -1;
    const std::wstring wtext = utf8_to_wide(text ? text : "");
    if (!AppendMenuW(menu->hmenu, MF_POPUP,
                     reinterpret_cast<UINT_PTR>(submenu->hmenu), wtext.c_str()))
        return -1;
    menu->submenus.push_back(submenu); /* parent owns the submenu's lifetime */
    return 0;
}

int heliosview_menu_show(heliosview_menu_t* menu, heliosview_window_t* window)
{
    if (!menu || !menu->hmenu || !window || !window->hwnd)
        return -1;
    POINT pt{};
    GetCursorPos(&pt);
    /* give the menu a foreground window so it is dismissed when the user clicks
     * elsewhere; item selection is delivered via WM_COMMAND to window->hwnd */
    SetForegroundWindow(window->hwnd);
    TrackPopupMenu(menu->hmenu,
                   TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, window->hwnd, nullptr);
    return 0;
}

/* ================= WebView (WebView2) ================= */

/* UI-thread task draining for cross-thread resolve/reject/broadcast.
 * WM_HV_WEBVIEW_MSG is posted to the parent HWND; the WndProc drains it. */

namespace {

/* ---- JS <-> native envelope parsing ----
 *
 * The JS shim (kWebView2BridgeScript) sends and receives messages in a compact,
 * JSON-free framing (see the wire-format comment near hv_valid_name above):
 * a tab-separated header plus a "\r\n\r\n"-fenced payload passed through
 * verbatim. Registered names are validated as C identifiers, so the header can
 * never contain a tab or CR/LF; the payload may contain anything. Parsing is
 * therefore just a header split + an id/name lookup — no JSON at all. */

/* Split the leading tab-separated header from the payload. On success returns
 * the byte offset just after the "\r\n\r\n" separator; msg.begin()+off is the
 * payload. Returns std::string::npos if there is no separator or the header is
 * malformed (must start with "HV"). */
size_t split_envelope(const std::string& msg, std::vector<std::string>& head, std::string& payload)
{
    const size_t sep = msg.find("\r\n\r\n");
    if (sep == std::string::npos)
        return std::string::npos;
    /* header: "HV\t<kind>\t<...fields>" */
    const std::string h = msg.substr(0, sep);
    if (h.compare(0, 2, "HV") != 0)
        return std::string::npos;
    size_t start = 0;
    head.clear();
    for (size_t pos = 0; pos <= h.size(); ++pos) {
        if (pos == h.size() || h[pos] == '\t') {
            head.push_back(h.substr(start, pos - start));
            start = pos + 1;
        }
    }
    payload = msg.substr(sep + 4);
    return sep + 4;
}

/* Parse the "HV\tcall\t<id>\t<name>\r\n\r\n<args>" message. Returns true for a
 * call, filling id / name / args (args is the raw payload text, or "[]" when the
 * JS side sent none). */
bool parse_call_envelope(const std::string& msg, uint64_t& id, std::string& name, std::string& args)
{
    id = 0;
    name.clear();
    args = "[]";
    std::vector<std::string> head;
    std::string payload;
    const size_t sep = split_envelope(msg, head, payload);
    if (sep == std::string::npos)
        return false;
    if (head.size() != 4 || head[1] != "call")
        return false;

    /* id */
    uint64_t v = 0;
    for (char c : head[2]) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (uint64_t)(c - '0');
    }
    id = v;

    /* name must be a valid C identifier (keeps the header separator-safe), or
     * one of the library's internal __hv.* names (hv_internal_name) — both are
     * safe in the tab-separated header. */
    if (!hv_valid_name(head[3].c_str()) && !hv_internal_name(head[3].c_str()))
        return false;
    name = head[3];

    if (!payload.empty())
        args = payload;
    return true;
}

/* Parse the "HV\tbroadcast\t<name>\r\n\r\n<data>" message. Returns true for a
 * broadcast, filling name and the raw payload text `data` ("" when absent). */
bool parse_broadcast_envelope(const std::string& msg, std::string& name, std::string& data)
{
    name.clear();
    data.clear();
    std::vector<std::string> head;
    std::string payload;
    const size_t sep = split_envelope(msg, head, payload);
    if (sep == std::string::npos)
        return false;
    if (head.size() != 3 || head[1] != "broadcast")
        return false;
    if (!hv_valid_name(head[2].c_str()))
        return false;
    name = head[2];
    data = payload;
    return true;
}

/* Dispatch a JS call to the bound native function. Runs on the UI thread. */
void hv_dispatch_call(heliosview_webview_t* wv, uint64_t id, const char* name,
                      const char* args_json)
{
    if (!wv || !name)
        return;
    auto it = wv->bindings.find(name);
    if (it == wv->bindings.end()) {
        /* unknown method: reject so the JS promise does not hang */
        heliosview_webview_reject(wv, id, "{\"error\":\"unknown method\"}");
        return;
    }
    hv_webview_binding& b = it->second;
    if (b.callback)
        b.callback(wv, id, name, args_json ? args_json : "", b.userdata);
}

/* Dispatch a JS BroadcastChannel.postMessage to the native subscription for that
 * name. Runs on the UI thread; no subscription is a silent no-op. */
void hv_dispatch_broadcast(heliosview_webview_t* wv, const std::string& name, const std::string& data)
{
    if (!wv || name.empty())
        return;
    auto it = wv->subscriptions.find(name);
    if (it == wv->subscriptions.end())
        return;
    hv_webview_subscription& s = it->second;
    if (s.callback)
        s.callback(wv, name.c_str(), data.c_str(), s.userdata);
}

/* ---------- Built-in window-control bridge ----------
 * Backs the injected <helios-window-controls> web component (see
 * kWebView2BridgeScript). Registered on every WebView under the reserved
 * __hv_* names; an app binding the same name replaces the built-in. */

/* First JSON string literal of a JSON array (e.g. ["minimize"]) — a lightweight
 * parse for the control actions the component sends. */
std::string hv_first_json_string(const char* args_json)
{
    if (!args_json)
        return {};
    const char* q = strchr(args_json, '"');
    if (!q)
        return {};
    const char* end = strchr(q + 1, '"');
    if (!end)
        return {};
    return std::string(q + 1, end);
}

/* The window a webview is attached to (for the built-in control actions). */
heliosview_window_t* hv_webview_owner(heliosview_webview_t* wv)
{
    if (!wv || !wv->parent)
        return nullptr;
    return reinterpret_cast<heliosview_window_t*>(GetWindowLongPtrW(wv->parent, GWLP_USERDATA));
}

/* "__hv.control"("minimize"|"maximize"|"restore"|"close") — perform the caption
 * action on the owner window (maximize auto-toggles like the real button). */
void hv_control_bind_cb(heliosview_webview_t* wv, uint64_t call_id, const char* name,
                        const char* args_json, void* userdata)
{
    (void)name;
    (void)userdata;
    auto* win = hv_webview_owner(wv);
    const std::string action = hv_first_json_string(args_json);
    if (!win || !win->hwnd) {
        heliosview_webview_reject(wv, call_id, R"({"error":"no window"})");
        return;
    }
    if (action == "minimize")
        ShowWindow(win->hwnd, SW_MINIMIZE);
    else if (action == "maximize") {
        /* Maximize is disabled while the window is not resizable (the maximize
         * box was removed); restoring an already-maximized window stays allowed. */
        if (!win->resizable && !IsZoomed(win->hwnd)) {
            heliosview_webview_reject(wv, call_id, R"({"error":"maximize disabled"})");
            return;
        }
        ShowWindow(win->hwnd, IsZoomed(win->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (action == "restore")
        ShowWindow(win->hwnd, SW_RESTORE);
    else if (action == "close")
        PostMessageW(win->hwnd, WM_CLOSE, 0, 0);
    else {
        heliosview_webview_reject(wv, call_id, R"({"error":"unknown control action"})");
        return;
    }
    heliosview_webview_resolve(wv, call_id, R"({"ok":true})");
}

/* "__hv.state"() — the owner's show state (the component toggles the maximize /
 * restore glyph from it), whether the window can be maximized (maximizable;
 * the component disables the maximize button when false), plus the title-bar
 * strip height. */
void hv_state_bind_cb(heliosview_webview_t* wv, uint64_t call_id, const char* name,
                      const char* args_json, void* userdata)
{
    (void)name;
    (void)args_json;
    (void)userdata;
    auto* win = hv_webview_owner(wv);
    if (!win || !win->hwnd) {
        heliosview_webview_reject(wv, call_id, R"({"error":"no window"})");
        return;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  R"({"maximized":%s,"minimized":%s,"fullscreen":%s,"maximizable":%s,"titleBarHeight":%d})",
                  IsZoomed(win->hwnd) ? "true" : "false",
                  IsIconic(win->hwnd) ? "true" : "false",
                  win->fullscreen ? "true" : "false",
                  win->resizable ? "true" : "false",
                  static_cast<int>(hv_title_bar_height(win->hwnd)));
    heliosview_webview_resolve(wv, call_id, buf);
}

/* "__hv.drag"() — start a window drag (the <helios-window-title-bar> component
 * calls it on mousedown over its own strip). Same mechanism as
 * heliosview_window_start_drag: a full-bleed WebView eats WM_NCHITTEST, so the
 * page must initiate the move loop itself. */
void hv_drag_bind_cb(heliosview_webview_t* wv, uint64_t call_id, const char* name,
                     const char* args_json, void* userdata)
{
    (void)name;
    (void)args_json;
    (void)userdata;
    auto* win = hv_webview_owner(wv);
    if (!win || !win->hwnd) {
        heliosview_webview_reject(wv, call_id, R"({"error":"no window"})");
        return;
    }
    ReleaseCapture();
    const DWORD pos = GetMessagePos();
    SendMessageW(win->hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                 MAKELPARAM(static_cast<int16_t>(LOWORD(pos)),
                            static_cast<int16_t>(HIWORD(pos))));
    heliosview_webview_resolve(wv, call_id, R"({"ok":true})");
}

/* Internal: register a built-in bridge method under a reserved __hv_* name,
 * bypassing the user-facing heliosview_webview_bind (which rejects reserved
 * names so applications cannot shadow the built-in components' bridge).
 * UI thread. */
void hv_bind_builtin(heliosview_webview_t* wv, const char* name,
                     heliosview_webview_bind_cb cb)
{
    auto it = wv->bindings.find(name);
    if (it != wv->bindings.end() && it->second.dtor)
        it->second.dtor(it->second.userdata);
    wv->bindings[name] = hv_webview_binding{cb, nullptr, nullptr};
}

} // namespace

heliosview_webview_t* heliosview_webview_create(heliosview_window_t* parent)
{
    if (!parent || !parent->hwnd)
        return nullptr;

    auto* webview = hv::hv_alloc<heliosview_webview>();
    webview->parent = parent->hwnd;
    parent->webviews.push_back(webview); /* attached: WM_SIZE resizes it, WM_HV_WEBVIEW_MSG dispatches to it */
    webview->creating = true;

    /* hv_alloc + Release: hand the initial reference to the API (Release to zero deletes it when done) */
    auto* env_handler = hv::hv_alloc<env_completed_handler>(
        [webview](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
                webview->creating = false;
                return result;
            }
            /* controller ready: display the WebView in the parent's client area */
            auto* controller_handler = hv::hv_alloc<controller_completed_handler>(
                [webview](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    webview->creating = false;
                    if (FAILED(result) || !controller)
                        return result;
                    webview->controller = controller;
                    controller->get_CoreWebView2(&webview->webview);
                    controller->put_IsVisible(TRUE);
                    controller->put_Bounds(hv_webview_rect(webview));
                   

                    /* JS -> native messaging */
                    auto* msg_handler = hv::hv_alloc<web_message_received_handler>(
                        [webview](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            (void)sender;
                            LPWSTR raw = nullptr;
                            /* The shim posts strings, so read the raw message text
                             * (TryGetWebMessageAsString fails if the page posted a
                             * non-string, which we intentionally ignore). */
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                const std::string msg = wide_to_utf8(raw);
                                CoTaskMemFree(raw);
                                uint64_t id = 0;
                                std::string name, args_json;
                                if (parse_call_envelope(msg, id, name, args_json))
                                    hv_dispatch_call(webview, id, name.c_str(), args_json.c_str());
                                else {
                                    std::string bc_name, bc_data;
                                    if (parse_broadcast_envelope(msg, bc_name, bc_data))
                                        hv_dispatch_broadcast(webview, bc_name, bc_data);
                                }
                            }
                            return S_OK;
                        });
                    webview->webview->add_WebMessageReceived(msg_handler, &webview->message_token);
                    msg_handler->Release();

                    /* shim: injected into every document (AddScriptToExecuteOnDocumentCreated
                     * runs on all future navigations automatically) */
                    auto* script_handler = hv::hv_alloc<add_script_completed_handler>(
                        [webview](HRESULT errorCode, LPCWSTR result) -> HRESULT {
                            (void)errorCode; (void)result;
                            webview->ready = true;
                            /* run operations queued during initialization */
                            for (auto& op : webview->pending_ops) {
                                const std::wstring wscript = utf8_to_wide(op.script);
                                if (op.async) {
                                    auto* eh = hv::hv_alloc<execute_script_completed_handler>(
                                        [webview, op](HRESULT errorCode, LPCWSTR result) -> HRESULT {
                                            if (op.callback) {
                                                const std::string out = result ? wide_to_utf8(result) : std::string{};
                                                op.callback(FAILED(errorCode) ? -static_cast<int>(errorCode) : 0,
                                                            out.c_str(), op.userdata);
                                            }
                                            return S_OK;
                                        });
                                    webview->webview->ExecuteScript(wscript.c_str(), eh);
                                    eh->Release();
                                } else {
                                    webview->webview->ExecuteScript(wscript.c_str(), nullptr);
                                }
                            }
                            webview->pending_ops.clear();
                            return S_OK;
                        });
                    webview->webview->AddScriptToExecuteOnDocumentCreated(
                        utf8_to_wide(kWebView2BridgeScript).c_str(), script_handler);
                    script_handler->Release();

                    /* navigation completed: report load results to the registered callback */
                    auto* nav_handler = hv::hv_alloc<navigation_completed_handler>(
                        [webview](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            BOOL success = FALSE;
                            if (SUCCEEDED(args->get_IsSuccess(&success)) && success) {
                                if (webview->nav_cb)
                                    webview->nav_cb(webview, 0, webview->nav_userdata);
                                return S_OK;
                            }
                            /* failed navigation: report the WebErrorStatus code
                             * (COREWEBVIEW2_WEB_ERROR_STATUS_*, 0 = UNKNOWN) */
                            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                            args->get_WebErrorStatus(&status);
                            if (webview->nav_cb)
                                webview->nav_cb(webview, static_cast<int>(status) + 1,
                                                webview->nav_userdata);
                            return S_OK;
                        });
                    webview->webview->add_NavigationCompleted(nav_handler, &webview->nav_token);
                    nav_handler->Release();

                    /* navigation starting: expose the target URI (and veto via the
                     * callback's return value) before the navigation proceeds */
                    auto* nav_start_handler = hv::hv_alloc<navigation_starting_handler>(
                        [webview](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            LPWSTR raw = nullptr;
                            if (FAILED(args->get_Uri(&raw)) || !raw)
                                return S_OK; /* no URI: let it proceed */
                            const std::string uri = wide_to_utf8(raw);
                            CoTaskMemFree(raw);
                            /* IsUserInitiated lives on the args2 interface */
                            BOOL user_initiated = FALSE;
                            Microsoft::WRL::ComPtr<ICoreWebView2NavigationStartingEventArgs2> args2;
                            BOOL is_redirected = FALSE;
                            args->get_IsRedirected(&is_redirected);
                            if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))))
                                args2->get_IsUserInitiated(&user_initiated);
                            const int veto = (webview->nav_start_cb)
                                ? webview->nav_start_cb(webview, uri.c_str(),
                                                        is_redirected ? 1 : 0,
                                                        user_initiated ? 1 : 0,
                                                        webview->nav_start_userdata)
                                : 0;
                            args->put_Cancel(veto != 0);
                            return S_OK;
                        });
                    webview->webview->add_NavigationStarting(nav_start_handler, &webview->nav_start_token);
                    nav_start_handler->Release();

                    /* source changed: report the new current URL */
                    auto* source_handler = hv::hv_alloc<source_changed_handler>(
                        [webview](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                            BOOL is_new_document = FALSE;
                            args->get_IsNewDocument(&is_new_document);
                            if (webview->source_cb) {
                                LPWSTR raw = nullptr;
                                if (sender && SUCCEEDED(sender->get_Source(&raw)) && raw) {
                                    const std::string uri = wide_to_utf8(raw);
                                    CoTaskMemFree(raw);
                                    webview->source_cb(webview, uri.c_str(),
                                                       is_new_document ? 1 : 0,
                                                       webview->source_userdata);
                                } else {
                                    webview->source_cb(webview, "", is_new_document ? 1 : 0,
                                                       webview->source_userdata);
                                }
                            }
                            return S_OK;
                        });
                    webview->webview->add_SourceChanged(source_handler, &webview->source_token);
                    source_handler->Release();

                    /* document title: needs ICoreWebView2_2 (DocumentTitleChanged);
                     * its args expose get_DocumentTitle */
                    Microsoft::WRL::ComPtr<ICoreWebView2_2> webview2;
                    if (SUCCEEDED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview2)))) {
                        auto* title_handler = hv::hv_alloc<title_changed_handler>(
                            [webview](ICoreWebView2* sender) -> HRESULT {
                                if (webview->title_cb) {
                                    LPWSTR raw = nullptr;
                                    if (sender && SUCCEEDED(sender->get_DocumentTitle(&raw)) && raw) {
                                        const std::string title = wide_to_utf8(raw);
                                        CoTaskMemFree(raw);
                                        webview->title_cb(webview, title.c_str(),
                                                          webview->title_userdata);
                                    } else {
                                        webview->title_cb(webview, "", webview->title_userdata);
                                    }
                                }
                                return S_OK;
                            });
                        webview2->add_DocumentTitleChanged(title_handler, &webview->title_token);
                        title_handler->Release();
                    }

                    /* Enable WebView2's CSS app-region: drag/no-drag support
                     * (ICoreWebView2Settings9, stable SDK): the injected
                     * <helios-window-title-bar> component drags the window with
                     * it (no bridge round-trip), and <helios-window-controls>
                     * opts its buttons out with app-region:no-drag. */
                    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                    if (SUCCEEDED(webview->webview->get_Settings(&settings))) {
                        Microsoft::WRL::ComPtr<ICoreWebView2Settings9> settings9;
                        if (SUCCEEDED(settings.As(&settings9)))
                            settings9->put_IsNonClientRegionSupportEnabled(TRUE);
                    }

                    /* Built-in window-control bridge for the injected
                     * <helios-window-controls> / <helios-window-title-bar> web
                     * components. The "__hv.*" names are not valid C identifiers
                     * (they contain a dot), so applications cannot bind or
                     * subscribe them — only this internal whitelist can. */
                    hv_bind_builtin(webview, "__hv.control", hv_control_bind_cb);
                    hv_bind_builtin(webview, "__hv.state", hv_state_bind_cb);
                    hv_bind_builtin(webview, "__hv.drag", hv_drag_bind_cb);

                    /* run the navigation queued during initialization */
                    if (webview->has_pending) {
                        const std::wstring text = utf8_to_wide(webview->pending_text);
                        if (webview->pending_html)
                            webview->webview->NavigateToString(text.c_str());
                        else
                            webview->webview->Navigate(text.c_str());
                        webview->has_pending = false;
                    }
                    return S_OK;
                });
            const HRESULT hr = env->CreateCoreWebView2Controller(webview->parent, controller_handler);
            controller_handler->Release();
            return hr;
        });
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, env_handler);
    env_handler->Release();

    if (FAILED(hr)) {
        parent->webviews.pop_back(); /* undo the attach above (it is the last entry) */
        hv::hv_dealloc(webview);
        return nullptr;
    }
    return webview;
}

void heliosview_webview_destroy(heliosview_webview_t* webview)
{
    /* Must be called on the thread that created the WebView (the UI thread):
     * the WebView2 controller and the bound userdata dtors are released here. */
    if (!webview)
        return;
    /* Detach from the owner window first, so queued WM_HV_WEBVIEW_MSG / WM_SIZE
     * messages no longer find it. If the window is already gone (the app
     * destroyed it before this WebView, contrary to the documented order), the
     * window's list went with it — nothing to remove. */
    if (webview->parent) {
        if (auto* win = reinterpret_cast<heliosview_window_t*>(
                GetWindowLongPtrW(webview->parent, GWLP_USERDATA))) {
            auto& list = win->webviews;
            if (auto it = std::find(list.begin(), list.end(), webview); it != list.end())
                list.erase(it);
        }
    }
    if (webview->webview && webview->message_token.value != 0)
        webview->webview->remove_WebMessageReceived(webview->message_token);
    if (webview->webview && webview->nav_token.value != 0)
        webview->webview->remove_NavigationCompleted(webview->nav_token);
    if (webview->nav_dtor)
        webview->nav_dtor(webview->nav_userdata);
    webview->nav_dtor = nullptr;
    if (webview->webview && webview->nav_start_token.value != 0)
        webview->webview->remove_NavigationStarting(webview->nav_start_token);
    if (webview->nav_start_dtor)
        webview->nav_start_dtor(webview->nav_start_userdata);
    webview->nav_start_dtor = nullptr;
    if (webview->webview && webview->source_token.value != 0)
        webview->webview->remove_SourceChanged(webview->source_token);
    if (webview->source_dtor)
        webview->source_dtor(webview->source_userdata);
    webview->source_dtor = nullptr;
    if (webview->webview && webview->title_token.value != 0) {
        Microsoft::WRL::ComPtr<ICoreWebView2_2> webview2;
        if (SUCCEEDED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview2))))
            webview2->remove_DocumentTitleChanged(webview->title_token);
    }
    if (webview->title_dtor)
        webview->title_dtor(webview->title_userdata);
    webview->title_dtor = nullptr;
    for (const auto& [name, binding] : webview->bindings)
        if (binding.dtor)
            binding.dtor(binding.userdata);
    webview->bindings.clear();
    for (const auto& [name, sub] : webview->subscriptions)
        if (sub.dtor)
            sub.dtor(sub.userdata);
    webview->subscriptions.clear();
    webview->controller.Reset(); /* release the controller first; the parent window is destroyed afterwards */
    webview->webview.Reset();
    hv::hv_dealloc(webview);
}

int heliosview_webview_navigate(heliosview_webview_t* webview, const char* url)
{
    if (!webview || !url)
        return -1;
    if (!webview->ready) {
        webview->pending_text = url; /* queued: run after init completes (last one wins) */
        webview->pending_html = false;
        webview->has_pending = true;
        return 0;
    }
    const std::wstring wurl = utf8_to_wide(url);
    return SUCCEEDED(webview->webview->Navigate(wurl.c_str())) ? 0 : -1;
}

int heliosview_webview_navigate_html(heliosview_webview_t* webview, const char* html)
{
    if (!webview || !html)
        return -1;
    if (!webview->ready) {
        webview->pending_text = html; /* queued: run after init completes (last one wins) */
        webview->pending_html = true;
        webview->has_pending = true;
        return 0;
    }
    const std::wstring whtml = utf8_to_wide(html);
    return SUCCEEDED(webview->webview->NavigateToString(whtml.c_str())) ? 0 : -1;
}

int heliosview_webview_bind(heliosview_webview_t* webview, const char* name,
                            heliosview_webview_bind_cb callback, void* userdata,
                            heliosview_webview_userdata_dtor dtor)
{
    if (!webview || !name || !callback)
        return -1;
    /* names must be C identifiers ([A-Za-z_][A-Za-z0-9_]*): this is both the
     * binding convention and what keeps the wire header separator-safe. The
     * library's internal bridge names ("__hv.*", see hv_internal_name) contain a
     * dot and therefore fail this check — they cannot be bound by applications. */
    if (!hv_valid_name(name))
        return -2;
    /* bindings are owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, name = std::string(name), callback, userdata, dtor] {
            heliosview_webview_bind(wv, name.c_str(), callback, userdata, dtor);
        });
        return 0;
    }
    auto it = webview->bindings.find(name);
    if (it != webview->bindings.end() && it->second.dtor)
        it->second.dtor(it->second.userdata); /* replacing an existing binding */
    webview->bindings[name] = hv_webview_binding{callback, userdata, dtor};
    return 0;
}

int heliosview_webview_resolve(heliosview_webview_t* webview, uint64_t call_id, const char* result_json)
{
    if (!webview)
        return -1;
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, call_id, result = std::string(result_json ? result_json : "null")] {
            heliosview_webview_resolve(wv, call_id, result.c_str());
        });
        return 0;
    }
    std::string s = "HV\tresolve\t" + std::to_string(call_id) + "\r\n\r\n"
                  + (result_json ? result_json : "null");
    hv_post_string(webview, s);
    return 0;
}

int heliosview_webview_reject(heliosview_webview_t* webview, uint64_t call_id, const char* error_json)
{
    if (!webview)
        return -1;
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, call_id, err = std::string(error_json ? error_json : "{}")] {
            heliosview_webview_reject(wv, call_id, err.c_str());
        });
        return 0;
    }
    std::string s = "HV\treject\t" + std::to_string(call_id) + "\r\n\r\n"
                  + (error_json ? error_json : "{}");
    hv_post_string(webview, s);
    return 0;
}

int heliosview_webview_eval(heliosview_webview_t* webview, const char* script)
{
    if (!webview || !script)
        return -1;
    if (!webview->ready) {
        webview->pending_ops.push_back({script, false, nullptr, nullptr});
        return 0;
    }
    const std::wstring wscript = utf8_to_wide(script);
    return SUCCEEDED(webview->webview->ExecuteScript(wscript.c_str(), nullptr)) ? 0 : -1;
}

int heliosview_webview_eval_async(heliosview_webview_t* webview, const char* script,
                                  heliosview_webview_eval_cb callback, void* userdata)
{
    if (!webview || !script || !callback)
        return -1;
    if (!webview->ready) {
        webview->pending_ops.push_back({script, true, callback, userdata});
        return 0;
    }
    const std::wstring wscript = utf8_to_wide(script);
    auto* handler = hv::hv_alloc<execute_script_completed_handler>(
        [wv = webview, callback, userdata](HRESULT errorCode, LPCWSTR result) -> HRESULT {
            if (callback) {
                const std::string out = result ? wide_to_utf8(result) : std::string{};
                callback(FAILED(errorCode) ? -static_cast<int>(errorCode) : 0,
                         out.c_str(), userdata);
            }
            return S_OK;
        });
    const HRESULT hr = webview->webview->ExecuteScript(wscript.c_str(), handler);
    handler->Release();
    return SUCCEEDED(hr) ? 0 : -1;
}

int heliosview_webview_broadcast(heliosview_webview_t* webview, const char* name, const char* data_json)
{
    if (!webview || !name)
        return -1;
    if (!hv_valid_name(name))
        return -2; /* would break the wire header */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, name = std::string(name),
                        data = std::string(data_json ? data_json : "null")] {
            heliosview_webview_broadcast(wv, name.c_str(), data.c_str());
        });
        return 0;
    }
    std::string s = std::string("HV\tbroadcast\t") + name + "\r\n\r\n"
                  + (data_json ? data_json : "null");
    hv_post_string(webview, s);
    return 0;
}

int heliosview_webview_subscribe(heliosview_webview_t* webview, const char* name,
                                 heliosview_webview_subscribe_cb callback, void* userdata,
                                 heliosview_webview_userdata_dtor dtor)
{
    if (!webview || !name || !callback)
        return -1;
    /* names must be C identifiers, same rule as bind (keeps the wire header
     * separator-safe and consistent with the function-naming convention); the
     * internal "__hv.*" bridge names (hv_internal_name) fail this check and
     * cannot be subscribed by applications. */
    if (!hv_valid_name(name))
        return -2;
    /* subscriptions are owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, name = std::string(name), callback, userdata, dtor] {
            heliosview_webview_subscribe(wv, name.c_str(), callback, userdata, dtor);
        });
        return 0;
    }
    auto it = webview->subscriptions.find(name);
    if (it != webview->subscriptions.end() && it->second.dtor)
        it->second.dtor(it->second.userdata); /* replacing an existing subscription */
    webview->subscriptions[name] = hv_webview_subscription{callback, userdata, dtor};
    return 0;
}

int heliosview_webview_unsubscribe(heliosview_webview_t* webview, const char* name)
{
    if (!webview || !name)
        return -1;
    /* subscriptions are owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, name = std::string(name)] {
            heliosview_webview_unsubscribe(wv, name.c_str());
        });
        return 0;
    }
    auto it = webview->subscriptions.find(name);
    if (it != webview->subscriptions.end()) {
        if (it->second.dtor)
            it->second.dtor(it->second.userdata);
        webview->subscriptions.erase(it);
    }
    return 0;
}

int heliosview_webview_set_navigation_callback(heliosview_webview_t* webview,
                                               heliosview_webview_navigation_cb callback,
                                               void* userdata,
                                               heliosview_webview_userdata_dtor dtor)
{
    if (!webview)
        return -1;
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, callback, userdata, dtor] {
            heliosview_webview_set_navigation_callback(wv, callback, userdata, dtor);
        });
        return 0;
    }
    if (webview->nav_dtor)
        webview->nav_dtor(webview->nav_userdata); /* replacing an existing callback */
    webview->nav_cb = callback;
    webview->nav_userdata = userdata;
    webview->nav_dtor = dtor;
    return 0;
}

int heliosview_webview_set_navigation_starting_callback(
    heliosview_webview_t* webview,
    heliosview_webview_navigation_starting_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor)
{
    if (!webview)
        return -1;
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, callback, userdata, dtor] {
            heliosview_webview_set_navigation_starting_callback(wv, callback, userdata, dtor);
        });
        return 0;
    }
    if (webview->nav_start_dtor)
        webview->nav_start_dtor(webview->nav_start_userdata); /* replacing an existing callback */
    webview->nav_start_cb = callback;
    webview->nav_start_userdata = userdata;
    webview->nav_start_dtor = dtor;
    return 0;
}

int heliosview_webview_set_source_changed_callback(heliosview_webview_t* webview,
                                                   heliosview_webview_source_changed_cb callback,
                                                   void* userdata,
                                                   heliosview_webview_userdata_dtor dtor)
{
    if (!webview)
        return -1;
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, callback, userdata, dtor] {
            heliosview_webview_set_source_changed_callback(wv, callback, userdata, dtor);
        });
        return 0;
    }
    if (webview->source_dtor)
        webview->source_dtor(webview->source_userdata); /* replacing an existing callback */
    webview->source_cb = callback;
    webview->source_userdata = userdata;
    webview->source_dtor = dtor;
    return 0;
}

int heliosview_webview_set_title_changed_callback(heliosview_webview_t* webview,
                                                  heliosview_webview_title_changed_cb callback,
                                                  void* userdata,
                                                  heliosview_webview_userdata_dtor dtor)
{
    if (!webview)
        return -1;
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, callback, userdata, dtor] {
            heliosview_webview_set_title_changed_callback(wv, callback, userdata, dtor);
        });
        return 0;
    }
    if (webview->title_dtor)
        webview->title_dtor(webview->title_userdata); /* replacing an existing callback */
    webview->title_cb = callback;
    webview->title_userdata = userdata;
    webview->title_dtor = dtor;
    return 0;
}

int heliosview_webview_map_local_folder(heliosview_webview_t* webview,
                                        const char* host_name,
                                        const char* folder_path)
{
    if (!webview || !host_name || !folder_path || !webview->webview)
        return -1;
    /* virtual host mappings are owned by the UI thread (require the controller) */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, host = std::string(host_name),
                        folder = std::string(folder_path)] {
            heliosview_webview_map_local_folder(wv, host.c_str(), folder.c_str());
        });
        return 0;
    }
    /* SetVirtualHostNameToFolderMapping lives on ICoreWebView2_3 (WebView2 SDK
     * 1.0.1293.44+); the runtime supports it, but the interface must be queried
     * from the base interface */
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
    if (FAILED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview3))))
        return -1;
    const HRESULT hr = webview3->SetVirtualHostNameToFolderMapping(
        utf8_to_wide(host_name).c_str(), utf8_to_wide(folder_path).c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    return SUCCEEDED(hr) ? 0 : -static_cast<int>(hr);
}

int heliosview_webview_set_insets(heliosview_webview_t* webview,
                                  int32_t top, int32_t right,
                                  int32_t bottom, int32_t left)
{
    if (!webview)
        return -1;
    /* The WebView2 controller is UI-thread bound (put_Bounds included); follow
     * the same marshalling contract as the other webview APIs. */
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, top, right, bottom, left] {
            heliosview_webview_set_insets(wv, top, right, bottom, left);
        });
        return 0;
    }
    webview->inset_top = top > 0 ? top : 0;
    webview->inset_right = right > 0 ? right : 0;
    webview->inset_bottom = bottom > 0 ? bottom : 0;
    webview->inset_left = left > 0 ? left : 0;
    webview->has_insets = true;
    if (auto* controller = webview->controller.Get())
        controller->put_Bounds(hv_webview_rect(webview));
    return 0;
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
 * the thread stays COM-enabled for the process lifetime, matching WebView2. */
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
    default:      return HELIOSVIEW_MESSAGE_RESULT_NONE;
    }
}

/* ================= System helpers ================= */

int heliosview_open_url(const char* url)
{
    if (!url || !*url)
        return -1;
    const std::wstring wurl = utf8_to_wide(url);
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32 ? 0 : -1;
}

int heliosview_show_in_folder(const char* path)
{
    if (!path || !*path)
        return -1;
    const std::wstring wpath = utf8_to_wide(path);
    /* /select,"<path>" (quoted: the path may contain spaces) */
    const std::wstring args = L"/select,\"" + wpath + L"\"";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32 ? 0 : -1;
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
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h) != nullptr) {
            void* dst = GlobalLock(h);
            if (dst) {
                std::memcpy(dst, w.c_str(), bytes);
                GlobalUnlock(h);
                result = 0;
            }
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
