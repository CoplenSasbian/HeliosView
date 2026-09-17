#pragma once

/**
 * HeliosView.Core -- Window: unified top-level window.
 *
 * Encapsulates the native desktop window shell, embedded WebView2,
 * and child viewport host management (UIHost & WebViewHost) in a single unified class.
 *
 * Depends on: Signal.h, Types.h, App.h (window registry).
 * Events are dispatched to signals via event(); connect via window.keyPressed.connect(...).
 */

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/System.h> /* Rect (work-area query) */
#include <HeliosViewCore/Types.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace helios {

/* The child viewport host that carries a widget tree. It is defined in
 * <HeliosViewCore/UIHost.h>, which includes this header first because a UIHost is
 * created against a complete Window; a translation unit that needs both includes
 * Window.h, then UIHost.h (the demo headers do exactly that). */
class UIHost;

/* Web engine version used by the WebView backend (UTF-8, e.g. "131.0.2903.86"). */
inline std::string webViewEngineVersion()
{
    char buf[64] = {};
    heliosview_webview_engine_version(buf, sizeof buf);
    return buf;
}

/* ---------- Right-click context menu targets and info ---------- */

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

struct ContextMenuInfo {
    ContextMenuTarget target{};
    int32_t x = 0;
    int32_t y = 0;
    std::string linkUrl;
    std::string linkText;
    std::string selectionText;
    std::string pageUrl;

    bool has(ContextMenuTarget flag) const { return (toUint(target) & toUint(flag)) != 0; }
};

/* Window taskbar progress state (mirrors heliosview_progress_state_t) */
enum class ProgressState : int32_t {
    None = HELIOSVIEW_PROGRESS_NONE,
    Normal = HELIOSVIEW_PROGRESS_NORMAL,
    Indeterminate = HELIOSVIEW_PROGRESS_INDETERMINATE,
    Error = HELIOSVIEW_PROGRESS_ERROR,
    Paused = HELIOSVIEW_PROGRESS_PAUSED,
};

/* Window backdrop material (mirrors heliosview_backdrop_t; Win11) */
enum class Backdrop : int32_t {
    None = HELIOSVIEW_BACKDROP_NONE,
    Mica = HELIOSVIEW_BACKDROP_MICA,
    Acrylic = HELIOSVIEW_BACKDROP_ACRYLIC,
};

class Window {
public:
    // Construct a window with the given client size, title (UTF-8) and preset style.
    Window(int width, int height, const char* title,
           WindowStyle style = WindowStyle::Normal)
        : Window(width, height, title, style, WindowFlag::None)
    {
    }

    // Same, with style flags (WindowFlag).
    Window(int width, int height, const char* title, WindowStyle style, WindowFlag flags)
        : m_window(heliosview_window_create_ex2(width, height, title,
                                                static_cast<heliosview_window_style_t>(style),
                                                toUint(flags), this))
    {
        if (!m_window)
            throwLastError("window creation failed");
    }

    // Destroy the window; ensures child hosts and webviews are cleanly destroyed first
    virtual ~Window() { close(); }

    // Non-copyable: a Window uniquely owns its native window and resources
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Move: transfers native window, webview, and child hosts
    Window(Window&& other) noexcept
        : m_window(other.m_window)
        , m_webview(other.m_webview)
        , m_hosts(std::move(other.m_hosts))
    {
        other.m_window = nullptr;
        other.m_webview = nullptr;
        heliosview_window_set_userdata(m_window, this);
        wireWebViewEvents();
    }

    Window& operator=(Window&& other) noexcept
    {
        if (this != &other) {
            close();
            m_window = other.m_window;
            m_webview = other.m_webview;
            m_hosts = std::move(other.m_hosts);
            other.m_window = nullptr;
            other.m_webview = nullptr;
            heliosview_window_set_userdata(m_window, this);
            wireWebViewEvents();
        }
        return *this;
    }

    // Show the native window
    void show() { heliosview_window_show(m_window); }

    // Hide the native window
    void hide() { heliosview_window_hide(m_window); }

    // Show the window in the given state
    void showState(ShowState state)
    {
        heliosview_window_show_state(m_window, static_cast<heliosview_show_state_t>(state));
    }
    void showNormal() { showState(ShowState::Normal); }
    void showMinimized() { showState(ShowState::Minimized); }
    void showMaximized() { showState(ShowState::Maximized); }

    // The current show state of the window
    ShowState state() const
    {
        return static_cast<ShowState>(heliosview_window_state(m_window));
    }

    // Minimize / maximize / restore
    void minimize() { heliosview_window_minimize(m_window); }
    void maximize() { heliosview_window_maximize(m_window); }
    void restore() { heliosview_window_restore(m_window); }
    void toggleMaximize() { heliosview_window_toggle_maximize(m_window); }

    // Enable/disable user resizing
    void setResizable(bool resizable) { heliosview_window_set_resizable(m_window, resizable ? 1 : 0); }

    // ---- Frameless dragging ----
    void addDragRegion(int32_t x, int32_t y, int32_t width, int32_t height)
    {
        heliosview_window_add_drag_region(m_window, x, y, width, height);
    }
    void clearDragRegions() { heliosview_window_clear_drag_regions(m_window); }
    void startDrag() { heliosview_window_start_drag(m_window); }

    // DPI & scale
    uint32_t dpi() const { return heliosview_window_dpi(m_window); }
    float scaleFactor() const { return heliosview_window_scale_factor(m_window); }
    WindowFlag flags() const { return static_cast<WindowFlag>(heliosview_window_flags(m_window)); }
    int32_t titleBarHeight() const { return heliosview_window_title_bar_height(m_window); }

    bool workArea(Rect& out) const
    {
        heliosview_rect_t r{};
        if (heliosview_window_work_area(m_window, &r) != 0)
            return false;
        out = {r.x, r.y, r.width, r.height};
        return true;
    }

    // Size constraints
    void setMinimumSize(int32_t w, int32_t h) { heliosview_window_set_min_size(m_window, w, h); }
    void setMaximumSize(int32_t w, int32_t h) { heliosview_window_set_max_size(m_window, w, h); }

    // Flash & Fullscreen
    void flash() { heliosview_window_flash(m_window); }
    void flashUntilFocus() { heliosview_window_flash_until_focus(m_window); }
    void setFullscreen(bool on) { heliosview_window_set_fullscreen(m_window, on ? 1 : 0); }
    bool isFullscreen() const { return heliosview_window_is_fullscreen(m_window) != 0; }

    // Enable & Focus
    void setEnabled(bool on) { heliosview_window_set_enabled(m_window, on ? 1 : 0); }
    bool isEnabled() const { return heliosview_window_is_enabled(m_window) != 0; }
    void requestClose() { heliosview_window_close(m_window); }
    void focus() { heliosview_window_focus(m_window); }
    void setTopmost(bool on) { heliosview_window_set_topmost(m_window, on ? 1 : 0); }
    bool isVisible() const { return heliosview_window_is_visible(m_window) != 0; }

    // Position & Size
    void move(int32_t x, int32_t y) { heliosview_window_set_position(m_window, x, y); }
    bool position(int32_t& x, int32_t& y) const
    {
        return heliosview_window_position(m_window, &x, &y) == 0;
    }
    void resize(int32_t width, int32_t height) { heliosview_window_set_size(m_window, width, height); }
    bool size(int32_t& width, int32_t& height) const
    {
        return heliosview_window_size(m_window, &width, &height) == 0;
    }
    void setGeometry(int32_t x, int32_t y, int32_t width, int32_t height)
    {
        move(x, y);
        resize(width, height);
    }
    bool geometry(int32_t& x, int32_t& y, int32_t& width, int32_t& height) const
    {
        return position(x, y) && size(width, height);
    }

    // Title, Center, Opacity, Icon
    void setTitle(const char* title) { heliosview_window_set_title(m_window, title); }
    void setTitle(const std::string& title) { heliosview_window_set_title(m_window, title.c_str()); }
    void center() { heliosview_window_center(m_window); }
    void setOpacity(float opacity) { heliosview_window_set_opacity(m_window, opacity); }
    void setIcon(const char* icon_path) { heliosview_window_set_icon(m_window, icon_path); }
    void setIcon(const char* icon_path, IconFlag flags)
    {
        heliosview_window_set_icon_ex(m_window, icon_path, toUint(flags));
    }

    // Taskbar progress
    void setProgress(uint32_t value, uint32_t max) { heliosview_window_set_progress(m_window, value, max); }
    void setProgressState(ProgressState state)
    {
        heliosview_window_set_progress_state(m_window, static_cast<heliosview_progress_state_t>(state));
    }
    void clearProgress() { heliosview_window_clear_progress(m_window); }

    // Win11 Backdrop & Dark mode
    int setBackdrop(Backdrop backdrop)
    {
        return heliosview_window_set_backdrop(m_window, static_cast<heliosview_backdrop_t>(backdrop));
    }
    int setDarkMode(bool on) { return heliosview_window_set_dark_mode(m_window, on ? 1 : 0); }

    // Close and destroy the native window and all attached resources
    void close()
    {
        for (auto* host : m_hosts) {
            detachHost(host);
        }
        m_hosts.clear();
        m_uiHosts.clear();

        if (m_webview) {
            heliosview_webview_destroy(m_webview);
            m_webview = nullptr;
        }

        if (m_window) {
            heliosview_window_destroy(m_window);
            m_window = nullptr;
        }
    }

    // Native handles
    uintptr_t id() const { return heliosview_window_id(m_window); }
    heliosview_window_t* nativeHandle() const { return m_window; }
    // The raw C handle under the name the other wrappers use (UIHost::handle,
    // Canvas::handle), so handle()-based helpers accept a Window directly.
    heliosview_window_t* handle() const { return m_window; }
    void* nativeHandle(heliosview_webview_handle_kind_t kind) const
    {
        return heliosview_webview_native_handle(m_webview, kind);
    }
    heliosview_webview_t* webview() const { return m_webview; }

    /* =========================================================================
     * Child Viewport Host Management (UIHost & WebViewHost)
     *
     * The three UIHost methods are declared here and defined in
     * <HeliosViewCore/UIHost.h>: creating one needs a complete UIHost, which this
     * header only forward-declares. A caller that uses them includes UIHost.h
     * (directly, or through HeliosViewCore/HeliosView.h or UI/Widget.h).
     * ========================================================================= */

    // A widget-tree viewport (helios::UIHost) of width x height at (x, y) inside
    // this window's client area, attached to the window: close() destroys it with
    // the window, and the returned UIHost is a borrowed handle, not the owner.
    UIHost createUIHost(int x, int y, int width, int height,
                        heliosview_canvas_engine_t engine = HELIOSVIEW_ENGINE_BLEND2D);

    // Same, for an embedded web viewport. Get the WebView itself with
    // heliosview_host_get_webview(host.handle()).
    UIHost createWebViewHost(int x, int y, int width, int height);

    // Destroy a host created by one of the factories above and detach it from the
    // window. The caller's UIHost becomes an empty object.
    void destroyHost(UIHost& host);

    // Register a C host this window now destroys on close(). Returns the host so a
    // caller can wrap it in one expression: UIHost(ownHost(heliosview_host_create_ui(...))).
    heliosview_host_t* ownHost(heliosview_host_t* host)
    {
        if (host) m_hosts.push_back(host);
        return host;
    }

    // The UI hosts this window carries, for key-event routing (used by event())
    const std::vector<heliosview_host_t*>& uiHosts() const { return m_uiHosts; }

    /* =========================================================================
     * Embedded WebView Management & RPC Bridge
     * ========================================================================= */

    void createWebView(const heliosview_webview_env_opts_t& opts)
    {
        if (!m_webview) {
            m_webview = heliosview_webview_create_ex(m_window, &opts);
            wireWebViewEvents();
        }
    }

    void createWebView(const char* user_data_folder = nullptr)
    {
        heliosview_webview_env_opts_t opts{};
        opts.user_data_folder = user_data_folder;
        createWebView(opts);
    }

    void ensureWebView()
    {
        if (!m_webview) {
            createWebView();
        }
    }

    void destroyWebView()
    {
        if (m_webview) {
            heliosview_webview_destroy(m_webview);
            m_webview = nullptr;
        }
    }

    void navigate(const char* url)
    {
        ensureWebView();
        heliosview_webview_navigate(m_webview, url);
    }

    void navigateHtml(const char* html)
    {
        ensureWebView();
        heliosview_webview_navigate_html(m_webview, html);
    }

    void setWebViewInsets(int32_t top, int32_t right, int32_t bottom, int32_t left)
    {
        if (m_webview)
            heliosview_webview_set_insets(m_webview, top, right, bottom, left);
    }

    int setLowFootprint(bool enabled, heliosview_webview_low_footprint_cb callback = nullptr,
                        void* userdata = nullptr)
    {
        if (!m_webview) return HELIOSVIEW_ERROR_UNSUPPORTED;
        return heliosview_webview_set_low_footprint(m_webview, enabled ? 1 : 0, callback, userdata);
    }

    bool isLowFootprint() const
    {
        if (!m_webview) return false;
        int on = 0;
        heliosview_webview_is_low_footprint(m_webview, &on);
        return on != 0;
    }

    int webviewSetBackgroundColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        ensureWebView();
        return heliosview_webview_set_background_color(m_webview, r, g, b, a);
    }

    int webviewSetTransparentBackground(bool transparent)
    {
        ensureWebView();
        return heliosview_webview_set_transparent_background(m_webview, transparent ? 1 : 0);
    }

    int addInitScript(const char* script)
    {
        ensureWebView();
        return heliosview_webview_add_init_script(m_webview, script);
    }

    int clearInitScripts()
    {
        if (!m_webview) return 0;
        return heliosview_webview_clear_init_scripts(m_webview);
    }

    int setZoom(double factor)
    {
        ensureWebView();
        return heliosview_webview_set_zoom(m_webview, factor);
    }

    double zoom() const
    {
        if (!m_webview) return 1.0;
        double factor = 1.0;
        heliosview_webview_zoom(m_webview, &factor);
        return factor;
    }

    using CookiesFn = std::function<void(int error, const heliosview_webview_cookie_t* cookies, size_t count)>;
    using CookieOpFn = std::function<void(int error)>;

    int getCookies(const char* url, CookiesFn callback)
    {
        if (!m_webview) return HELIOSVIEW_ERROR_UNSUPPORTED;
        auto* fn = new CookiesFn(std::move(callback));
        const int rc = heliosview_webview_get_cookies(m_webview, url, &cookiesTrampoline, fn);
        if (rc != 0) delete fn;
        return rc;
    }

    int setCookie(const char* url, const heliosview_webview_cookie_t* cookie, CookieOpFn callback = {})
    {
        if (!m_webview) return HELIOSVIEW_ERROR_UNSUPPORTED;
        auto* fn = new CookieOpFn(std::move(callback));
        const int rc = heliosview_webview_set_cookie(m_webview, url, cookie, &cookieOpTrampoline, fn);
        if (rc != 0) delete fn;
        return rc;
    }

    int deleteCookie(const char* name, const char* url, CookieOpFn callback = {})
    {
        if (!m_webview) return HELIOSVIEW_ERROR_UNSUPPORTED;
        auto* fn = new CookieOpFn(std::move(callback));
        const int rc = heliosview_webview_delete_cookie(m_webview, name, url, &cookieOpTrampoline, fn);
        if (rc != 0) delete fn;
        return rc;
    }

    int clearCookies(CookieOpFn callback = {})
    {
        if (!m_webview) return HELIOSVIEW_ERROR_UNSUPPORTED;
        auto* fn = new CookieOpFn(std::move(callback));
        const int rc = heliosview_webview_clear_cookies(m_webview, &cookieOpTrampoline, fn);
        if (rc != 0) delete fn;
        return rc;
    }

    int setContextMenuEnabled(bool enabled)
    {
        ensureWebView();
        return heliosview_webview_set_context_menu(m_webview, enabled ? 1 : 0);
    }

    std::function<bool(const ContextMenuInfo&)> contextMenuGate;

    int setDevToolsEnabled(bool enabled)
    {
        ensureWebView();
        return heliosview_webview_set_devtools(m_webview, enabled ? 1 : 0);
    }

    int openDevTools()
    {
        if (!m_webview) return HELIOSVIEW_ERROR_UNSUPPORTED;
        return heliosview_webview_open_devtools(m_webview);
    }

    int mapLocalFolder(const char* host_name, const char* folder_path)
    {
        ensureWebView();
        return heliosview_webview_map_local_folder(m_webview, host_name, folder_path);
    }

    std::string localUrl(const char* host_name, const char* path)
    {
        if (!m_webview) return {};
        char buf[1024] = {};
        if (heliosview_webview_local_url(m_webview, host_name, path, buf, sizeof buf) != 0)
            return {};
        return buf;
    }

    int lastNativeError() const
    {
        if (!m_webview) return 0;
        return heliosview_webview_last_native_error(m_webview);
    }

    // JS <-> native RPC bindings
    void bind(const char* name, heliosview_webview_bind_cb callback, void* userdata = nullptr,
              heliosview_webview_userdata_dtor userdata_dtor = nullptr)
    {
        ensureWebView();
        const int rc = heliosview_webview_bind(m_webview, name, callback, userdata, userdata_dtor);
        if (rc != 0)
            throwLastError<std::invalid_argument>("heliosview bind");
    }

    template <class... Args, class Fn>
    void bindJson(const char* name, Fn&& handler);

    template <class... Args, class Obj, class MFPtr>
    void bindJson(const char* name, Obj* obj, MFPtr method);

    void resolve(uint64_t call_id, const char* result_json)
    {
        if (m_webview)
            heliosview_webview_resolve(m_webview, call_id, result_json);
    }

    void reject(uint64_t call_id, const char* error_json)
    {
        if (m_webview)
            heliosview_webview_reject(m_webview, call_id, error_json);
    }

    void eval(const char* script)
    {
        if (m_webview)
            heliosview_webview_eval(m_webview, script);
    }

    void evalAsync(const char* script, heliosview_webview_eval_cb callback, void* userdata = nullptr)
    {
        if (m_webview)
            heliosview_webview_eval_async(m_webview, script, callback, userdata);
    }

    void broadcast(const char* name, const char* data_json)
    {
        if (m_webview)
            heliosview_webview_broadcast(m_webview, name, data_json);
    }

    void subscribe(const char* name, heliosview_webview_subscribe_cb callback,
                   void* userdata = nullptr, heliosview_webview_userdata_dtor userdata_dtor = nullptr)
    {
        ensureWebView();
        const int rc = heliosview_webview_subscribe(m_webview, name, callback, userdata, userdata_dtor);
        if (rc != 0)
            throwLastError<std::invalid_argument>("heliosview subscribe");
    }

    template <class Req = void, class Fn>
    void subscribeJson(const char* name, Fn&& callback);

    template <class Req = void, class Obj, class MFPtr>
    void subscribeJson(const char* name, Obj* obj, MFPtr method);

    void unsubscribe(const char* name)
    {
        if (m_webview)
            heliosview_webview_unsubscribe(m_webview, name);
    }

    /* ===== Signals ===== */

    // Window signals
    Signal<> firstShown;
    Signal<> closeRequested;
    Signal<int32_t, int32_t> resized;
    Signal<int32_t, int32_t> moved;
    Signal<int32_t, int32_t> moving;
    Signal<int32_t, int32_t> sizing;
    Signal<> minimized;
    Signal<> maximized;
    Signal<> restored;
    Signal<> shown;
    Signal<> hidden;
    Signal<> focused;
    Signal<> blurred;
    Signal<bool> enabledChanged;
    Signal<KeyCode> keyPressed;
    Signal<KeyCode> keyReleased;
    Signal<KeyCode> keyRepeated;
    Signal<const KeyEvent&> keyEvent;
    Signal<const std::string&> textInput;
    Signal<int32_t, int32_t> mouseMoved;
    Signal<int32_t, int32_t, MouseButton> mouseButtonPressed;
    Signal<int32_t, int32_t, MouseButton> mouseButtonReleased;

    // WebView navigation signals
    Signal<std::string, bool, bool> navigationStarting;
    Signal<std::string, bool> urlChanged;
    Signal<std::string> titleChanged;
    Signal<int> navigationCompleted;
    std::function<bool(const std::string&, bool, bool)> navigationStartingGate;

    template <class Obj, class Ret>
    void connectNavigation(Ret Obj::* member, Obj* obj)
    {
        navigationCompleted.connect(member, obj);
    }

    template <class Obj, class Ret>
    void connectStarting(Ret Obj::* member, Obj* obj)
    {
        navigationStarting.connect(member, obj);
    }

    // Event dispatch
    virtual bool event(const Event& e)
    {
        /* Keyboard events reach the window, not the focused child viewport, so a window
         * that hosts a widget tree forwards them to its UI hosts here. Only the host
         * whose widget holds focus consumes the event; the text itself arrives through
         * WM_CHAR directly at the host, which is why only key events are routed. */
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) {
            const bool isDown = (e.type == EventType::KeyDown);
            for (heliosview_host_t* host : m_uiHosts) {
                if (heliosview_host_ui_dispatch_key(host, static_cast<int>(e.key), e.modifiers, isDown ? 1 : 0))
                    return true;
            }
        }

        switch (e.type) {
        case EventType::WindowFirstShown:
            firstShown();   /* the native window was displayed for the first time */
            return true;
        case EventType::WindowResize:
            resized(e.width, e.height);
            return true;
        case EventType::WindowMoved:
            moved(e.x, e.y);
            return true;
        case EventType::WindowMoving:
            moving(e.x, e.y);
            return true;
        case EventType::WindowSizing:
            sizing(e.width, e.height);
            return true;
        case EventType::WindowFocus:
            focused();
            return true;
        case EventType::WindowBlur:
            blurred();
            return true;
        case EventType::WindowEnabled:
            enabledChanged(true);
            return true;
        case EventType::WindowDisabled:
            enabledChanged(false);
            return true;
        case EventType::WindowMinimized:
            minimized();
            return true;
        case EventType::WindowMaximized:
            maximized();
            return true;
        case EventType::WindowRestored:
            restored();
            return true;
        case EventType::WindowShown:
            shown();
            return true;
        case EventType::WindowHidden:
            hidden();
            return true;
        case EventType::KeyDown: {
            const bool repeat = (e.flags & HELIOSVIEW_EVENT_FLAG_KEY_REPEAT) != 0;
            keyEvent(KeyEvent{e.key, e.modifiers, /*pressed=*/true, repeat});
            if (repeat)
                keyRepeated(e.key);
            else
                keyPressed(e.key);
            return true;
        }
        case EventType::KeyUp:
            keyEvent(KeyEvent{e.key, e.modifiers, /*pressed=*/false, /*repeat=*/false});
            keyReleased(e.key);
            return true;
        case EventType::TextInput:
            textInput(std::string(e.text, e.textLen));
            return true;
        case EventType::MouseMove:
            mouseMoved(e.x, e.y);
            return true;
        case EventType::MouseButtonDown:
            mouseButtonPressed(e.x, e.y, e.mouseButton);
            return true;
        case EventType::MouseButtonUp:
            mouseButtonReleased(e.x, e.y, e.mouseButton);
            return true;
        case EventType::WindowClose:
            closeRequested();   /* emit signal; call close() in the handler to actually close */
            return true;
        default:
            return false; /* Quit and other non-window events */
        }
    }

private:
    // Destroy one tracked child host and drop it from the tracked list
    void detachHost(heliosview_host_t* host)
    {
        if (!host) return;
        auto it = std::find(m_hosts.begin(), m_hosts.end(), host);
        if (it != m_hosts.end())
            m_hosts.erase(it);
        auto uit = std::find(m_uiHosts.begin(), m_uiHosts.end(), host);
        if (uit != m_uiHosts.end())
            m_uiHosts.erase(uit);
        heliosview_host_destroy(host);
    }

    // A child host is created against this window's own handle
    friend class UIHost;

    static void cookiesTrampoline(int error, const heliosview_webview_cookie_t* cookies,
                                  size_t count, void* userdata)
    {
        std::unique_ptr<CookiesFn> fn(static_cast<CookiesFn*>(userdata));
        if (*fn) (*fn)(error, cookies, count);
    }

    static void cookieOpTrampoline(int error, void* userdata)
    {
        std::unique_ptr<CookieOpFn> fn(static_cast<CookieOpFn*>(userdata));
        if (*fn) (*fn)(error);
    }

    void wireWebViewEvents()
    {
        if (!m_webview) return;

        heliosview_webview_set_navigation_callback(
            m_webview,
            [](heliosview_webview_t* wv, int error, void* userdata) {
                static_cast<Window*>(userdata)->navigationCompleted(error);
            },
            this, nullptr);

        heliosview_webview_set_navigation_starting_callback(
            m_webview,
            [](heliosview_webview_t* wv, const char* uri, int is_redirected,
               int is_user_initiated, void* userdata) -> int {
                auto* self = static_cast<Window*>(userdata);
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
                auto* self = static_cast<Window*>(userdata);
                self->urlChanged(uri ? uri : "", is_new_document != 0);
            },
            this, nullptr);

        heliosview_webview_set_title_changed_callback(
            m_webview,
            [](heliosview_webview_t* wv, const char* title, void* userdata) {
                static_cast<Window*>(userdata)->titleChanged(title ? title : "");
            },
            this, nullptr);

        heliosview_webview_set_context_menu_callback(
            m_webview,
            [](heliosview_webview_t* wv, const heliosview_context_menu_info_t* info,
               void* userdata) -> int {
                auto* self = static_cast<Window*>(userdata);
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
                    return 0;
                }
            },
            this, nullptr);
    }

    heliosview_window_t* m_window = nullptr;
    heliosview_webview_t* m_webview = nullptr;
    std::vector<heliosview_host_t*> m_hosts;
    std::vector<heliosview_host_t*> m_uiHosts; /* the subset carrying a widget tree */
};

/* ---------- App message-loop callback ---------- */

inline int App::loopCallback(void* userdata)
{
    auto* self = static_cast<App*>(userdata);

    /* Application frame logic (animation, repaint requests) runs once per iteration,
     * before that iteration's events are dispatched. */
    if (self->frameCallback)
        self->frameCallback();

    Event ev;
    while (self->pollEvent(ev)) {
        if (ev.type == EventType::Quit) {
            self->quit();
            break;
        }
        bool handled = false;
        for (const auto& [id, sink] : self->m_sinks)
            if (sink && sink(ev)) { handled = true; break; }
        if (handled)
            continue;

        if (heliosview_window_t* win = heliosview_window_from_id(ev.windowId)) {
            if (auto* w = static_cast<Window*>(heliosview_window_userdata(win))) {
                if (w->event(ev))
                    continue;
            }
        }
        self->event(ev);
    }

    self->drainTasks();

    if (heliosview_window_count() == 0) {
        self->quit();
        return 1;
    }
    return 0;
}

} // namespace helios
