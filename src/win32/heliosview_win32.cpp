// HeliosView.dll — Windows implementation: windows, message loop, native-message → event conversion.
// The cross-platform interface is in heliosview.h; this file implements only the win32 side.
#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <wrl/client.h> /* ComPtr */
#include <WebView2.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <flat_map>
#include <functional>
#include <map>
#include <memory_resource>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

/* flat_map backed by std::pmr::vector, so a WebView's binding/subscription tables
 * allocate through the default PMR resource (set in main, after these runtime
 * objects are created). The global tables below keep std::vector: as statics they
 * are constructed before main, so a PMR default set later would not apply. */
template <class K, class V>
using hv_pmr_flat_map = std::flat_map<K, V, std::less<K>,
                                      std::pmr::vector<K>, std::pmr::vector<V>>;

/* ================= Window (completes the header's opaque declaration; must be at global scope) ================= */

struct heliosview_window {
    int32_t id = 0;
    int width = 0;
    int height = 0;
    std::string title; /* UTF-8 */
    heliosview_window_style_t style = HELIOSVIEW_WINDOW_NORMAL;
    HWND hwnd = nullptr;
    void* userdata = nullptr; /* caller data (the C++ wrapper stores an object pointer) */
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

    /* JS <-> native bridge */
    DWORD ui_thread = GetCurrentThreadId();            /* thread that created the webview */
    EventRegistrationToken message_token{};            /* JS -> native messages */
    hv_pmr_flat_map<std::string, hv_webview_binding> bindings; /* name -> binding (UI thread only) */
    hv_pmr_flat_map<std::string, hv_webview_subscription> subscriptions; /* BroadcastChannel name -> subscription (UI thread only) */

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

/* JSON string value for an error payload: {"error":<msg>} with quoting */
std::string json_quote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

/* Post a bridge envelope to the JS page (must run on the UI thread) */
void hv_post_json(heliosview_webview_t* wv, const std::string& json)
{
    if (wv && wv->webview && wv->ready) {
        const std::wstring w = utf8_to_wide(json);
        wv->webview->PostWebMessageAsJson(w.c_str());
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

/* The JS shim, injected into every document. Pure JS, no escaping needed (it is
 * inserted via AddScriptToExecuteOnDocumentCreated which takes raw script text).
 * window.helios.call invokes native functions; BroadcastChannel is subclassed so
 * native broadcasts (heliosview_webview_broadcast) dispatch synthetic message
 * events, and page postMessage()s are forwarded to native subscriptions
 * (heliosview_webview_subscribe) while still going to other same-origin tabs. */
const char* kWebView2BridgeScript = R"JS(
(function () {
  'use strict';
  if (window.__hvShim) return;        /* installed already (subframe/navigation) */
  window.__hvShim = 1;

  var pending = new Map();            /* id -> {resolve, reject} */
  var seq = 0;

  function post(obj) { window.chrome.webview.postMessage(obj); }

  function recv(e) {
    var m = e.data;
    if (typeof m === 'string') { try { m = JSON.parse(m); } catch (err) { return; } }
    if (!m || m.__hv !== 1) return;
    if (m.kind === 'resolve' || m.kind === 'reject') {
      var p = pending.get(m.id);
      if (!p) return;
      pending.delete(m.id);
      if (m.kind === 'resolve') p.resolve(m.result);
      else p.reject(new Error(JSON.stringify(m.error)));
    } else if (m.kind === 'broadcast') {
      dispatchBC(m.name, m.data);
    }
  }

  window.chrome.webview.addEventListener('message', recv);

  window.helios = {
    call: function (name) {
      var args = Array.prototype.slice.call(arguments, 1);
      return new Promise(function (resolve, reject) {
        var id = ++seq;
        pending.set(id, { resolve: resolve, reject: reject });
        post({ __hv: 1, kind: 'call', id: id, name: name, args: args });
      });
    }
  };

  /* BroadcastChannel: keep the native broadcast and the standard same-origin
     channel working together by subclassing. A native broadcast dispatches a
     synthetic MessageEvent on matching instances; a page postMessage is forwarded
     to native (subscribe) and still delivered to the other same-origin tabs. */
  var NativeBC = window.BroadcastChannel;
  var live = new Set();
  function dispatchBC(name, data) {
    live.forEach(function (ch) {
      if (ch._hvName === name)
        ch.dispatchEvent(new MessageEvent('message', { data: data }));
    });
  }
  window.BroadcastChannel = function (name) {
    var ch = new NativeBC(name);
    ch._hvName = name;
    live.add(ch);
    var origPost = ch.postMessage.bind(ch);
    var origClose = ch.close.bind(ch);
    ch.postMessage = function (data) {
      post({ __hv: 1, kind: 'broadcast', name: name, data: data === undefined ? null : data });
      return origPost(data);
    };
    ch.close = function () { live.delete(ch); return origClose(); };
    return ch;
  };
  window.BroadcastChannel.prototype = NativeBC.prototype;
})();
)JS";

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

/* Preset style → Win32 window style */
DWORD map_win32_style(heliosview_window_style_t style)
{
    switch (style) {
    case HELIOSVIEW_WINDOW_NORMAL:
        return WS_OVERLAPPEDWINDOW;
    case HELIOSVIEW_WINDOW_BORDERLESS:
        return WS_POPUP; /* borderless and titleless; fully custom */
    case HELIOSVIEW_WINDOW_FRAMELESS:
        /* resizable border, no title bar: the classic combo for custom-title-bar windows */
        return WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    default:
        return WS_OVERLAPPEDWINDOW;
    }
}

namespace {

/* Event object that wakes the message loop's wait (triggered by post_event / quit via hv::g_platform_wake) */
HANDLE g_wakeup_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

/* Register the platform wake callback with the cross-platform core (static init, before main) */
const bool g_wake_registered = (hv::g_platform_wake = [] { SetEvent(g_wakeup_event); }, true);

std::atomic<int32_t> g_next_window_id{1};
std::flat_map<HWND, heliosview_window_t*> g_windows_by_hwnd;
std::flat_map<int32_t, heliosview_window_t*> g_windows_by_id; /* event dispatch: id → window */
std::flat_map<HWND, heliosview_webview_t*> g_webviews_by_hwnd; /* resize WebViews together with their window */

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

    switch (msg->message) {
    case WM_CLOSE:
        out->type = HELIOSVIEW_EVENT_WINDOW_CLOSE;
        out->timestamp_ms = ts;
        return 1;
    case WM_SIZE:
        out->type = HELIOSVIEW_EVENT_WINDOW_RESIZE;
        out->width = static_cast<int32_t>(LOWORD(msg->lParam));
        out->height = static_cast<int32_t>(HIWORD(msg->lParam));
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

LRESULT CALLBACK heliosview_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    /* WM_NCCREATE is sent synchronously during CreateWindowExW: register here so
     * messages such as WM_SIZE delivered during creation get the correct window_id */
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* win = static_cast<heliosview_window_t*>(cs->lpCreateParams);
        if (win) {
            win->hwnd = hwnd;
            g_windows_by_hwnd[hwnd] = win;
        }
        return DefWindowProc(hwnd, message, wparam, lparam);
    }

    /* WebViews attached to this window resize along with it */
    if (message == WM_SIZE) {
        auto range = g_webviews_by_hwnd.equal_range(hwnd);
        for (auto it = range.first; it != range.second; ++it) {
            if (auto* controller = it->second->controller.Get()) {
                RECT rc{0, 0, LOWORD(lparam), HIWORD(lparam)};
                controller->put_Bounds(rc);
            }
        }
    }

    /* Marshalled webview tasks (cross-thread resolve/reject/broadcast).
     * Looked up by hwnd: if the webview was destroyed, the entry is gone and the
     * queued message is a no-op (never touches a freed heliosview_webview_t). */
    if (message == WM_HV_WEBVIEW_MSG) {
        auto it = g_webviews_by_hwnd.find(hwnd);
        if (it != g_webviews_by_hwnd.end())
            hv_drain_ui_tasks(it->second);
        return 0;
    }

    const auto window_id_of = [hwnd] {
        auto it = g_windows_by_hwnd.find(hwnd);
        return it != g_windows_by_hwnd.end() ? it->second->id : 0;
    };

    MSG native{};
    native.hwnd = hwnd;
    native.message = message;
    native.wParam = wparam;
    native.lParam = lparam;

    /* 1. user-registered conversion delegate (optional) */
    heliosview_event_t event{};
    if (hv::g_native_handler) {
        const int handled = hv::g_native_handler(&native, &event);
        if (handled == 1) {
            event.window_id = window_id_of();
            hv::queue_push(event);
            return 0;
        }
        if (handled == 0)
            return 0;
    }

    /* 2. library default conversion (WM_CLOSE/WM_SIZE/keyboard/mouse, etc.) */
    const int def = default_native_convert(&native, &event);
    if (def == 1) {
        event.window_id = window_id_of();
        hv::queue_push(event);
        return 0; /* consume the message: whether the window is destroyed is up to the app */
    }
    if (def == 0)
        return 0;

    return DefWindowProc(hwnd, message, wparam, lparam);
}

} // namespace

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
    window->id = g_next_window_id.fetch_add(1);
    window->width = width;
    window->height = height;
    window->title = title;
    window->style = style;
    window->userdata = userdata;
    g_windows_by_id[window->id] = window;
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

heliosview_window_t* heliosview_window_from_id(int32_t window_id)
{
    auto it = g_windows_by_id.find(window_id);
    return it != g_windows_by_id.end() ? it->second : nullptr;
}

int heliosview_window_count(void)
{
    return static_cast<int>(g_windows_by_id.size());
}

void heliosview_window_destroy(heliosview_window_t* window)
{
    if (!window)
        return;
    g_windows_by_id.erase(window->id);
    if (window->hwnd) {
        g_windows_by_hwnd.erase(window->hwnd);
        DestroyWindow(window->hwnd); /* triggers WM_DESTROY → PostQuitMessage → message loop exits */
    }
    hv::hv_dealloc(window);
}

int heliosview_window_show(heliosview_window_t* window)
{
    if (!window)
        return -1;
    if (window->hwnd) {
        ShowWindow(window->hwnd, SW_SHOW);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = heliosview_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"HeliosViewWindow";
    RegisterClassExW(&wc); /* re-registering is harmless (silently fails if the class exists) */

    const std::wstring title = utf8_to_wide(window->title);

    /* compute the window size for the preset style (AdjustWindowRect is the identity for borderless styles) */
    const DWORD style = map_win32_style(window->style);
    RECT rect{0, 0, window->width, window->height};
    AdjustWindowRect(&rect, style, FALSE);

    window->hwnd = CreateWindowExW(0, L"HeliosViewWindow", title.c_str(), style,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   rect.right - rect.left, rect.bottom - rect.top,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), window);
    if (!window->hwnd)
        return -2;

    /* already registered in WM_NCCREATE; fall back here if that did not happen */
    if (g_windows_by_hwnd.find(window->hwnd) == g_windows_by_hwnd.end())
        g_windows_by_hwnd[window->hwnd] = window;
    ShowWindow(window->hwnd, SW_SHOW);
    return 0;
}

int32_t heliosview_window_id(const heliosview_window_t* window)
{
    return window ? window->id : 0;
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
    AdjustWindowRect(&rc, map_win32_style(window->style), FALSE);
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
    const std::wstring wtitle = utf8_to_wide(title);
    window->title = title;
    return SetWindowTextW(window->hwnd, wtitle.c_str()) ? 0 : -1;
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

/* ================= WebView (WebView2) ================= */

/* UI-thread task draining for cross-thread resolve/reject/broadcast.
 * WM_HV_WEBVIEW_MSG is posted to the parent HWND; the WndProc drains it. */

namespace {

/* Parse the JS->native call envelope {__hv, kind, id, name, args} posted by the
 * JS shim. Returns true for a "call" message, filling id/name/args. */
bool parse_call_envelope(const std::string& msg, uint64_t& id, std::string& name, std::string& args)
{
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(msg);
        if (!env.is_object() || env.value("__hv", 0) != 1 || env.value("kind", "") != "call")
            return false;

        id = env.value("id", 0ull);
        name = env.value("name", "");
        /* args as raw JSON text, so the native side still gets the original string */
        args = env.contains("args") ? env["args"].dump() : "[]";
        return !name.empty();
    } catch (...) {
        /* page input: any unexpected shape is not a call; never throw into the COM callback */
        return false;
    }
}

/* Parse the JS->native broadcast envelope {__hv, kind:'broadcast', name, data} posted by
 * the shim's BroadcastChannel.postMessage wrapper. Returns true for a "broadcast"
 * message, filling name and the raw JSON text of the posted value. */
bool parse_broadcast_envelope(const std::string& msg, std::string& name, std::string& data)
{
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(msg);
        if (!env.is_object() || env.value("__hv", 0) != 1 || env.value("kind", "") != "broadcast")
            return false;

        name = env.value("name", "");
        /* data as raw JSON text, so the native side still gets the original string */
        data = env.contains("data") ? env["data"].dump() : "";
        return !name.empty();
    } catch (...) {
        /* page input: any unexpected shape is not a broadcast; never throw into the COM callback */
        return false;
    }
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

} // namespace

heliosview_webview_t* heliosview_webview_create(heliosview_window_t* parent)
{
    if (!parent || !parent->hwnd)
        return nullptr;

    auto* webview = hv::hv_alloc<heliosview_webview>();
    webview->parent = parent->hwnd;
    g_webviews_by_hwnd[parent->hwnd] = webview;
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
                    RECT rc{};
                    GetClientRect(webview->parent, &rc);
                    controller->put_Bounds(rc);

                    /* JS -> native messaging */
                    auto* msg_handler = hv::hv_alloc<web_message_received_handler>(
                        [webview](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            (void)sender;
                            LPWSTR raw = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
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
        g_webviews_by_hwnd.erase(webview->parent);
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
    g_webviews_by_hwnd.erase(webview->parent);
    if (webview->webview && webview->message_token.value != 0)
        webview->webview->remove_WebMessageReceived(webview->message_token);
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
    std::string json = "{\"__hv\":1,\"kind\":\"resolve\",\"id\":" + std::to_string(call_id)
                     + ",\"result\":" + (result_json ? result_json : "null") + "}";
    hv_post_json(webview, json);
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
    std::string json = "{\"__hv\":1,\"kind\":\"reject\",\"id\":" + std::to_string(call_id)
                     + ",\"error\":" + (error_json ? error_json : "{}") + "}";
    hv_post_json(webview, json);
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
    if (GetCurrentThreadId() != webview->ui_thread) {
        hv_ui(webview, [wv = webview, name = std::string(name),
                        data = std::string(data_json ? data_json : "null")] {
            heliosview_webview_broadcast(wv, name.c_str(), data.c_str());
        });
        return 0;
    }
    std::string json = "{\"__hv\":1,\"kind\":\"broadcast\",\"name\":" + json_quote(name)
                     + ",\"data\":" + (data_json ? data_json : "null") + "}";
    hv_post_json(webview, json);
    return 0;
}

int heliosview_webview_subscribe(heliosview_webview_t* webview, const char* name,
                                 heliosview_webview_subscribe_cb callback, void* userdata,
                                 heliosview_webview_userdata_dtor dtor)
{
    if (!webview || !name || !callback)
        return -1;
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
