// HeliosView.dll — Windows WebView2 backend (heliosview_webview_* API).
// All WebView2 code lives here, separated from the window/message-loop layer
// (heliosview_window_win32.cpp). The window layer never references a WebView2 type.
//
// Coupling is one-directional: a WebView knows its parent window's HWND (it is
// a child of it) and reaches the window only through the public API
// (heliosview_window_id / _is_resizable / _is_fullscreen / _from_id, plus
// hv_window_hwnd); the window layer knows nothing about WebViews.
//
// Self-management via SetWindowSubclass: the WebView installs a subclass on its
// parent window at creation and removes it at destruction. The subclass callout
// (hv_webview_subclass_proc) keeps the WebView in sync with its parent
// (WM_SIZE → put_Bounds / put_IsVisible) and doubles as the liveness guard: a
// running callout proves the subclass is still installed, which proves the
// WebView object is still alive (destroy removes the subclass before freeing),
// so no HWND → WebView registry is needed anywhere.
//
// Threading: every WebView API is a UI-thread call — the thread that created
// the WebView (the parent window's message-loop thread). Calls from any other
// thread fail with a negative error instead of being marshalled.

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"
#include "heliosview_win32_internal.h"

#include <wrl/client.h> /* ComPtr */
#define WEBVIEW2_USE_EXPERIMENTAL
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h> /* CoreWebView2EnvironmentOptions (env options impl) */
#include <WebView2Experimental.h>
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
 * objects are created). */
template <class K, class V>
using hv_pmr_flat_map = std::flat_map<K, V, std::less<K>,
                                      std::pmr::vector<K>, std::pmr::vector<V>>;

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

struct heliosview_webview {
    HWND parent = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    Microsoft::WRL::ComPtr<ICoreWebView2ExperimentalWindowControlsOverlay> window_controls_overlay;
    bool creating = false;
    bool ready = false;
    bool has_pending = false;   /* navigation queued before init completes (only the last one runs) */
    bool pending_html = false;  /* true = NavigateToString, false = Navigate */
    std::string pending_text;   /* UTF-8 */
    /* WebView2 user data folder (UTF-16; empty = runtime default next to the
     * executable). Set at creation via heliosview_webview_create_ex; read once
     * when the WebView2 environment is created. */
    std::wstring user_data_folder;

    /* virtual-host folder mappings queued before init completes (applied when
     * the core becomes ready, before the queued navigation runs) */
    std::vector<std::pair<std::string, std::string>> pending_mappings;

    /* WebView placement within the parent client area (client pixels); set via
     * heliosview_webview_set_insets. The WebView fills the client area minus
     * these insets, so a strip (e.g. a title bar with the DWM caption buttons)
     * can stay visible around it. */
    bool has_insets = false;
    int last_native_error = 0;  /* COREWEBVIEW2_WEB_ERROR_STATUS_* of the last failed navigation */

    /* Bridge eval channel: the shim (window.__hvEval) evaluates scripts and
     * awaits returned promises, which WebView2's ExecuteScript does not. Used
     * once a document exists (shim_ready); ExecuteScript remains the fallback. */
    bool shim_ready = false;    /* set on the first NavigationCompleted (document created) */
    uint64_t next_eval_id = 1;
    std::flat_map<uint64_t, std::pair<heliosview_webview_eval_cb, void*>> pending_evals;
    int32_t inset_top = 0, inset_right = 0, inset_bottom = 0, inset_left = 0;

    /* WebView2 status bar (hovered-link target URL, bottom-left); off by
     * default, toggleable at runtime via heliosview_webview_set_status_bar. */
    bool status_bar_enabled = false;

    /* Right-click: whether the engine's own menu may open (on by default,
     * toggleable via heliosview_webview_set_context_menu) plus the optional
     * interception callback consulted for every right click - returning non-zero
     * from it keeps the engine's menu closed for that click. */
    bool context_menu_enabled = true;
    heliosview_webview_context_menu_cb context_menu_cb = nullptr;
    void* context_menu_userdata = nullptr;
    heliosview_webview_userdata_dtor context_menu_dtor = nullptr;
    EventRegistrationToken context_menu_token{}; /* ContextMenuRequested handler (ICoreWebView2_11) */

    /* WebView2 DevTools access (F12 / right-click Inspect); on by default,
     * toggleable at runtime via heliosview_webview_set_devtools. */
    bool devtools_enabled = true;

    /* WebView2 built-in window controls overlay (min/max/restore/close buttons
     * drawn over the page's top-right). Off by default: apps that draw their
     * own title-bar buttons (e.g. the injected <helios-window-controls>
     * component) leave it off. Toggleable at runtime via
     * heliosview_webview_set_window_controls_overlay. */
    bool window_controls_enabled = false;

    /* Window controls overlay background color (alpha, red, green, blue).
     * Default: fully transparent — the page's own title bar shows through and
     * the buttons float over it. Set via
     * heliosview_webview_set_window_controls_background_color. */
    COREWEBVIEW2_COLOR window_controls_background_color{.A = 0, .R = 0, .G = 0, .B = 0};

    /* low-footprint mode (WebView2 TrySuspend / Resume): requested/applied
     * state, plus the completion callback of a suspend requested before the
     * core was ready (applied at ready, after queued navigation/scripts). */
    bool suspended = false;
    heliosview_webview_suspend_cb suspend_cb = nullptr;
    void* suspend_userdata = nullptr;

    /* WebView2 default background color (COREWEBVIEW2_COLOR: alpha, red,
     * green, blue); applied when the core becomes ready. Default: opaque white. */
    COREWEBVIEW2_COLOR background_color{.A = 255, .R = 255, .G = 255, .B = 255};

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
};

/* The WebView's bounds within the parent client area: the full client rect
 * shrunk by the registered insets. Used wherever the WebView is placed (at
 * creation, on every parent resize via the subclass callout, and after a
 * set_insets call), so a native title-bar strip carrying the DWM caption
 * buttons can stay visible above the WebView. */
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

/* Map the engine's navigation error to the portable heliosview_webview_error_t.
 * The engine's own value is kept separately for heliosview_webview_last_native_error. */
int hv_webview_error_from_native(COREWEBVIEW2_WEB_ERROR_STATUS status)
{
    switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
        return HELIOSVIEW_WEBVIEW_ERROR_TLS;
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
        return HELIOSVIEW_WEBVIEW_ERROR_CONNECTION_FAILED;
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT:
        return HELIOSVIEW_WEBVIEW_ERROR_TIMEOUT;
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED:
        return HELIOSVIEW_WEBVIEW_ERROR_HOST_NOT_FOUND;
    case COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED:
        return HELIOSVIEW_WEBVIEW_ERROR_CANCELLED;
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE:
        return HELIOSVIEW_WEBVIEW_ERROR_HTTP;
    default:
        return HELIOSVIEW_WEBVIEW_ERROR_OTHER;
    }
}

/* Post a raw envelope string to the JS page (must run on the UI thread). */
void hv_post_string(heliosview_webview_t* wv, const std::string& s)
{
    if (wv && wv->webview && wv->ready) {
        const std::wstring w = utf8_to_wide(s);
        wv->webview->PostWebMessageAsString(w.c_str());
    }
}

/* The JS bridge shim, injected into every document (AddScriptToExecuteOnDocumentCreated
 * takes raw script text). window.helios.call invokes native functions; BroadcastChannel
 * is subclassed so native broadcasts dispatch synthetic message events and page
 * postMessage()s are forwarded to native subscriptions; <helios-window-controls> is the
 * built-in title-bar button web component. The shim lives in a real .js file
 * (editable/versionable/lintable on its own): CMake wraps ../webview_bridge.js
 * into the generated webview_bridge.inc below (C++23 #embed is not supported by
 * MSVC yet). The shim itself is engine-neutral — its transport adapter detects
 * WebView2 / WKWebView / WebKitGTK — so the other backends embed the same file. */
#include "webview_bridge.inc" /* generated from ../webview_bridge.js (see src/CMakeLists.txt): kWebView2BridgeScript */

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

} // namespace

/* ================= Parent-window subclass =================
 *
 * The WebView self-registers as a subclass of its parent window
 * (SetWindowSubclass, see heliosview_webview_create_ex) and removes itself on
 * destroy / WM_NCDESTROY. The callout is the WebView's only touchpoint with the
 * window's message stream — the window layer itself stays WebView2-free.
 *
 * Liveness: the callout's dwRefData IS the WebView pointer, and a running
 * callout proves the subclass is still installed, which proves the WebView
 * object is still alive (destroy removes the subclass before freeing). So the
 * callout dereferences dwRefData freely. */

inline const UINT WM_HV_WEBVIEW_SYNC = WM_APP + 0x41; /* deferred webview bounds/visibility sync (posted from WM_SIZE) */

LRESULT CALLBACK hv_webview_subclass_proc(HWND hwnd, UINT message, WPARAM wparam,
                                          LPARAM lparam, UINT_PTR, DWORD_PTR ref)
{
    auto* wv = reinterpret_cast<heliosview_webview_t*>(ref);
    switch (message) {
    case WM_NCDESTROY:
        /* the parent window is being destroyed (normally after the WebView, but
         * the documented order is not enforced): unsubclass so the system can
         * tear the subclass chain down cleanly */
        RemoveWindowSubclass(hwnd, hv_webview_subclass_proc, 0);
        break;
    case WM_HV_WEBVIEW_SYNC: {
        /* deferred bounds/visibility sync (posted by WM_SIZE): runs OUTSIDE the
         * WM_SIZE dispatch so the cross-process put_Bounds COM call never
         * re-enters the browser while the host is still dispatching the resize.
         * (Kept deferred on principle — it does not change the WCO glyph state,
         * which WebView2 does not track for externally-driven window state
         * changes at all, see the WCO probe.) */
        if (wparam != SIZE_MAXHIDE && wparam != SIZE_MAXSHOW) {
            if (auto* controller = wv->controller.Get()) {
                if (wparam == SIZE_MINIMIZED) {
                    controller->put_IsVisible(FALSE);
                } else {
                    controller->put_Bounds(hv_webview_rect(wv));
                    if (wparam == SIZE_RESTORED)
                        controller->put_IsVisible(TRUE);
                }
            }
        }
        break;
    }
    case WM_SIZE:
        /* Keep the WebView in sync with its parent. The actual put_Bounds /
         * put_IsVisible calls are DEFERRED (see WM_HV_WEBVIEW_SYNC): issuing a
         * cross-process COM call from inside the WM_SIZE dispatch disturbed
         * WebView2's window-state tracking (the WCO maximize glyph never
         * updated). SIZE_MAXHIDE/SIZE_MAXSHOW notify owned windows that ANOTHER
         * window was maximized/restored — not a change of this window. The
         * message itself is always passed through so the window's own WM_SIZE
         * handling (event emission) still runs. */
        if (wparam != SIZE_MAXHIDE && wparam != SIZE_MAXSHOW)
            PostMessageW(hwnd, WM_HV_WEBVIEW_SYNC, wparam, 0);
        break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

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

/* One COM callback wrapper for every WebView2 event handler: holds a
 * std::function matching the interface's Invoke signature and forwards the
 * call. The handlers are only used internally, so a single template replaces
 * one hand-written struct per interface. */
template <typename Interface, typename... Args>
struct hv_callback : com_callback_base<Interface> {
    using Fn = std::function<HRESULT(Args...)>;
    explicit hv_callback(Fn fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP Invoke(Args... args) noexcept override
    {
        return m_fn(args...);
    }
    Fn m_fn;
};

using env_completed_handler =
    hv_callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>;
using controller_completed_handler =
    hv_callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>;
using web_message_received_handler =
    hv_callback<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;
using navigation_completed_handler =
    hv_callback<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>;
using navigation_starting_handler =
    hv_callback<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>;
using source_changed_handler =
    hv_callback<ICoreWebView2SourceChangedEventHandler, ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*>;
using title_changed_handler =
    hv_callback<ICoreWebView2DocumentTitleChangedEventHandler, ICoreWebView2*, IUnknown*>;
using context_menu_requested_handler =
    hv_callback<ICoreWebView2ContextMenuRequestedEventHandler, ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs*>;
using add_script_completed_handler =
    hv_callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, HRESULT, LPCWSTR>;
using execute_script_completed_handler =
    hv_callback<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>;
using try_suspend_completed_handler =
    hv_callback<ICoreWebView2TrySuspendCompletedHandler, HRESULT, BOOL>;
using window_close_requested_handler =
    hv_callback<ICoreWebView2WindowCloseRequestedEventHandler, ICoreWebView2*, IUnknown*>;

/* Backing storage for heliosview_context_menu_info_t's strings: the C struct
 * points into it, so it must outlive the callback that receives the struct. */
struct hv_context_menu_strings {
    std::string link_url;
    std::string link_text;
    std::string selection_text;
    std::string page_url;
};

/* Copy one WebView2 wide string into `dst` and free the COM allocation. Empty on
 * failure (the C API reports "" rather than NULL). */
static void hv_take_wide(LPWSTR raw, std::string& dst)
{
    if (!raw)
        return;
    dst = wide_to_utf8(raw);
    CoTaskMemFree(raw);
}

/* Translate a ContextMenuRequested into the C-facing info (target bits + the
 * strings in `strings`). A missing/unknown target yields an all-zero info, which
 * the callback reads as "nothing specific was hit". */
static void hv_fill_context_menu_info(ICoreWebView2ContextMenuRequestedEventArgs* args,
                                      heliosview_context_menu_info_t* out,
                                      hv_context_menu_strings& strings)
{
    if (!args || !out)
        return;

    POINT location{};
    if (SUCCEEDED(args->get_Location(&location))) {
        out->x = location.x;
        out->y = location.y;
    }

    Microsoft::WRL::ComPtr<ICoreWebView2ContextMenuTarget> target;
    if (FAILED(args->get_ContextMenuTarget(&target)) || !target)
        return;

    COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND kind = COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_PAGE;
    target->get_Kind(&kind);
    BOOL editable = FALSE, has_link = FALSE, has_selection = FALSE;
    target->get_IsEditable(&editable);
    target->get_HasLinkUri(&has_link);
    target->get_HasSelection(&has_selection);

    uint32_t bits = 0;
    switch (kind) {
    case COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_PAGE:
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_PAGE;
        break;
    case COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_IMAGE:
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_IMAGE;
        break;
    case COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_AUDIO:
    case COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_VIDEO:
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_MEDIA;
        break;
    case COREWEBVIEW2_CONTEXT_MENU_TARGET_KIND_SELECTED_TEXT:
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_SELECTION;
        break;
    default:
        break;
    }
    if (has_link)
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_LINK;
    if (has_selection)
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_SELECTION;
    if (editable)
        bits |= HELIOSVIEW_CONTEXT_MENU_TARGET_EDITABLE;
    out->target = bits;

    if (has_link) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(target->get_LinkUri(&raw)))
            hv_take_wide(raw, strings.link_url);
        BOOL has_text = FALSE;
        target->get_HasLinkText(&has_text);
        if (has_text && SUCCEEDED(target->get_LinkText(&raw)))
            hv_take_wide(raw, strings.link_text);
    }
    if (has_selection) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(target->get_SelectionText(&raw)))
            hv_take_wide(raw, strings.selection_text);
    }
    {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(target->get_PageUri(&raw)))
            hv_take_wide(raw, strings.page_url);
    }

    out->link_url = strings.link_url.c_str();
    out->link_text = strings.link_text.c_str();
    out->selection_text = strings.selection_text.c_str();
    out->page_url = strings.page_url.c_str();
}

/* Apply a low-footprint suspend (TrySuspend) on the UI thread; the core must
 * be initialized. callback/userdata deliver the async completion (may be NULL). */
int hv_webview_try_suspend(heliosview_webview_t* webview,
                           heliosview_webview_suspend_cb callback, void* userdata)
{
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
    const HRESULT hr_qi = webview->webview->QueryInterface(IID_PPV_ARGS(&webview3));
    if (FAILED(hr_qi))
        return hv_fail_hresult(hr_qi, "QueryInterface(ICoreWebView2_3) failed");
    auto* handler = hv::hv_alloc<try_suspend_completed_handler>(
        [webview, callback, userdata](HRESULT result, BOOL is_suspended) -> HRESULT {
            /* the tracked state follows the completed attempt */
            webview->suspended = is_suspended ? true : webview->suspended;
            if (callback)
                callback(SUCCEEDED(result) ? 0 : -static_cast<int>(result),
                         is_suspended ? 1 : 0, userdata);
            return S_OK;
        });
    if (!handler)
        return hv_fail(-1, "out of memory (callback handler alloc failed)");
    const HRESULT hr = webview3->TrySuspend(handler);
    handler->Release();
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "TrySuspend failed");
}

/* Enable/disable WebView2's built-in window controls overlay (the min/max/
 * restore/close buttons drawn over the page's top-right). Off by default so
 * apps can draw their own title-bar buttons (e.g. the injected
 * <helios-window-controls> component). Requires the experimental
 * ICoreWebView2Experimental31 interface; returns -1 if the runtime does not
 * support it. UI thread. */
int hv_webview_apply_window_controls(heliosview_webview_t* webview)
{
    if (!webview->window_controls_enabled) {
        if (webview->window_controls_overlay)
            webview->window_controls_overlay->put_IsEnabled(FALSE);
        return 0;
    }
    if (!webview->window_controls_overlay) {
        Microsoft::WRL::ComPtr<ICoreWebView2Experimental31> webview2_31;
        const HRESULT hr_qi = webview->webview->QueryInterface(IID_PPV_ARGS(&webview2_31));
        if (FAILED(hr_qi))
            return hv_fail_hresult(hr_qi, "QueryInterface(ICoreWebView2Experimental31) failed (WCO not supported)"); /* experimental interface unavailable */
        const HRESULT hr_wco = webview2_31->get_WindowControlsOverlay(&webview->window_controls_overlay);
        if (FAILED(hr_wco))
            return hv_fail_hresult(hr_wco, "get_WindowControlsOverlay failed");
    }
    webview->window_controls_overlay->put_BackgroundColor(webview->window_controls_background_color);
    const HRESULT hr_enable = webview->window_controls_overlay->put_IsEnabled(TRUE);
    return SUCCEEDED(hr_enable) ? 0 : hv_fail_hresult(hr_enable, "put_IsEnabled (window controls overlay) failed");
}

/* Register the WCO close-button path: WebView2's built-in caption buttons do
 * NOT send WM_CLOSE to the host window — the close button raises the
 * WindowCloseRequested event instead (see the official sample, which closes
 * the app window in this handler). Route it into the standard close pipeline
 * (WM_CLOSE -> WINDOW_CLOSE event) so the app's closeRequested handling stays
 * the single close path. */
void hv_bind_window_close_requested(heliosview_webview_t* webview)
{
    Microsoft::WRL::ComPtr<ICoreWebView2_17> webview17;
    if (FAILED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview17))))
        return; /* older runtime: no WindowCloseRequested (no WCO close either) */
    auto* handler = hv::hv_alloc<window_close_requested_handler>(
        [webview](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
            (void)sender;
            (void)args;
            if (webview->parent)
                PostMessageW(webview->parent, WM_CLOSE, 0, 0);
            return S_OK;
        });
    EventRegistrationToken token{};
    webview17->add_WindowCloseRequested(handler, &token);
    handler->Release();
}

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

/* Parse the "HV\teval\t<id>\t<ok|error>\r\n\r\n<json|message>" reply the shim
 * sends for a heliosview_webview_eval_async routed through window.__hvEval.
 * Returns true when the message is such a reply. */
bool parse_eval_reply(const std::string& msg, uint64_t& id, bool& ok, std::string& payload)
{
    id = 0;
    ok = false;
    payload.clear();
    std::vector<std::string> head;
    std::string body;
    if (split_envelope(msg, head, body) == std::string::npos)
        return false;
    if (head.size() != 4 || head[1] != "eval")
        return false;
    uint64_t v = 0;
    for (char c : head[2]) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (head[3] != "ok" && head[3] != "error")
        return false;
    id = v;
    ok = (head[3] == "ok");
    payload = body;
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
    if (!win || !hv_window_hwnd(win)) {
        heliosview_webview_reject(wv, call_id, R"({"error":"no window"})");
        return;
    }
    const HWND hwnd = hv_window_hwnd(win);
    if (action == "minimize")
        ShowWindow(hwnd, SW_MINIMIZE);
    else if (action == "maximize") {
        /* Maximize is disabled while the window is not resizable (the maximize
         * box was removed); restoring an already-maximized window stays allowed. */
        if (!heliosview_window_is_resizable(win) && !IsZoomed(hwnd)) {
            heliosview_webview_reject(wv, call_id, R"({"error":"maximize disabled"})");
            return;
        }
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (action == "restore")
        ShowWindow(hwnd, SW_RESTORE);
    else if (action == "close")
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
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
    if (!win || !hv_window_hwnd(win)) {
        heliosview_webview_reject(wv, call_id, R"({"error":"no window"})");
        return;
    }
    const HWND hwnd = hv_window_hwnd(win);
    char buf[240];
    std::snprintf(buf, sizeof(buf),
                  R"({"maximized":%s,"minimized":%s,"fullscreen":%s,"maximizable":%s,"titleBarHeight":%d,"osBuild":%d})",
                  IsZoomed(hwnd) ? "true" : "false",
                  IsIconic(hwnd) ? "true" : "false",
                  heliosview_window_is_fullscreen(win) ? "true" : "false",
                  heliosview_window_is_resizable(win) ? "true" : "false",
                  static_cast<int>(hv_title_bar_height(hwnd)),
                  hv_os_build());
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
    if (!win || !hv_window_hwnd(win)) {
        heliosview_webview_reject(wv, call_id, R"({"error":"no window"})");
        return;
    }
    const HWND hwnd = hv_window_hwnd(win);
    ReleaseCapture();
    const DWORD pos = GetMessagePos();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
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

/* Build the WebView2 environment options object from the creation-time struct;
 * returns nullptr when no option field is set (CreateCoreWebView2-
 * EnvironmentWithOptions treats a null options object as the runtime default).
 *
 * The user data folder is deliberately NOT part of this object: it is passed
 * as its own CreateCoreWebView2EnvironmentWithOptions parameter (see
 * heliosview_webview_create_ex), so a WebView created with only a user data
 * folder keeps every runtime default — no options object is needed at all.
 *
 * TargetCompatibleBrowserVersion is only set when explicitly given: the SDK
 * options class pins it to its own concrete product version by default, and
 * the WebView2 runtime rejects an explicitly-set empty string ("Target-
 * CompatibleBrowserVersion is invalid: version="), so an unset version must
 * stay unset (the class default is then used — valid and effectively
 * "the runtime's latest"). */
Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>
hv_build_env_options(const heliosview_webview_env_opts_t* opts)
{
    if (!opts)
        return nullptr;
    const bool has_any =
        (opts->browser_executable_folder && *opts->browser_executable_folder) ||
        (opts->language && *opts->language) ||
        (opts->additional_browser_arguments && *opts->additional_browser_arguments) ||
        (opts->target_compatible_browser_version && *opts->target_compatible_browser_version) ||
        opts->allow_sso_with_os_primary_account != 0 ||
        opts->exclusive_user_data_folder_access != 0 ||
        opts->disable_tracking_prevention != 0 ||
        opts->are_browser_extensions_enabled != 0;
    if (!has_any)
        return nullptr;

    auto eo = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!eo)
        return nullptr;
    if (opts->language && *opts->language)
        eo->put_Language(utf8_to_wide(opts->language).c_str());
    if (opts->additional_browser_arguments && *opts->additional_browser_arguments)
        eo->put_AdditionalBrowserArguments(utf8_to_wide(opts->additional_browser_arguments).c_str());
    if (opts->target_compatible_browser_version && *opts->target_compatible_browser_version)
        eo->put_TargetCompatibleBrowserVersion(
            utf8_to_wide(opts->target_compatible_browser_version).c_str());
    if (opts->allow_sso_with_os_primary_account)
        eo->put_AllowSingleSignOnUsingOSPrimaryAccount(TRUE);
    if (opts->exclusive_user_data_folder_access)
        eo->put_ExclusiveUserDataFolderAccess(TRUE);
    if (opts->disable_tracking_prevention)
        eo->put_EnableTrackingPrevention(FALSE);
    if (opts->are_browser_extensions_enabled)
        eo->put_AreBrowserExtensionsEnabled(TRUE);
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> base;
    return SUCCEEDED(eo.As(&base)) ? base : nullptr;
}

} // namespace

/* ================= WebView instance registry =================
 *
 * The bridge calls (heliosview_webview_resolve / _reject / _broadcast) are
 * UI-thread calls, but the app can still pass a WebView handle AFTER
 * heliosview_webview_destroy ran (the lifetime contract: destroy only when no
 * asynchronous call is in flight). Calling them with a destroyed WebView is the
 * developer's mistake, but it must fail with a clear message, not an opaque
 * access violation.
 *
 * The registry tracks every WebView's liveness by POINTER VALUE only (the
 * pointer is never dereferenced here, so a stale lookup is memory-safe):
 *   - heliosview_webview_create_ex registers the instance as live;
 *   - heliosview_webview_destroy transitions it to destroyed (and keeps the
 *     entry, so the address still identifies the dead instance);
 *   - resolve/reject/broadcast check the state BEFORE touching the instance:
 *     a destroyed (or unknown) instance fails with the documented error code
 *     HELIOSVIEW_WEBVIEW_DESTROYED (-3) and never dereferences the freed
 *     pointer, so the misuse surfaces as a clear error instead of a crash.
 * If a new WebView reuses the address of a destroyed one, create re-registers
 * it as live and calls on the real instance keep working. */

namespace {

enum class hv_webview_slot_state : uint8_t { live, destroyed };

struct hv_webview_slot {
    heliosview_webview_t* wv; /* pointer value only — never dereferenced */
    hv_webview_slot_state state;
};

std::mutex g_webview_registry_mutex;
std::vector<hv_webview_slot> g_webview_registry;

hv_webview_slot_state hv_webview_state(const heliosview_webview_t* wv)
{
    std::lock_guard<std::mutex> lock(g_webview_registry_mutex);
    for (const auto& s : g_webview_registry)
        if (s.wv == wv)
            return s.state;
    return hv_webview_slot_state::destroyed; /* unknown instance: treat as gone (defensive) */
}

void hv_webview_register_live(heliosview_webview_t* wv)
{
    std::lock_guard<std::mutex> lock(g_webview_registry_mutex);
    for (auto& s : g_webview_registry) {
        if (s.wv == wv) { /* address reused by a new instance: back to live */
            s.state = hv_webview_slot_state::live;
            return;
        }
    }
    g_webview_registry.push_back({wv, hv_webview_slot_state::live});
}

/* Transition live -> destroyed. Returns true when the instance was live (the
 * first destroy), false for a double destroy / never-registered instance. */
bool hv_webview_mark_destroyed(heliosview_webview_t* wv)
{
    std::lock_guard<std::mutex> lock(g_webview_registry_mutex);
    for (auto& s : g_webview_registry) {
        if (s.wv == wv) {
            if (s.state == hv_webview_slot_state::destroyed)
                return false;
            s.state = hv_webview_slot_state::destroyed;
            return true;
        }
    }
    return false;
}

/* Stale-instance error for a bridge call that targets a destroyed WebView.
 * Returns a distinct, documented error code (HELIOSVIEW_WEBVIEW_DESTROYED,
 * -3) instead of touching the freed instance: the caller can see exactly why
 * the call failed (its instance was destroyed — e.g. destroyWebView ran while
 * this asynchronous resolve/reject/broadcast was still in flight) and react,
 * instead of the process crashing with an access violation. */
constexpr int kWebviewDestroyedError = -3;

} // namespace

/* ================= Controller-ready initialization =================
 * Runs once the WebView2 controller is created (async completion): places the
 * WebView in the parent client area, wires up every handler (bridge,
 * navigation / source / title events, settings, built-in window-control
 * bridge) and applies state that was set before init completed (virtual-host
 * mappings, queued navigation, background color, suspend). Extracted from the
 * create_ex completion lambdas to keep them flat. */

HRESULT hv_webview_init_core(heliosview_webview_t* webview)
{
    webview->controller->put_IsVisible(TRUE);
    webview->controller->put_Bounds(hv_webview_rect(webview));
    hv_webview_apply_window_controls(webview); /* opt-in: draws the built-in caption buttons when enabled */

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
                    else {
                        uint64_t eval_id = 0;
                        bool eval_ok = false;
                        std::string eval_payload;
                        if (parse_eval_reply(msg, eval_id, eval_ok, eval_payload)) {
                            const auto it = webview->pending_evals.find(eval_id);
                            if (it != webview->pending_evals.end()) {
                                const auto [cb, ud] = it->second;
                                webview->pending_evals.erase(it);
                                if (cb)
                                    cb(eval_ok ? 0 : -1, eval_payload.c_str(), ud);
                            }
                        }
                    }
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
            /* a low-footprint suspend requested before init applies as soon as
             * the core is ready (after the queued navigation/scripts have run) */
            if (webview->suspended) {
                hv_webview_try_suspend(webview, webview->suspend_cb,
                                       webview->suspend_userdata);
                webview->suspend_cb = nullptr;
                webview->suspend_userdata = nullptr;
            }
            return S_OK;
        });
    webview->webview->AddScriptToExecuteOnDocumentCreated(
        utf8_to_wide(kWebView2BridgeScript).c_str(), script_handler);
    script_handler->Release();

    /* navigation completed: report load results to the registered callback */
    auto* nav_handler = hv::hv_alloc<navigation_completed_handler>(
        [webview](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            /* a document now exists, so the shim is installed in it */
            webview->shim_ready = true;
            if (SUCCEEDED(args->get_IsSuccess(&success)) && success) {
                webview->last_native_error = 0;
                if (webview->nav_cb)
                    webview->nav_cb(webview, HELIOSVIEW_WEBVIEW_OK, webview->nav_userdata);
                return S_OK;
            }
            /* Failed navigation: keep the engine's own code (COREWEBVIEW2_WEB_ERROR_STATUS_*)
             * for heliosview_webview_last_native_error and report the portable
             * heliosview_webview_error_t to the callback. */
            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            args->get_WebErrorStatus(&status);
            webview->last_native_error = static_cast<int>(status);
            if (webview->nav_cb)
                webview->nav_cb(webview, hv_webview_error_from_native(status), webview->nav_userdata);
            return S_OK;
        });
    webview->webview->add_NavigationCompleted(nav_handler, &webview->nav_token);
    nav_handler->Release();

    /* navigation starting: expose the target URI (and veto via the callback's
     * return value) before the navigation proceeds */
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
            [webview](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
                (void)args;
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
     * <helios-window-title-bar> component drags the window with it (no bridge
     * round-trip), and <helios-window-controls> opts its buttons out with
     * app-region:no-drag. */
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->webview->get_Settings(&settings))) {
        /* The WebView2 status bar shows the hovered link's target URL
         * bottom-left; off by default, toggleable at runtime via
         * heliosview_webview_set_status_bar. */
        settings->put_IsStatusBarEnabled(webview->status_bar_enabled ? TRUE : FALSE);

        /* DevTools (F12 / right-click Inspect): off while the app disabled it
         * via heliosview_webview_set_devtools (an open DevTools window closes
         * as well). */
        settings->put_AreDevToolsEnabled(webview->devtools_enabled ? TRUE : FALSE);

        Microsoft::WRL::ComPtr<ICoreWebView2Settings9> settings9;
        if (SUCCEEDED(settings.As(&settings9)))
            settings9->put_IsNonClientRegionSupportEnabled(TRUE);
    }

    /* Default background color (ICoreWebView2Controller2): applied from the
     * stored value (e.g. set before init) when the core becomes ready; a
     * transparent color (alpha 0) lets the parent window show through. */
    Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(webview->controller.As(&controller2)))
        controller2->put_DefaultBackgroundColor(webview->background_color);

    /* Right-click: one always-registered handler answers two independent questions
     * - does the application intercept this click (context_menu_cb), and may the
     * engine's own menu open (context_menu_enabled)? Requires ICoreWebView2_11
     * (SDK 1.0.1418.22+, runtime 100+); on older runtimes neither the switch nor
     * the callback has any effect and the engine's menu always shows. */
    Microsoft::WRL::ComPtr<ICoreWebView2_11> webview11;
    if (SUCCEEDED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview11)))) {
        auto* ctx_handler = hv::hv_alloc<context_menu_requested_handler>(
            [webview](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                bool intercepted = false;
                if (webview->context_menu_cb) {
                    /* The info strings must outlive the callback, so the struct
                     * points into this frame's storage. */
                    hv_context_menu_strings strings;
                    heliosview_context_menu_info_t info{};
                    hv_fill_context_menu_info(args, &info, strings);
                    intercepted = webview->context_menu_cb(webview, &info,
                                                           webview->context_menu_userdata) != 0;
                }
                /* Intercepted, or nothing may open: the engine's menu stays closed. */
                if (intercepted || !webview->context_menu_enabled)
                    args->put_Handled(TRUE);
                return S_OK;
            });
        webview11->add_ContextMenuRequested(ctx_handler, &webview->context_menu_token);
        ctx_handler->Release();
    }

    /* Built-in window-control bridge for the injected
     * <helios-window-controls> / <helios-window-title-bar> web components. The
     * "__hv.*" names are not valid C identifiers (they contain a dot), so
     * applications cannot bind or subscribe them — only this internal
     * whitelist can. */
    hv_bind_builtin(webview, "__hv.control", hv_control_bind_cb);
    hv_bind_builtin(webview, "__hv.state", hv_state_bind_cb);
    hv_bind_builtin(webview, "__hv.drag", hv_drag_bind_cb);

    /* WCO close button -> WindowCloseRequested event -> WM_CLOSE (see
     * hv_bind_window_close_requested; the built-in caption buttons never send
     * WM_CLOSE to the host on their own) */
    hv_bind_window_close_requested(webview);

    /* apply virtual-host folder mappings queued before init completes (they
     * must exist before the queued navigation) */
    for (const auto& [host, folder] : webview->pending_mappings) {
        Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
        if (SUCCEEDED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview3))))
            webview3->SetVirtualHostNameToFolderMapping(
                utf8_to_wide(host).c_str(), utf8_to_wide(folder).c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }
    webview->pending_mappings.clear();

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
}

/* ================= WebView engine version / availability =================
 *
 * GetAvailableCoreWebView2BrowserVersionString is the documented way to detect
 * an installed WebView2 Runtime (or a preview Edge channel): it returns a null
 * version when neither is present. Used both by the public query and to fail
 * creation with HELIOSVIEW_ERROR_UNSUPPORTED instead of an opaque HRESULT. */
static std::wstring hv_webview2_runtime_version()
{
    LPWSTR version = nullptr;
    std::wstring out;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version && *version)
        out = version;
    if (version)
        CoTaskMemFree(version);
    return out;
}

int heliosview_webview_engine_version(char* buf, size_t size)
{
    if (!buf || size == 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "buf is NULL or size is 0");
    buf[0] = '\0';
    const std::wstring version = hv_webview2_runtime_version();
    if (!version.empty()) {
        const std::string utf8 = wide_to_utf8(version);
        std::snprintf(buf, size, "%s", utf8.c_str());
    }
    return 0;
}

heliosview_webview_t* heliosview_webview_create_ex(heliosview_window_t* parent,
                                                   const heliosview_webview_env_opts_t* opts)
{
    const HWND parent_hwnd = hv_window_hwnd(parent);
    if (!parent_hwnd) {
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "parent window is NULL or its native window is not created");
        return nullptr;
    }

    /* Fixed-version runtime folder (NULL/"" = the system WebView2 Runtime). */
    const std::wstring browser_folder =
        (opts && opts->browser_executable_folder && *opts->browser_executable_folder)
            ? utf8_to_wide(opts->browser_executable_folder)
            : std::wstring();

    /* The WebView2 Runtime is a deploy-time component: preinstalled on Windows
     * 11, present on most but not all Windows 10 devices. Report the honest
     * "unsupported here" instead of an opaque HRESULT — but only when the caller
     * did not pin a fixed-version folder, whose presence only the OS can tell. */
    if (browser_folder.empty() && hv_webview2_runtime_version().empty()) {
        hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED,
                "WebView2 Runtime is not installed — install the Evergreen Runtime, or pass a "
                "fixed-version folder in heliosview_webview_env_opts_t::browser_executable_folder");
        return nullptr;
    }

    /* WebView2 requires a COM STA apartment on the calling thread (the UI
     * thread): the environment/controller completion callbacks are dispatched
     * on this thread's message loop. Initialize once and keep the apartment
     * for the process lifetime (WebView objects outlive this call; repeated
     * calls return S_FALSE = already initialized). */
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (co == RPC_E_CHANGED_MODE) {
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "thread is already in an MTA apartment; WebView2 requires STA");
        return nullptr; /* the thread is already MTA: WebView2 needs STA */
    }
    if (FAILED(co)) {
        hv_fail_hresult(co, "CoInitializeEx failed");
        return nullptr;
    }

    auto* webview = hv::hv_alloc<heliosview_webview>();
    webview->parent = parent_hwnd;
    /* Attach the parent-window subclass NOW: from this point the WebView keeps
     * itself in sync with its parent (WM_SIZE → put_Bounds / put_IsVisible)
     * through the subclass callout — the window layer stays completely
     * WebView2-free, and no HWND → WebView registry is needed (a running
     * callout proves the object is alive). */
    SetWindowSubclass(parent_hwnd, hv_webview_subclass_proc, 0, (DWORD_PTR)webview);
    webview->creating = true;
    /* Creation-locked environment options: captured here (before the WebView2
     * environment is created); they cannot be changed afterwards. */
    if (opts && opts->user_data_folder && *opts->user_data_folder)
        webview->user_data_folder = utf8_to_wide(opts->user_data_folder);

    /* hv_alloc + Release: hand the initial reference to the API (Release to zero deletes it when done) */
    auto* env_handler = hv::hv_alloc<env_completed_handler>(
        [webview](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
                webview->creating = false;
                return result;
            }
            /* controller ready: hand off to hv_webview_init_core (all wiring
             * and initial state lives there, so the lambdas stay flat) */
            auto* controller_handler = hv::hv_alloc<controller_completed_handler>(
                [webview](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    webview->creating = false;
                    if (FAILED(result) || !controller)
                        return result;
                    webview->controller = controller;
                    controller->get_CoreWebView2(&webview->webview);
                    return hv_webview_init_core(webview);
                });
            const HRESULT hr = env->CreateCoreWebView2Controller(webview->parent, controller_handler);
            controller_handler->Release();
            return hr;
        });
    const Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> env_options =
        hv_build_env_options(opts);
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        browser_folder.empty() ? nullptr : browser_folder.c_str(),
        webview->user_data_folder.empty() ? nullptr : webview->user_data_folder.c_str(),
        env_options.Get(), env_handler);
    env_handler->Release();

    if (FAILED(hr)) {
        RemoveWindowSubclass(webview->parent, hv_webview_subclass_proc, 0); /* undo the subclass above */
        hv::hv_dealloc(webview);
        /* A fixed-version folder that does not exist (or a runtime removed
         * between the check above and here) surfaces as a "not found" HRESULT:
         * report it as the missing engine, not as an opaque platform code. */
        if (browser_folder.empty()
            && (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
                || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)))
            hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "WebView2 Runtime is not installed");
        else
            hv_fail_hresult(hr, "CreateCoreWebView2EnvironmentWithOptions failed");
        return nullptr;
    }
    hv_webview_register_live(webview); /* track the instance for stale-call detection */
    return webview;
}

heliosview_webview_t* heliosview_webview_create(heliosview_window_t* parent)
{
    return heliosview_webview_create_ex(parent, nullptr);
}

void heliosview_webview_destroy(heliosview_webview_t* webview)
{
    /* Must be called on the thread that created the WebView (the UI thread):
     * the WebView2 controller and the bound userdata dtors are released here. */
    if (!webview)
        return;
    /* Transition the instance to destroyed FIRST: from here on, any late
     * resolve/reject/broadcast on this pointer returns kWebviewDestroyedError
     * instead of dereferencing freed memory. A second destroy is also caught
     * here (the instance is no longer live): it becomes a safe no-op. */
    if (!hv_webview_mark_destroyed(webview))
        return; /* double destroy or never-registered instance: nothing to tear down */
    /* Detach the parent-window subclass FIRST: a queued WM_SIZE can no longer
     * reach this WebView — the subclass callout stops running (this is what
     * makes "callout running == object alive" hold). */
    if (webview->parent)
        RemoveWindowSubclass(webview->parent, hv_webview_subclass_proc, 0);
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
    if (webview->webview && webview->context_menu_token.value != 0) {
        Microsoft::WRL::ComPtr<ICoreWebView2_11> webview11;
        if (SUCCEEDED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview11))))
            webview11->remove_ContextMenuRequested(webview->context_menu_token);
    }
    if (webview->context_menu_dtor)
        webview->context_menu_dtor(webview->context_menu_userdata);
    webview->context_menu_dtor = nullptr;
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
        return hv_fail(-1, "webview or url is NULL");
    if (!webview->ready) {
        webview->pending_text = url; /* queued: run after init completes (last one wins) */
        webview->pending_html = false;
        webview->has_pending = true;
        return 0;
    }
    const std::wstring wurl = utf8_to_wide(url);
    const HRESULT hr = webview->webview->Navigate(wurl.c_str());
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "Navigate failed");
}

int heliosview_webview_navigate_html(heliosview_webview_t* webview, const char* html)
{
    if (!webview || !html)
        return hv_fail(-1, "webview or html is NULL");
    if (!webview->ready) {
        webview->pending_text = html; /* queued: run after init completes (last one wins) */
        webview->pending_html = true;
        webview->has_pending = true;
        return 0;
    }
    const std::wstring whtml = utf8_to_wide(html);
    const HRESULT hr = webview->webview->NavigateToString(whtml.c_str());
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "NavigateToString failed");
}

int heliosview_webview_bind(heliosview_webview_t* webview, const char* name,
                            heliosview_webview_bind_cb callback, void* userdata,
                            heliosview_webview_userdata_dtor dtor)
{
    if (!webview || !name || !callback)
        return hv_fail(-1, "webview, name or callback is NULL");
    /* names must be C identifiers ([A-Za-z_][A-Za-z0-9_]*): this is both the
     * binding convention and what keeps the wire header separator-safe. The
     * library's internal bridge names ("__hv.*", see hv_internal_name) contain a
     * dot and therefore fail this check — they cannot be bound by applications. */
    if (!hv_valid_name(name))
        return hv_fail(-2, "name is not a valid C identifier");
    /* bindings are owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    auto it = webview->bindings.find(name);
    if (it != webview->bindings.end() && it->second.dtor)
        it->second.dtor(it->second.userdata); /* replacing an existing binding */
    webview->bindings[name] = hv_webview_binding{callback, userdata, dtor};
    return 0;
}

int heliosview_webview_resolve(heliosview_webview_t* webview, uint64_t call_id, const char* result_json)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* The WebView may already be destroyed (e.g. destroyWebView() ran while this
     * asynchronous call was in flight). Detect that BEFORE touching the (freed)
     * instance and return the documented stale-instance error instead of an
     * access violation — the caller learns exactly what went wrong. */
    if (hv_webview_state(webview) != hv_webview_slot_state::live)
        return hv_fail(kWebviewDestroyedError, "WebView instance was already destroyed");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    std::string s = "HV\tresolve\t" + std::to_string(call_id) + "\r\n\r\n"
                  + (result_json ? result_json : "null");
    hv_post_string(webview, s);
    return 0;
}

int heliosview_webview_reject(heliosview_webview_t* webview, uint64_t call_id, const char* error_json)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* Same stale-instance guard as resolve: reject must never touch a destroyed
     * WebView (returns kWebviewDestroyedError, see heliosview_webview_resolve). */
    if (hv_webview_state(webview) != hv_webview_slot_state::live)
        return hv_fail(kWebviewDestroyedError, "WebView instance was already destroyed");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    std::string s = "HV\treject\t" + std::to_string(call_id) + "\r\n\r\n"
                  + (error_json ? error_json : "{}");
    hv_post_string(webview, s);
    return 0;
}

/* Send a script to the page's shim for evaluation (awaits promises) and, when
 * `callback` is set, record it against a fresh eval id. Returns 0 on success. */
int eval_via_shim(heliosview_webview_t* webview, const char* script,
                  heliosview_webview_eval_cb callback, void* userdata)
{
    const uint64_t id = webview->next_eval_id++;
    if (callback)
        webview->pending_evals[id] = {callback, userdata};
    std::string envelope = "HV\teval\t";
    envelope += std::to_string(id);
    envelope += "\r\n\r\n";
    envelope += script;
    hv_post_string(webview, envelope);
    return 0;
}

int heliosview_webview_eval(heliosview_webview_t* webview, const char* script)
{
    if (!webview || !script)
        return hv_fail(-1, "webview or script is NULL");
    if (!webview->ready) {
        webview->pending_ops.push_back({script, false, nullptr, nullptr});
        return 0;
    }
    if (webview->shim_ready)
        return eval_via_shim(webview, script, nullptr, nullptr);
    const std::wstring wscript = utf8_to_wide(script);
    const HRESULT hr = webview->webview->ExecuteScript(wscript.c_str(), nullptr);
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "ExecuteScript failed");
}

int heliosview_webview_eval_async(heliosview_webview_t* webview, const char* script,
                                  heliosview_webview_eval_cb callback, void* userdata)
{
    if (!webview || !script || !callback)
        return hv_fail(-1, "webview, script or callback is NULL");
    if (!webview->ready) {
        webview->pending_ops.push_back({script, true, callback, userdata});
        return 0;
    }
    /* The shim awaits promises; ExecuteScript (fallback, used before the first
     * document exists) does not. */
    if (webview->shim_ready)
        return eval_via_shim(webview, script, callback, userdata);
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
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "ExecuteScript failed");
}

int heliosview_webview_broadcast(heliosview_webview_t* webview, const char* name, const char* data_json)
{
    if (!webview || !name)
        return hv_fail(-1, "webview or name is NULL");
    if (!hv_valid_name(name))
        return hv_fail(-2, "name is not a valid C identifier"); /* would break the wire header */
    /* Same stale-instance guard as resolve: broadcast must never touch a
     * destroyed WebView (returns kWebviewDestroyedError, see
     * heliosview_webview_resolve). */
    if (hv_webview_state(webview) != hv_webview_slot_state::live)
        return hv_fail(kWebviewDestroyedError, "WebView instance was already destroyed");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
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
        return hv_fail(-1, "webview, name or callback is NULL");
    /* names must be C identifiers, same rule as bind (keeps the wire header
     * separator-safe and consistent with the function-naming convention); the
     * internal "__hv.*" bridge names (hv_internal_name) fail this check and
     * cannot be subscribed by applications. */
    if (!hv_valid_name(name))
        return hv_fail(-2, "name is not a valid C identifier");
    /* subscriptions are owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    auto it = webview->subscriptions.find(name);
    if (it != webview->subscriptions.end() && it->second.dtor)
        it->second.dtor(it->second.userdata); /* replacing an existing subscription */
    webview->subscriptions[name] = hv_webview_subscription{callback, userdata, dtor};
    return 0;
}

int heliosview_webview_unsubscribe(heliosview_webview_t* webview, const char* name)
{
    if (!webview || !name)
        return hv_fail(-1, "webview or name is NULL");
    /* subscriptions are owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
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
        return hv_fail(-1, "webview is NULL");
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
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
        return hv_fail(-1, "webview is NULL");
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
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
        return hv_fail(-1, "webview is NULL");
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
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
        return hv_fail(-1, "webview is NULL");
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    if (webview->title_dtor)
        webview->title_dtor(webview->title_userdata); /* replacing an existing callback */
    webview->title_cb = callback;
    webview->title_userdata = userdata;
    webview->title_dtor = dtor;
    return 0;
}

int heliosview_webview_last_native_error(heliosview_webview_t* webview)
{
    return webview ? webview->last_native_error : 0;
}

int heliosview_webview_local_url(heliosview_webview_t* webview, const char* host_name,
                                 const char* path, char* buf, size_t size)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (!host_name || !*host_name)
        return hv_fail(-2, "host_name is NULL or empty");
    if (!buf || size == 0)
        return hv_fail(-2, "buf is NULL or size is 0");
    /* WebView2 virtual-host mappings are https origins; the host must use the
     * .local suffix to be a trusted origin (see heliosview_webview_map_local_folder). */
    std::string url = "https://";
    url += host_name;
    if (path && *path) {
        if (*path != '/')
            url += '/';
        url += path;
    }
    std::snprintf(buf, size, "%s", url.c_str());
    return 0;
}

int heliosview_webview_map_local_folder(heliosview_webview_t* webview,
                                        const char* host_name,
                                        const char* folder_path)
{
    if (!webview || !host_name || !folder_path)
        return hv_fail(-1, "webview, host_name or folder_path is NULL");
    /* virtual host mappings are owned by the UI thread (require the controller) */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    /* Not initialized yet: queue the mapping, applied when the core becomes
     * ready (before the queued navigation runs), like navigate() does. */
    if (!webview->webview) {
        webview->pending_mappings.emplace_back(host_name, folder_path);
        return 0;
    }
    /* SetVirtualHostNameToFolderMapping lives on ICoreWebView2_3 (WebView2 SDK
     * 1.0.1293.44+); the runtime supports it, but the interface must be queried
     * from the base interface */
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
    const HRESULT hr_qi = webview->webview->QueryInterface(IID_PPV_ARGS(&webview3));
    if (FAILED(hr_qi))
        return hv_fail_hresult(hr_qi, "QueryInterface(ICoreWebView2_3) failed");
    const HRESULT hr = webview3->SetVirtualHostNameToFolderMapping(
        utf8_to_wide(host_name).c_str(), utf8_to_wide(folder_path).c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "SetVirtualHostNameToFolderMapping failed");
}

int heliosview_webview_set_insets(heliosview_webview_t* webview,
                                  int32_t top, int32_t right,
                                  int32_t bottom, int32_t left)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* The WebView2 controller is UI-thread bound (put_Bounds included) */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->inset_top = top > 0 ? top : 0;
    webview->inset_right = right > 0 ? right : 0;
    webview->inset_bottom = bottom > 0 ? bottom : 0;
    webview->inset_left = left > 0 ? left : 0;
    webview->has_insets = true;
    if (auto* controller = webview->controller.Get())
        controller->put_Bounds(hv_webview_rect(webview));
    return 0;
}

int heliosview_webview_set_status_bar(heliosview_webview_t* webview, int enabled)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* ICoreWebView2Settings is UI-thread bound */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->status_bar_enabled = enabled != 0;
    /* Settings are only reachable once the core is initialized; the create
     * path applies the stored value when it becomes ready. */
    if (webview->webview) {
        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        const HRESULT hr_settings = webview->webview->get_Settings(&settings);
        if (FAILED(hr_settings))
            return hv_fail_hresult(hr_settings, "ICoreWebView2Settings lookup failed");
        settings->put_IsStatusBarEnabled(webview->status_bar_enabled ? TRUE : FALSE);
    }
    return 0;
}

int heliosview_webview_set_context_menu(heliosview_webview_t* webview, int enabled)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->context_menu_enabled = enabled != 0;
    /* No WebView2 call needed: the always-registered ContextMenuRequested handler
     * consults the flag whenever the menu is about to open. Suppressing needs
     * ICoreWebView2_11 (runtime 100+): report that once the core is up so the app
     * can fall back to a menu drawn in the page (before init the check is
     * deferred; the handler then finds no interface and the engine menu shows). */
    if (webview->context_menu_enabled)
        return 0;
    if (webview->webview) {
        Microsoft::WRL::ComPtr<ICoreWebView2_11> webview11;
        if (FAILED(webview->webview->QueryInterface(IID_PPV_ARGS(&webview11))))
            return hv_fail(-4, "this WebView2 runtime cannot suppress the context menu (needs 100+)");
    }
    return 0;
}

int heliosview_webview_set_context_menu_callback(heliosview_webview_t* webview,
                                                 heliosview_webview_context_menu_cb callback,
                                                 void* userdata,
                                                 heliosview_webview_userdata_dtor dtor)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* the callback table is owned by the UI thread */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    if (webview->context_menu_dtor)
        webview->context_menu_dtor(webview->context_menu_userdata); /* replacing an existing callback */
    webview->context_menu_cb = callback;
    webview->context_menu_userdata = userdata;
    webview->context_menu_dtor = dtor;
    return 0;
}

int heliosview_webview_set_devtools(heliosview_webview_t* webview, int enabled)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->devtools_enabled = enabled != 0;
    /* Settings are only reachable once the core is initialized; the create
     * path applies the stored value when it becomes ready. Disabling also
     * closes an already-open DevTools window. */
    if (webview->webview) {
        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        const HRESULT hr_settings = webview->webview->get_Settings(&settings);
        if (FAILED(hr_settings))
            return hv_fail_hresult(hr_settings, "ICoreWebView2Settings lookup failed");
        settings->put_AreDevToolsEnabled(webview->devtools_enabled ? TRUE : FALSE);
    }
    return 0;
}

int heliosview_webview_set_window_controls_overlay(heliosview_webview_t* webview, int enabled)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->window_controls_enabled = enabled != 0;
    if (!webview->controller.Get())
        return 0; /* applied when the core becomes ready */
    return hv_webview_apply_window_controls(webview);
}

int heliosview_webview_set_window_controls_background_color(heliosview_webview_t* webview,
                                                            uint8_t red, uint8_t green,
                                                            uint8_t blue, uint8_t alpha)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    /* COREWEBVIEW2_COLOR stores channels as (alpha, red, green, blue) */
    webview->window_controls_background_color = COREWEBVIEW2_COLOR{alpha, red, green, blue};
    if (!webview->window_controls_overlay)
        return 0; /* applied when the overlay is created/enabled */
    const HRESULT hr = webview->window_controls_overlay->put_BackgroundColor(webview->window_controls_background_color);
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "put_BackgroundColor (window controls overlay) failed");
}

int heliosview_webview_suspend(heliosview_webview_t* webview,
                               heliosview_webview_suspend_cb callback, void* userdata)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* the controller is UI-thread bound */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->suspended = true;
    if (!webview->webview) {
        /* not initialized yet: record the request (and the completion
         * callback, last one wins); applied when the core becomes ready */
        if (callback) {
            webview->suspend_cb = callback;
            webview->suspend_userdata = userdata;
        }
        return 0;
    }
    return hv_webview_try_suspend(webview, callback, userdata);
}

int heliosview_webview_resume(heliosview_webview_t* webview)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    webview->suspended = false;
    /* drop a pending pre-init suspend request (and its completion callback) */
    webview->suspend_cb = nullptr;
    webview->suspend_userdata = nullptr;
    if (!webview->webview)
        return 0; /* nothing initialized yet: nothing to resume */
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
    const HRESULT hr_qi = webview->webview->QueryInterface(IID_PPV_ARGS(&webview3));
    if (FAILED(hr_qi))
        return hv_fail_hresult(hr_qi, "QueryInterface(ICoreWebView2_3) failed");
    const HRESULT hr = webview3->Resume();
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "Resume failed");
}

int heliosview_webview_is_suspended(heliosview_webview_t* webview, int* out_suspended)
{
    if (!webview || !out_suspended)
        return hv_fail(-1, "webview or out_suspended is NULL");
    /* tracked state: true from the moment a suspend is requested, confirmed
     * when the TrySuspend attempt completes; false again after resume() */
    *out_suspended = webview->suspended ? 1 : 0;
    return 0;
}

int heliosview_webview_set_background_color(heliosview_webview_t* webview,
                                            uint8_t red, uint8_t green,
                                            uint8_t blue, uint8_t alpha)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    /* the controller is UI-thread bound */
    if (GetCurrentThreadId() != webview->ui_thread)
        return hv_fail(-1, "webview API called from a non-UI thread");
    /* COREWEBVIEW2_COLOR stores channels as (alpha, red, green, blue) */
    webview->background_color = COREWEBVIEW2_COLOR{alpha, red, green, blue};
    if (!webview->controller.Get())
        return 0; /* applied when the core becomes ready */
    Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
    const HRESULT hr_ctrl2 = webview->controller.As(&controller2);
    if (FAILED(hr_ctrl2))
        return -static_cast<int>(hr_ctrl2);
    const HRESULT hr = controller2->put_DefaultBackgroundColor(webview->background_color);
    return SUCCEEDED(hr) ? 0 : hv_fail_hresult(hr, "put_DefaultBackgroundColor failed");
}

int heliosview_webview_set_transparent_background(heliosview_webview_t* webview, int transparent)
{
    if (!webview)
        return hv_fail(-1, "webview is NULL");
    if (transparent)
        return heliosview_webview_set_background_color(webview, 0, 0, 0, 0);
    return heliosview_webview_set_background_color(webview, 255, 255, 255, 255);
}