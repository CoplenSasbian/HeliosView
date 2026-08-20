#pragma once

/**
 * HeliosView.Core -- WebViewWindow: a window with an embedded WebView.
 *
 * Inherits Window; on win32 it is built on WebView2 (Edge Chromium, SDK pulled in via FetchContent).
 * The WebView is a standalone handle (heliosview_webview_t) attached to its parent window
 * at creation; afterwards, operations involve only the WebView itself.
 * Offers navigation (URL / HTML string) and navigation events (navigationStarting
 * with a veto gate, urlChanged, titleChanged, navigationCompleted).
 *
 * Usage:
 *   helios::WebViewWindow win(900, 640, "title");
 *   win.createWebView();      // async initialization (the native window exists from construction)
 *   win.navigateHtml("<h1>hi</h1>");   // queued automatically if called before init finishes
 *   win.show();
 */

#include <HeliosViewCore/Window.h>

#include <cstdint>
#include <functional>
#include <stdexcept>

namespace helios {

class WebViewWindow : public Window {
public:
    // Construct a window (see Window's constructor); the WebView is created
    // separately via createWebView().
    WebViewWindow(int width, int height, const char* title,
                  WindowStyle style = WindowStyle::Normal)
        : Window(width, height, title, style)
    {
    }

    // Destroy the WebView first (it is attached to the window), then the window
    ~WebViewWindow()
    {
        if (m_webview) {
            heliosview_webview_destroy(m_webview);
            m_webview = nullptr;
        }
    }

    // Move: transfers both the window and the WebView handle
    WebViewWindow(WebViewWindow&& other) noexcept
        : Window(std::move(other))
        , m_webview(other.m_webview)
    {
        other.m_webview = nullptr;
        wireWebViewEvents(); /* the C userdata must point at this new object */
    }

    WebViewWindow& operator=(WebViewWindow&& other) noexcept
    {
        if (this != &other) {
            Window::operator=(std::move(other));
            if (m_webview)
                heliosview_webview_destroy(m_webview);
            m_webview = other.m_webview;
            other.m_webview = nullptr;
            wireWebViewEvents();
        }
        return *this;
    }

    // Non-copyable: a WebViewWindow uniquely owns its WebView
    WebViewWindow(const WebViewWindow&) = delete;
    WebViewWindow& operator=(const WebViewWindow&) = delete;

    // Create and initialize the WebView control in the window's client area.
    // Initialization is asynchronous: navigation requests made before it finishes
    // are queued automatically (the last one wins).
    // The window must stay alive until initialization completes (usually a few ms).
    void createWebView()
    {
        if (!m_webview) {
            m_webview = heliosview_webview_create(nativeHandle());
            wireWebViewEvents();
        }
    }

    // Navigate to a URL (queued automatically if the WebView is still initializing)
    void navigate(const char* url)
    {
        heliosview_webview_navigate(m_webview, url);
    }

    // Load an HTML string and render it as a page
    // (queued automatically if the WebView is still initializing)
    void navigateHtml(const char* html)
    {
        heliosview_webview_navigate_html(m_webview, html);
    }

    // ---- navigation events ----

    // Fired on the UI thread when a new navigation begins (initial load, links,
    // navigate(), browser back/forward, redirects). uri is the target URI;
    // isRedirected / isUserInitiated follow WebView2's NavigationStarting
    // semantics. To cancel a navigation, register a gate via
    // setNavigationStartingGate() (a Signal cannot veto). 
    Signal<std::string, bool, bool> navigationStarting;

    // Fired on the UI thread whenever the WebView's current URL changes. uri is
    // the new source URI; isNewDocument is true when the change comes from a new
    // document load (vs an in-document change such as a fragment/history.pushState).
    Signal<std::string, bool> urlChanged;

    // Fired on the UI thread when the page's <title> changes.
    Signal<std::string> titleChanged;

    // Fired on the UI thread when a page load completes: error == 0 on success,
    // otherwise a negated platform error code (on WebView2: -HRESULT, e.g.
    // -COREWEBVIEW2_WEB_ERROR_STATUS_*). Not fired for navigations that never
    // finish (e.g. aborted). Use it to know when the page is ready for eval()
    // or to show an error state when loading fails.
    Signal<int> navigationCompleted;

    // Register a veto callback for navigations. It runs on the UI thread just
    // before a navigation starts and returns true to cancel it (e.g. to block
    // external links or cross-origin navigations inside the WebView). The gate,
    // if set, receives (uri, isRedirected, isUserInitiated). Only one gate may be
    // registered; calling again replaces the previous one.
    std::function<bool(const std::string&, bool, bool)> navigationStartingGate;

    // ---- WebView layout ----

    // Keep the given insets (client pixels) clear around the WebView: it fills
    // the window's client area minus these insets, and the cleared strips stay
    // the window's own surface. Re-applied on every window resize. Useful when
    // the window keeps native chrome of its own around the WebView (e.g. a
    // header strip drawn by the app). Zero insets restore the default (WebView
    // fills the whole client area). Applies immediately when initialized;
    // otherwise when it becomes ready.
    void setWebViewInsets(int32_t top, int32_t right, int32_t bottom, int32_t left)
    {
        heliosview_webview_set_insets(m_webview, top, right, bottom, left);
    }

    // Show (enabled) or hide the WebView2 status bar, which displays the target
    // URL of a hovered link at the bottom-left corner of the WebView. Disabled
    // by default. Applies immediately when initialized; otherwise when it
    // becomes ready. Returns 0 = success, negative = error.
    int setStatusBarEnabled(bool enabled)
    {
        return heliosview_webview_set_status_bar(m_webview, enabled ? 1 : 0);
    }

    // Enable (enabled) or disable the WebView2 default right-click context
    // menu (copy/paste/inspect etc.). Enabled by default. On WebView2 runtimes
    // older than 100 the toggle has no effect. Returns 0 = success,
    // negative = error.
    int setContextMenuEnabled(bool enabled)
    {
        return heliosview_webview_set_context_menu(m_webview, enabled ? 1 : 0);
    }

    // Enable (enabled) or disable WebView2 DevTools (F12, right-click Inspect).
    // When disabled, DevTools cannot be opened and an already-open DevTools
    // window is closed. Enabled by default. Returns 0 = success, negative = error.
    int setDevToolsEnabled(bool enabled)
    {
        return heliosview_webview_set_devtools(m_webview, enabled ? 1 : 0);
    }

    // ---- local resources ----

    // Map a local folder to a virtual host name so the page can load files from
    // it via https://<host>/<relative-path>. WebView2 restricts mappings to the
    // ".local" host suffix (e.g. "assets.local"). Call before navigating; the
    // page must be reloaded for new mappings to take effect.
    // Returns 0 = success, negative = failure.
    int mapLocalFolder(const char* host_name, const char* folder_path)
    {
        return heliosview_webview_map_local_folder(m_webview, host_name, folder_path);
    }

    // ---- JS <-> native bridge ----
    // The WebView injects a shim exposing window.helios.call(name, ...args) -> Promise
    // and a BroadcastChannel(name) that receives native broadcasts.

    // Register a native function callable from JS via window.helios.call(name, ...).
    // callback(call_id, name, args_json, userdata) fires on the UI thread; args_json
    // is the JSON array of the JS arguments. Reply with resolve/reject.
    // userdata_dtor is called when the binding is replaced or the WebView is destroyed
    // (may be nullptr). Rebinding a name replaces the previous binding.
    // name must be a valid C identifier ([A-Za-z_][A-Za-z0-9_]*); the C layer rejects
    // anything else (e.g. dots) and this wrapper throws std::invalid_argument, so a
    // bad name fails loudly at setup instead of silently never being callable.
    // Internal names: the library's built-in bridge uses "__hv."-prefixed names
    // (__hv.control / __hv.state / __hv.drag, used by the injected components);
    // the dot makes them invalid C identifiers, so they cannot be bound here.
    void bind(const char* name, heliosview_webview_bind_cb callback, void* userdata = nullptr,
              heliosview_webview_userdata_dtor userdata_dtor = nullptr)
    {
        const int rc = heliosview_webview_bind(m_webview, name, callback, userdata, userdata_dtor);
        if (rc != 0)
            throw std::invalid_argument(
                "heliosview bind: invalid webview or name (names must be C identifiers, no dots)");
    }

    // Boost.JSON auto-binding (declared here, defined in <HeliosViewCore/WebViewJson.h>):
    // parses the JS call's argument array into the Args... types, runs handler(Args...)
    // as a detached std::execution::task, and resolves the Promise with the serialized
    // Resp (or rejects it with the error). Requires Boost.JSON; include WebViewJson.h
    // before calling. Example:
    //   win.bindJson<int, int>("add", [](int a, int b) -> std::execution::task<int> {
    //       co_return a + b;
    //   });
    template <class... Args, class Fn>
    void bindJson(const char* name, Fn&& handler);

    // Member-function overload of bindJson: bind a member function of `obj` (usually
    // `this`) whose signature is `Sender (Obj::*)(Args...)` and returns a sender (e.g.
    // std::execution::task<Resp>). `obj` is captured by pointer and must outlive the
    // binding. Example:
    //   win.bindJson<int, int>("add", this, &MyClass::add);
    template <class... Args, class Obj, class MFPtr>
    void bindJson(const char* name, Obj* obj, MFPtr method);

    // Resolve a pending JS Promise (call_id from the bind callback). result_json is any
    // valid JSON value. Thread-safe: may be called from any thread.
    void resolve(uint64_t call_id, const char* result_json)
    {
        heliosview_webview_resolve(m_webview, call_id, result_json);
    }

    // Reject a pending JS Promise. error_json is any valid JSON value. Thread-safe.
    void reject(uint64_t call_id, const char* error_json)
    {
        heliosview_webview_reject(m_webview, call_id, error_json);
    }

    // Run a JavaScript string (fire-and-forget). Queued while initializing.
    void eval(const char* script)
    {
        heliosview_webview_eval(m_webview, script);
    }

    // Run a JavaScript string; callback(error, result_json, userdata) fires once on the
    // UI thread with the JSON encoding of the completion value. Queued while initializing.
    void evalAsync(const char* script, heliosview_webview_eval_cb callback, void* userdata = nullptr)
    {
        heliosview_webview_eval_async(m_webview, script, callback, userdata);
    }

    // Broadcast a JSON value to the page's BroadcastChannel(name) instances; the page
    // receives it as a standard 'message' event. Thread-safe.
    void broadcast(const char* name, const char* data_json)
    {
        heliosview_webview_broadcast(m_webview, name, data_json);
    }

    // Subscribe to broadcasts the page posts via its BroadcastChannel(name) instances:
    // callback(name, data_json, userdata) fires on the UI thread for every postMessage
    // to a channel of that name. Rebinding a name replaces the previous subscription
    // (running its dtor). userdata_dtor runs when replaced or on destruction (may be
    // nullptr). UI-thread call (thread-safe: other threads are marshalled). The
    // internal "__hv.*" names (see bind) are not valid identifiers and cannot be
    // subscribed. Like bind, an invalid name throws std::invalid_argument.
    void subscribe(const char* name, heliosview_webview_subscribe_cb callback,
                   void* userdata = nullptr, heliosview_webview_userdata_dtor userdata_dtor = nullptr)
    {
        const int rc = heliosview_webview_subscribe(m_webview, name, callback, userdata, userdata_dtor);
        if (rc != 0)
            throw std::invalid_argument(
                "heliosview subscribe: invalid webview or name (names must be C identifiers, no dots)");
    }

    // Boost.JSON auto-subscription (declared here, defined in <HeliosViewCore/WebViewJson.h>):
    // the page's BroadcastChannel(name).postMessage(data) is deserialized into a Req DTO and
    // passed to callback(Req) on the UI thread; the callback returns void. Requires
    // Boost.JSON; include WebViewJson.h before calling. Example:
    //   win.subscribeJson<StatusReq>("status", [](StatusReq req) { ... });
    template <class Req, class Fn>
    void subscribeJson(const char* name, Fn&& callback);

    // Member-function overload of subscribeJson: subscribe a member function of `obj`
    // (usually `this`) with signature `void (Obj::*)(Req)`. `obj` is captured by pointer
    // and must outlive the subscription. Example:
    //   win.subscribeJson<StatusReq>("status", this, &MyClass::onStatus);
    template <class Req, class Obj, class MFPtr>
    void subscribeJson(const char* name, Obj* obj, MFPtr method);

    // Remove the BroadcastChannel(name) subscription (running its dtor). UI-thread call.
    void unsubscribe(const char* name)
    {
        heliosview_webview_unsubscribe(m_webview, name);
    }

    // Signal for navigation events (see navigationCompleted above); this overload
    // connects a member function that returns a sender (started fire-and-forget
    // on each load completion). The object must outlive the connection.
    template <class Obj, class Ret>
    void connectNavigation(Ret Obj::* member, Obj* obj)
    {
        navigationCompleted.connect(member, obj);
    }

    // Convenience connector for the navigationStarting signal: connect a member
    // function (sync or async) of `obj`. Signature: void/async (Obj::*)(std::string,
    // bool, bool). The object must outlive the connection.
    template <class Obj, class Ret>
    void connectStarting(Ret Obj::* member, Obj* obj)
    {
        navigationStarting.connect(member, obj);
    }

private:
    // Bridge the C webview event callbacks to the C++ signals. The C layer stores
    // the userdata (this WebViewWindow) and runs the callbacks on the UI thread;
    // the registrations happen here so that after a move (new object address) the
    // C userdata is re-pointed at the new object.
    void wireWebViewEvents()
    {
        heliosview_webview_set_navigation_callback(
            m_webview,
            [](heliosview_webview_t* wv, int error, void* userdata) {
                static_cast<WebViewWindow*>(userdata)->navigationCompleted(error);
            },
            this, nullptr);

        /* navigation starting: consult the veto gate (if any), then observe. */
        heliosview_webview_set_navigation_starting_callback(
            m_webview,
            [](heliosview_webview_t* wv, const char* uri, int is_redirected,
               int is_user_initiated, void* userdata) -> int {
                auto* self = static_cast<WebViewWindow*>(userdata);
                std::string u = uri ? uri : "";
                const bool redirect = is_redirected != 0;
                const bool user_init = is_user_initiated != 0;
                self->navigationStarting(u, redirect, user_init);
                if (self->navigationStartingGate)
                    return self->navigationStartingGate(u, redirect, user_init) ? 1 : 0;
                return 0;
            },
            this, nullptr);

        heliosview_webview_set_source_changed_callback(
            m_webview,
            [](heliosview_webview_t* wv, const char* uri, int is_new_document, void* userdata) {
                auto* self = static_cast<WebViewWindow*>(userdata);
                self->urlChanged(uri ? uri : "", is_new_document != 0);
            },
            this, nullptr);

        heliosview_webview_set_title_changed_callback(
            m_webview,
            [](heliosview_webview_t* wv, const char* title, void* userdata) {
                static_cast<WebViewWindow*>(userdata)->titleChanged(title ? title : "");
            },
            this, nullptr);
    }

    heliosview_webview_t* m_webview = nullptr;
};

} // namespace helios
