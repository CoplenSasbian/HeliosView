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

#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/Window.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace helios {

// Web engine version used by the WebView backend (UTF-8, e.g. "131.0.2903.86").
// Empty = no engine available (on Windows: the WebView2 Runtime is not installed),
// in which case createWebView() fails and the C API reports -4.
// Platform meaning: Windows = WebView2 Runtime, macOS = system WebKit, Linux = WebKitGTK.
inline std::string webViewEngineVersion()
{
    char buf[64] = {};
    heliosview_webview_engine_version(buf, sizeof buf);
    return buf;
}

/* ---------- right-click interception (see WebViewWindow::contextMenuGate) ---------- */

// What a right-click hit (bit flags: a link inside an editable field combines two).
enum class ContextMenuTarget : uint32_t {
    Page = HELIOSVIEW_CONTEXT_MENU_TARGET_PAGE,
    Selection = HELIOSVIEW_CONTEXT_MENU_TARGET_SELECTION,
    Link = HELIOSVIEW_CONTEXT_MENU_TARGET_LINK,
    Image = HELIOSVIEW_CONTEXT_MENU_TARGET_IMAGE,
    Media = HELIOSVIEW_CONTEXT_MENU_TARGET_MEDIA,
    Editable = HELIOSVIEW_CONTEXT_MENU_TARGET_EDITABLE,
};

inline constexpr ContextMenuTarget operator|(ContextMenuTarget a, ContextMenuTarget b)
{
    return static_cast<ContextMenuTarget>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr uint32_t toUint(ContextMenuTarget flags)
{
    return static_cast<uint32_t>(flags);
}

// The right-click request handed to WebViewWindow::contextMenuGate. Strings are
// UTF-8 and empty when the field does not apply.
struct ContextMenuInfo {
    ContextMenuTarget target{};  // what was hit
    int32_t x = 0;               // request position relative to the WebView's top-left
    int32_t y = 0;               // (a native menu pops at the cursor anyway)
    std::string linkUrl;         // the link under the cursor
    std::string linkText;        // that link's text
    std::string selectionText;   // the selected text
    std::string pageUrl;         // the document's URL

    // Did the click hit `flag`? e.g. info.has(ContextMenuTarget::Link)
    bool has(ContextMenuTarget flag) const { return (toUint(target) & toUint(flag)) != 0; }
};

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
    // opts: creation-time WebView2 environment options (see
    // heliosview_webview_env_opts_t). They are read when the WebView2 environment
    // is created and cannot be changed afterwards, so pass them here at creation.
    // Zero-initialize the struct and set only what you need (a zeroed struct or
    // nullptr = the runtime defaults).
    void createWebView(const heliosview_webview_env_opts_t& opts)
    {
        if (!m_webview) {
            m_webview = heliosview_webview_create_ex(nativeHandle(), &opts);
            wireWebViewEvents();
        }
    }

    // Convenience: create the WebView with just an explicit WebView2 user data
    // folder (UTF-8 absolute path; nullptr/empty = the default next to the
    // executable; the folder is created by WebView2 if missing). Everything
    // else stays at the runtime default.
    void createWebView(const char* user_data_folder = nullptr)
    {
        heliosview_webview_env_opts_t opts{};   /* zero = runtime defaults */
        opts.user_data_folder = user_data_folder;
        createWebView(opts);
    }

    // Destroy the WebView (releasing WebView2's controller, event handlers,
    // bindings and subscriptions), keeping the window itself alive — the window
    // stays usable and createWebView() can be called again later (e.g. to unload
    // the page and reload a fresh WebView). Runs the userdata dtors of the
    // registered bindings/subscriptions; navigation signals (navigationStarting,
    // urlChanged, titleChanged, navigationCompleted) no longer fire after this.
    // Idempotent: no-op when no WebView exists (also called automatically by the
    // destructor). Message-loop thread, like the other WebView APIs.
    void destroyWebView()
    {
        if (m_webview) {
            heliosview_webview_destroy(m_webview);
            m_webview = nullptr;
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

    // Fired on the UI thread when a page load completes: error ==
    // WebViewError::Ok (0) on success, otherwise a portable WebViewError value
    // (the engine's own code is available via lastNativeError()). Not fired for
    // navigations that never finish (e.g. aborted). Use it to know when the page
    // is ready for eval() or to show an error state when loading fails.
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

    // ---- low-footprint mode (webview suspend / resume) ----

    // Suspend the WebView (WebView2 TrySuspend): rendering stops and most of
    // the browser process's resources are released while the page stays alive —
    // the low-footprint mode for a hidden/background window. webviewResume()
    // restores it where it left off. Typical use: webviewSuspend() when the
    // window is hidden, webviewResume() when it is shown again.
    // Completion is asynchronous: callback(error, suspended, userdata) fires on
    // the UI thread (nullptr = fire-and-forget). suspended is 1 if the WebView
    // actually suspended (TrySuspend can decline, e.g. while audio is playing).
    // A suspend requested before initialization is recorded and applied when
    // the core becomes ready.
    void webviewSuspend(heliosview_webview_suspend_cb callback = nullptr, void* userdata = nullptr)
    {
        heliosview_webview_suspend(m_webview, callback, userdata);
    }

    // Resume a suspended WebView (synchronous; no-op if not suspended).
    // Returns 0 = success.
    int webviewResume() { return heliosview_webview_resume(m_webview); }

    // Whether the WebView is currently suspended (true once a suspend request
    // is made/completed; false after webviewResume()).
    bool webviewIsSuspended() const
    {
        int s = 0;
        heliosview_webview_is_suspended(m_webview, &s);
        return s != 0;
    }

    // ---- webview background color ----

    // Set the WebView's default background color (WebView2
    // DefaultBackgroundColor): the color painted behind the page content
    // (default: opaque white). (r, g, b, a) are 0-255; alpha 0 = transparent —
    // the parent window's own content shows through the WebView (for the
    // desktop to show through, the parent window must itself be transparent,
    // e.g. a layered window). Applies immediately when initialized, otherwise
    // when it becomes ready. Returns 0 = success.
    int webviewSetBackgroundColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return heliosview_webview_set_background_color(m_webview, r, g, b, a);
    }

    // Convenience: make the WebView background transparent (true) or restore
    // the default opaque white (false). Returns 0 = success.
    int webviewSetTransparentBackground(bool transparent)
    {
        return heliosview_webview_set_transparent_background(m_webview, transparent ? 1 : 0);
    }

    // Show (enabled) or hide the WebView2 status bar, which displays the target
    // URL of a hovered link at the bottom-left corner of the WebView. Disabled
    // by default. Applies immediately when initialized; otherwise when it
    // becomes ready. Returns 0 = success, negative = error.
    int setStatusBarEnabled(bool enabled)
    {
        return heliosview_webview_set_status_bar(m_webview, enabled ? 1 : 0);
    }

    // Show (enabled, the default) or suppress the WebView2 default right-click
    // context menu (copy/paste/inspect etc.). This is the one switch that decides
    // whether the engine's own menu may open; who else reacts to the right click is
    // independent of it (contextMenuGate below, or the page's own 'contextmenu'
    // event). Suppressing needs the engine's context-menu hook: turning it off
    // returns a negative error (HELIOSVIEW_ERROR_UNSUPPORTED) when the engine has
    // none (e.g. a WebView2 runtime older than 100), so the app can fall back to a
    // menu drawn in the page; turning it back on always succeeds.
    // Returns 0 = success, negative = error.
    int setContextMenuEnabled(bool enabled)
    {
        return heliosview_webview_set_context_menu(m_webview, enabled ? 1 : 0);
    }

    // The native interception point: consulted for every right click before the
    // engine opens its own menu, with what the click hit (ContextMenuInfo: target
    // flags, link/selection/page URLs, position). Return true to intercept that
    // click - the engine's menu stays closed, whatever setContextMenuEnabled() says
    // - or false to fall through to the switch, so one gate can serve some targets
    // itself and leave the rest to the engine.
    //
    // The library does not choose what to show: build a helios::Menu (menu.show()
    // pops at the cursor, i.e. where the click happened), let the page draw one, or
    // show nothing. Runs on the UI thread. A popup menu runs a modal message loop,
    // and this callback runs inside the engine's own event dispatch, so suppress
    // now and open yours on the next UI turn instead of re-entering the engine:
    //     win.contextMenuGate = [&](const ContextMenuInfo& info) {
    //         if (info.has(ContextMenuTarget::Editable)) return false;
    //         App::instance()->postTask([&] { menu.show(win.nativeHandle()); });
    //         return true;
    //     };
    std::function<bool(const ContextMenuInfo&)> contextMenuGate;

    // Enable (enabled) or disable WebView2 DevTools (F12, right-click Inspect).
    // When disabled, DevTools cannot be opened and an already-open DevTools
    // window is closed. Enabled by default. Returns 0 = success, negative = error.
    int setDevToolsEnabled(bool enabled)
    {
        return heliosview_webview_set_devtools(m_webview, enabled ? 1 : 0);
    }

    // Enable (enabled) or disable WebView2's built-in window controls overlay
    // (the min/max/restore/close buttons WebView2 draws over the page's
    // top-right corner). Disabled by default — apps that render their own
    // title-bar buttons (e.g. the injected <helios-window-controls> component)
    // leave it off. Applies immediately when initialized; when called during
    // initialization it is applied when the WebView becomes ready. Requires the
    // experimental WebView2 interface; on runtimes without it the call returns
    // negative and has no effect. Returns 0 = success, negative = error.
    int setWindowControlsOverlay(bool enabled)
    {
        return heliosview_webview_set_window_controls_overlay(m_webview, enabled ? 1 : 0);
    }

    // Set the window controls overlay's background color (r, g, b, a, 0-255).
    // Default: fully transparent (a = 0) — the page's own title bar shows
    // through and the buttons float over it. Applies immediately when the
    // overlay exists; when called before it is enabled the color is applied
    // when the overlay is created. Returns 0 = success, negative = error.
    int setWindowControlsBackgroundColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return heliosview_webview_set_window_controls_background_color(m_webview, r, g, b, a);
    }

    // ---- local resources ----

    // Map a local folder to a virtual host name so the page can load files from
    // it. Build the URL with localUrl() — the URL shape is engine-defined
    // (Windows: https://<host>/..., WebView2 requires the ".local" host suffix,
    // e.g. "assets.local"; other engines register a custom scheme). Call before
    // navigating; the page must be reloaded for new mappings to take effect.
    // Returns 0 = success, negative = failure.
    int mapLocalFolder(const char* host_name, const char* folder_path)
    {
        return heliosview_webview_map_local_folder(m_webview, host_name, folder_path);
    }

    // The URL serving `path` from the folder mapped to `host_name` (see
    // mapLocalFolder). Empty string on failure.
    std::string localUrl(const char* host_name, const char* path)
    {
        char buf[1024] = {};
        if (heliosview_webview_local_url(m_webview, host_name, path, buf, sizeof buf) != 0)
            return {};
        return buf;
    }

    // The engine's own error code for the last failed navigation (WebView2: a
    // COREWEBVIEW2_WEB_ERROR_STATUS_* value; macOS: NSURLError; Linux: GError).
    // 0 = none.
    int lastNativeError() const { return heliosview_webview_last_native_error(m_webview); }

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
            throwLastError<std::invalid_argument>(
                "heliosview bind"); /* invalid webview/name: the C layer recorded the reason */
    }

    // Boost.JSON auto-binding (declared here, defined in <HeliosViewCore/WebViewJson.h>):
    // parses the JS call's argument array into the Args... types, runs handler(Args...)
    // as a detached std::execution::task, and resolves the Promise with the serialized
    // Resp (or rejects it with the error). Requires Boost.JSON; include WebViewJson.h
    // before calling.
    //
    // Args... may be omitted: it is then deduced from the handler's own parameter types
    // (decayed to values), which requires a single non-template signature — a lambda, a
    // functor with one operator(), or a free function. A generic lambda / std::function
    // has to spell the types out. Examples:
    //   win.bindJson("add", [](int a, int b) -> std::execution::task<int> { co_return a + b; });
    //   win.bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<int> { ... }); // explicit
    //   win.bindJson("tick", []() -> std::execution::task<bool> { co_return true; });     // no args
    template <class... Args, class Fn>
    void bindJson(const char* name, Fn&& handler);

    // Member-function overload of bindJson: bind a member function of `obj` (usually
    // `this`) whose signature is `Sender (Obj::*)(Args...)` and returns a sender (e.g.
    // std::execution::task<Resp>). `obj` is captured by pointer and must outlive the
    // binding. Args... is deduced from the member pointer when omitted. Examples:
    //   win.bindJson("repeat", this, &MyClass::repeat);          // deduced from the signature
    //   win.bindJson<RepeatReq>("repeat", this, &MyClass::repeat); // explicit
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
    // UI thread with the JSON encoding of the completion value. A returned Promise
    // is awaited (so `fetch(...).then(r => r.json())` yields the JSON value); a
    // thrown error or rejected promise reports a negative error and the message
    // text. Queued while initializing.
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
            throwLastError<std::invalid_argument>(
                "heliosview subscribe"); /* invalid webview/name: the C layer recorded the reason */
    }

    // Boost.JSON auto-subscription (declared here, defined in <HeliosViewCore/WebViewJson.h>):
    // the page's BroadcastChannel(name).postMessage(data) is deserialized into a Req DTO and
    // passed to callback(Req) on the UI thread; the callback returns void. Requires
    // Boost.JSON; include WebViewJson.h before calling.
    //
    // Req defaults to void = "deduce it from the callback", which then must take exactly
    // one parameter (a generic lambda / std::function has to spell it out). Examples:
    //   win.subscribeJson("status", [](StatusReq req) { ... });          // deduced
    //   win.subscribeJson<StatusReq>("status", [](StatusReq req) { ... }); // explicit
    template <class Req = void, class Fn>
    void subscribeJson(const char* name, Fn&& callback);

    // Member-function overload of subscribeJson: subscribe a member function of `obj`
    // (usually `this`) with signature `void (Obj::*)(Req)`. `obj` is captured by pointer
    // and must outlive the subscription. Req is deduced from the member pointer when
    // omitted. Examples:
    //   win.subscribeJson("status", this, &MyClass::onStatus);           // deduced
    //   win.subscribeJson<StatusReq>("status", this, &MyClass::onStatus); // explicit
    template <class Req = void, class Obj, class MFPtr>
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

        /* right-click: the C layer calls this trampoline for every click. An empty
         * gate means "not intercepted" (return 0 = the engine's own menu may open,
         * subject to setContextMenuEnabled). */
        heliosview_webview_set_context_menu_callback(
            m_webview,
            [](heliosview_webview_t* wv, const heliosview_context_menu_info_t* info,
               void* userdata) -> int {
                auto* self = static_cast<WebViewWindow*>(userdata);
                if (!self->contextMenuGate || !info)
                    return 0;
                ContextMenuInfo ci;
                ci.target = static_cast<ContextMenuTarget>(info->target);
                ci.x = info->x;
                ci.y = info->y;
                ci.linkUrl = info->link_url ? info->link_url : "";
                ci.linkText = info->link_text ? info->link_text : "";
                ci.selectionText = info->selection_text ? info->selection_text : "";
                ci.pageUrl = info->page_url ? info->page_url : "";
                try {
                    return self->contextMenuGate(ci) ? 1 : 0;
                } catch (...) {
                    return 0; /* never let a gate's exception cross the C boundary */
                }
            },
            this, nullptr);
    }

    heliosview_webview_t* m_webview = nullptr;
};

} // namespace helios