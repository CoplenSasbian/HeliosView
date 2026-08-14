#pragma once

/**
 * HeliosView.Core -- Window: top-level window.
 *
 * Depends on: Signal.h, Types.h, App.h (window registry).
 * Events are dispatched to signals via event(); connect via window.keyPressed.connect(...).
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/System.h> /* Rect (work-area query) */
#include <HeliosViewCore/Types.h>

#include <cstdint>
#include <string>

namespace helios {

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
    // Construct a window with the given client size, title (UTF-8) and preset
    // style. The native window is created lazily on the first show(). This
    // object is stored as userdata on the C-layer window, which dispatches
    // events through it.
    Window(int width, int height, const char* title,
           WindowStyle style = WindowStyle::Normal)
        : m_window(heliosview_window_create_ex(width, height, title,
                                               static_cast<heliosview_window_style_t>(style),
                                               this))
    {
    }

    // Destroy the window (closes the native window if it was shown)
    virtual ~Window() { close(); }

    // Non-copyable: a Window uniquely owns its native window
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Move: transfers the native window; event dispatch continues to follow the new object
    Window(Window&& other) noexcept : m_window(other.m_window)
    {
        other.m_window = nullptr;
        heliosview_window_set_userdata(m_window, this); /* after move, userdata points to the new object (null check lives in C layer) */
    }

    Window& operator=(Window&& other) noexcept
    {
        if (this != &other) {
            close();
            m_window = other.m_window;
            other.m_window = nullptr;
            heliosview_window_set_userdata(m_window, this);
        }
        return *this;
    }

    // Create (if not yet created) and show the native window.
    // The underlying C call reports errors as a return code (0 = success);
    // use heliosview_window_show directly if you need to inspect it.
    void show() { heliosview_window_show(m_window); }

    // Hide the native window (keeps it alive; show()/showState() bring it back).
    void hide() { heliosview_window_hide(m_window); }

    // ---- window operations ----

    // Show the window in the given state
    void showState(ShowState state)
    {
        heliosview_window_show_state(m_window, static_cast<heliosview_show_state_t>(state));
    }
    // Show normally / minimized / maximized (convenience for showState)
    void showNormal() { showState(ShowState::Normal); }
    void showMinimized() { showState(ShowState::Minimized); }
    void showMaximized() { showState(ShowState::Maximized); }

    // The current show state of the window
    ShowState state() const
    {
        return static_cast<ShowState>(heliosview_window_state(m_window));
    }

    // Minimize / maximize / restore the window (convenience over showState)
    void minimize() { heliosview_window_minimize(m_window); }
    void maximize() { heliosview_window_maximize(m_window); }
    void restore() { heliosview_window_restore(m_window); }

    // Toggle between normal and maximized (e.g. for a title-bar maximize button)
    void toggleMaximize() { heliosview_window_toggle_maximize(m_window); }

    // Enable/disable user resizing (and the maximize box). Applied immediately
    // on a shown window; honored at creation otherwise.
    void setResizable(bool resizable) { heliosview_window_set_resizable(m_window, resizable ? 1 : 0); }

    // ---- frameless dragging ----

    // Register a client-area drag region: mouse-down + drag inside it moves the
    // window like a title bar (WM_NCHITTEST -> HTCAPTION). Call this for each
    // custom title-bar strip of a frameless/borderless window.
    void addDragRegion(int32_t x, int32_t y, int32_t width, int32_t height)
    {
        heliosview_window_add_drag_region(m_window, x, y, width, height);
    }

    // Remove all registered drag regions
    void clearDragRegions() { heliosview_window_clear_drag_regions(m_window); }

    // ---- custom title-bar control buttons ----

    // Register a client-area rectangle for a window control (minimize / maximize
    // / close). The library wires it to the OS title-bar button behavior: a click
    // performs the action (maximize auto-toggles with the window state) and never
    // starts a drag. The button's *look* is yours to draw; this only handles the
    // hit-testing + behavior.
    void addControlButton(ControlButton button, int32_t x, int32_t y,
                          int32_t width, int32_t height)
    {
        heliosview_window_add_control_button(
            m_window, static_cast<heliosview_control_button_t>(button), x, y, width, height);
    }

    // Remove all registered control buttons
    void clearControlButtons() { heliosview_window_clear_control_buttons(m_window); }

    // Draw the registered control buttons the modern Windows way (Segoe MDL2
    // glyphs — the same ones as the system title bar — with hover/pressed
    // feedback following the light/dark theme), as child windows layered above
    // the WebView. The alternative is drawing them yourself in the client area.
    // Call after show(). Pass false to return to app-drawn buttons.
    void enableNativeButtons(bool on) { heliosview_window_enable_native_buttons(m_window, on ? 1 : 0); }

    // The window's DPI (per-monitor; 0 if not created)
    uint32_t dpi() const { return heliosview_window_dpi(m_window); }

    // Work area (excluding taskbar) of the monitor the window is on.
    // Returns false if the window is not created.
    bool workArea(Rect& out) const
    {
        heliosview_rect_t r{};
        if (heliosview_window_work_area(m_window, &r) != 0)
            return false;
        out = {r.x, r.y, r.width, r.height};
        return true;
    }

    // ---- size constraints ----

    // Enforce a minimum client size (so the UI is not crushed). 0 = unconstrained.
    void setMinimumSize(int32_t w, int32_t h) { heliosview_window_set_min_size(m_window, w, h); }

    // Enforce a maximum client size. (0, 0) = no maximum.
    void setMaximumSize(int32_t w, int32_t h) { heliosview_window_set_max_size(m_window, w, h); }

    // ---- taskbar flash ----

    // Flash the taskbar button a few times (background task finished).
    void flash() { heliosview_window_flash(m_window); }

    // Flash the taskbar button until the window is focused (urgent notification).
    void flashUntilFocus() { heliosview_window_flash_until_focus(m_window); }

    // ---- fullscreen ----

    // Enter (true) or leave (false) fullscreen. The previous geometry/style are
    // restored on exit. A fullscreen window covers the whole monitor.
    void setFullscreen(bool on) { heliosview_window_set_fullscreen(m_window, on ? 1 : 0); }

    // Whether the window is currently fullscreen.
    bool isFullscreen() const { return heliosview_window_is_fullscreen(m_window) != 0; }

    // ---- enabled / modal ----

    // Enable or disable the window (a disabled window is locked against input;
    // used for modal states). Fires enabledChanged.
    void setEnabled(bool on) { heliosview_window_set_enabled(m_window, on ? 1 : 0); }

    // Whether the window is enabled.
    bool isEnabled() const
    {
        return heliosview_window_is_enabled(m_window) != 0;
    }

    // Request to close the window; goes through the event pipeline
    // (WINDOW_CLOSE -> event()), so it can be vetoed by overriding event().
    void requestClose() { heliosview_window_close(m_window); }

    // Give the window focus (foreground activation + keyboard focus)
    void focus() { heliosview_window_focus(m_window); }

    // Keep the window always on top (or restore normal z-order)
    void setTopmost(bool on) { heliosview_window_set_topmost(m_window, on ? 1 : 0); }

    // True if the window is currently visible (false when not shown / not created)
    bool isVisible() const { return heliosview_window_is_visible(m_window) != 0; }

    // ---- position and size ----

    // Move the window so its top-left corner is at screen coordinates (x, y)
    void move(int32_t x, int32_t y) { heliosview_window_set_position(m_window, x, y); }

    // Query the window position in screen coordinates (top-left corner).
    // Returns true on success, false on failure (x/y unchanged).
    bool position(int32_t& x, int32_t& y) const
    {
        return heliosview_window_position(m_window, &x, &y) == 0;
    }

    // Resize the window's client area
    void resize(int32_t width, int32_t height) { heliosview_window_set_size(m_window, width, height); }

    // Query the window client size. Returns true on success, false on failure (width/height unchanged).
    bool size(int32_t& width, int32_t& height) const
    {
        return heliosview_window_size(m_window, &width, &height) == 0;
    }

    // Set position and size in one call
    void setGeometry(int32_t x, int32_t y, int32_t width, int32_t height)
    {
        move(x, y);
        resize(width, height);
    }

    // Query position and size. Returns true only if both queries succeed.
    bool geometry(int32_t& x, int32_t& y, int32_t& width, int32_t& height) const
    {
        return position(x, y) && size(width, height);
    }

    // ---- other window operations ----

    // Set the window title (UTF-8).
    void setTitle(const char* title) { heliosview_window_set_title(m_window, title); }
    void setTitle(const std::string& title) { heliosview_window_set_title(m_window, title.c_str()); }

    // Center the window on the current monitor's work area
    void center() { heliosview_window_center(m_window); }

    // Set the window opacity (0.0 fully transparent to 1.0 opaque)
    void setOpacity(float opacity) { heliosview_window_set_opacity(m_window, opacity); }

    // Replace the window icon (an .ico/.cur path, UTF-8; nullptr = default)
    void setIcon(const char* icon_path) { heliosview_window_set_icon(m_window, icon_path); }

    // ---- taskbar progress ----

    // Show a determinate taskbar progress indicator (value of max; clamped)
    void setProgress(uint32_t value, uint32_t max) { heliosview_window_set_progress(m_window, value, max); }

    // Change only the progress visual state (indeterminate / paused / error / ...)
    void setProgressState(ProgressState state)
    {
        heliosview_window_set_progress_state(m_window, static_cast<heliosview_progress_state_t>(state));
    }

    // Remove the taskbar progress indicator
    void clearProgress() { heliosview_window_clear_progress(m_window); }

    // ---- backdrop & dark mode (Win11 DWM; returns 0 on success) ----

    // Apply a system backdrop (Mica / Acrylic); negative on unsupported systems.
    int setBackdrop(Backdrop backdrop)
    {
        return heliosview_window_set_backdrop(m_window, static_cast<heliosview_backdrop_t>(backdrop));
    }

    // Toggle the immersive dark-mode title bar.
    int setDarkMode(bool on) { return heliosview_window_set_dark_mode(m_window, on ? 1 : 0); }

    // Close and destroy the native window. If it was the last window, the message
    // loop exits (null check lives in the C layer). Idempotent (also called by the destructor).
    void close()
    {
        heliosview_window_destroy(m_window);
        m_window = nullptr;
    }

    // The window id (the windowId field of events targeting this window; 0 before creation)
    int32_t id() const { return heliosview_window_id(m_window); }

    // The raw C window handle (nullptr if not created/closed). Exposed so the
    // low-level API (e.g. helios::Tray) can be used directly on the native handle.
    heliosview_window_t* nativeHandle() const { return m_window; }

    /* ===== signals (window.keyPressed.connect(...)) ===== */

    Signal<> closed;                                       // close requested (user clicked X); default handling destroys the window
    Signal<int32_t, int32_t> resized;                      // size changed (w, h)
    Signal<int32_t, int32_t> moved;                        // moved; final position (x, y)
    Signal<int32_t, int32_t> moving;                       // move drag in progress (x, y)
    Signal<int32_t, int32_t> sizing;                       // resize drag in progress (w, h)
    Signal<> focused;                                      // window gained focus (activated)
    Signal<> blurred;                                      // window lost focus (deactivated)
    Signal<bool> enabledChanged;                           // enabled (true) / disabled (false)
    Signal<KeyCode> keyPressed;                            // key pressed (auto-repeat filtered)
    Signal<KeyCode> keyReleased;                           // key released
    Signal<int32_t, int32_t> mouseMoved;                   // mouse moved (x, y)
    Signal<int32_t, int32_t, MouseButton> mouseButtonPressed;  // pressed (x, y, button)
    Signal<int32_t, int32_t, MouseButton> mouseButtonReleased; // released (x, y, button)

    // Window event handler. The default implementation emits signals from events.
    // Return true if handled; unhandled events go to App::event().
    // To veto a close, override this function, intercept WindowClose, and return true.
    virtual bool event(const Event& e)
    {
        switch (e.type) {
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
        case EventType::KeyDown:
            keyPressed(e.key);
            return true;
        case EventType::KeyUp:
            keyReleased(e.key);
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
            closed();
            close(); /* default: a close request destroys the window (App exits after the last one closes) */
            return true;
        default:
            return false; /* Quit and other non-window events */
        }
    }

private:
    heliosview_window_t* m_window = nullptr;
};

/* ---------- App message-loop callback (defined here: needs the complete Window type) ----------
 * Events are dispatched via the C-layer window userdata (a Window object pointer); no C++-side registry */

inline int App::loopCallback(void* userdata)
{
    auto* self = static_cast<App*>(userdata);

    // Pump queued events to their target windows; unhandled ones go to App::event()
    Event ev;
    while (self->pollEvent(ev)) {
        if (ev.type == EventType::Quit) {
            self->quit();
            break;
        }
        /* app-level extension sinks first: decoupled objects (a Tray attached to a
         * raw window handle, without a C++ Window) handle events here. */
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

    // Idle: run scheduled tasks (the path for background tasks to return to the UI thread)
    self->drainTasks();

    // All windows closed -> exit the loop
    if (heliosview_window_count() == 0) {
        self->quit();
        return 1;
    }
    return 0;
}

} // namespace helios
