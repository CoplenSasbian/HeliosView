#pragma once

/**
 * HeliosView.Core -- WebViewWindow: a window with an embedded WebView.
 *
 * Inherits Window; on win32 it is built on WebView2 (Edge Chromium, SDK pulled in via FetchContent).
 * The WebView is a standalone handle (heliosview_webview_t) attached to its parent window
 * at creation; afterwards, operations involve only the WebView itself.
 * Currently offers navigation (URL / HTML string); events are planned for later.
 *
 * Usage:
 *   helios::WebViewWindow win(900, 640, "title");
 *   win.show();
 *   win.createWebView();      // async initialization
 *   win.navigateHtml("<h1>hi</h1>");   // queued automatically if called before init finishes
 */

#include <HeliosViewCore/Window.h>

#include <cstdint>

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
    }

    WebViewWindow& operator=(WebViewWindow&& other) noexcept
    {
        if (this != &other) {
            Window::operator=(std::move(other));
            if (m_webview)
                heliosview_webview_destroy(m_webview);
            m_webview = other.m_webview;
            other.m_webview = nullptr;
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
        if (!m_webview)
            m_webview = heliosview_webview_create(nativeHandle());
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

    // ---- JS <-> native bridge ----
    // The WebView injects a shim exposing window.helios.call(name, ...args) -> Promise
    // and a BroadcastChannel(name) that receives native broadcasts.

    // Register a native function callable from JS via window.helios.call(name, ...).
    // callback(call_id, name, args_json, userdata) fires on the UI thread; args_json
    // is the JSON array of the JS arguments. Reply with resolve/reject.
    // userdata_dtor is called when the binding is replaced or the WebView is destroyed
    // (may be nullptr). Rebinding a name replaces the previous binding.
    void bind(const char* name, heliosview_webview_bind_cb callback, void* userdata = nullptr,
              heliosview_webview_userdata_dtor userdata_dtor = nullptr)
    {
        heliosview_webview_bind(m_webview, name, callback, userdata, userdata_dtor);
    }

    // nlohmann auto-binding (declared here, defined in <HeliosViewCore/WebViewJson.h>):
    // parses the JS call's first argument into a Req DTO, runs handler(Req) as a
    // detached std::execution::task, and resolves the Promise with the serialized Resp
    // (or rejects it with the error). Requires nlohmann::json; include WebViewJson.h
    // before calling. Example:
    //   win.bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<std::string> {
    //       co_return std::format("{}", req.a + req.b);
    //   });
    template <class Req, class Fn>
    void bindJson(const char* name, Fn&& handler);

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
    // nullptr). UI-thread call (thread-safe: other threads are marshalled).
    void subscribe(const char* name, heliosview_webview_subscribe_cb callback,
                   void* userdata = nullptr, heliosview_webview_userdata_dtor userdata_dtor = nullptr)
    {
        heliosview_webview_subscribe(m_webview, name, callback, userdata, userdata_dtor);
    }

    // nlohmann auto-subscription (declared here, defined in <HeliosViewCore/WebViewJson.h>):
    // the page's BroadcastChannel(name).postMessage(data) is deserialized into a Req DTO and
    // passed to callback(Req) on the UI thread; the callback returns void. Requires
    // nlohmann::json; include WebViewJson.h before calling. Example:
    //   win.subscribeJson<StatusReq>("status", [](StatusReq req) { ... });
    template <class Req, class Fn>
    void subscribeJson(const char* name, Fn&& callback);

    // Remove the BroadcastChannel(name) subscription (running its dtor). UI-thread call.
    void unsubscribe(const char* name)
    {
        heliosview_webview_unsubscribe(m_webview, name);
    }

private:
    heliosview_webview_t* m_webview = nullptr;
};

} // namespace helios
