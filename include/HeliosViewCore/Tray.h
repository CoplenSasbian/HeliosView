#pragma once

/**
 * HeliosView.Core -- Tray: a notification-area (system tray) icon.
 *
 * Attached to a created (shown) native window via window.nativeHandle(); the
 * icon lives until the Tray is destroyed or its window is destroyed (which
 * removes any trays attached to it). Mouse events on the icon are delivered
 * as TRAY_* events through the app's extension-sink registry, so a Tray works
 * without a C++ Window wrapper — it only needs the raw window handle.
 *
 * Signals run on the message-loop thread:
 *   tray.leftClicked, tray.leftDoubleClicked, tray.rightClicked, tray.middleClicked
 *
 * Usage (from the README):
 *   helios::Window window(800, 600, "Tray Demo");
 *   window.show();                        // window must exist first
 *   helios::Tray tray(window.nativeHandle(), "Tray Demo");   // tooltip
 *   tray.leftClicked.connect([] { ... });
 *   tray.rightClicked.connect([&] { ... context menu ... });
 *
 * Destroy the tray before its window.
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/Types.h>

namespace helios {

/* Balloon notification icon type (mirrors heliosview_tray_notify_icon_t) */
enum class NotifyIcon : int32_t {
    None = HELIOSVIEW_TRAY_NOTIFY_NONE,
    Info = HELIOSVIEW_TRAY_NOTIFY_INFO,
    Warning = HELIOSVIEW_TRAY_NOTIFY_WARNING,
    Error = HELIOSVIEW_TRAY_NOTIFY_ERROR,
};

class Tray {
public:
    // Attach a tray icon to `window` (must already be created/shown) with the
    // given tooltip (UTF-8). icon_path is an .ico/.cur path, or nullptr for
    // the default application icon. Not copyable/movable: a Tray owns its icon.
    Tray(heliosview_window_t* window, const char* tooltip, const char* icon_path = nullptr)
        : m_tray(heliosview_tray_create(window, tooltip, icon_path, this))
    {
        if (m_tray)
            m_sink = App::instance()->addSink([this](const Event& ev) { return handleEvent(ev); });
    }

    ~Tray()
    {
        if (m_sink != 0)
            App::instance()->removeSink(m_sink);
        heliosview_tray_destroy(m_tray);
    }

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // True when the tray was created successfully (window was created/shown)
    bool valid() const { return m_tray != nullptr; }

    // Update the tooltip / replace the icon (nullptr = default icon)
    void setTooltip(const char* tooltip) { heliosview_tray_set_tooltip(m_tray, tooltip); }
    void setIcon(const char* icon_path) { heliosview_tray_set_icon(m_tray, icon_path); }

    // Show a balloon notification next to the icon (works with no setup,
    // unlike OS toasts). Message-loop thread. Returns true when the OS accepted
    // it (false when suppressed, e.g. notifications off / Focus assist).
    bool notify(const char* title, const char* message,
                NotifyIcon iconType = NotifyIcon::Info, uint32_t timeoutMs = 0)
    {
        return heliosview_tray_notify(m_tray, title, message,
                                      static_cast<heliosview_tray_notify_icon_t>(iconType),
                                      timeoutMs) == 0;
    }

    // ---- signals (UI thread) ----
    Signal<> leftClicked;         // tray icon left-click
    Signal<> leftDoubleClicked;   // tray icon left-double-click
    Signal<> rightClicked;        // tray icon right-click (typically opens a context menu)
    Signal<> middleClicked;       // tray icon middle-click

private:
    // Route TRAY_* events for this tray to the matching signal
    bool handleEvent(const Event& ev)
    {
        if (ev.userdata != this)
            return false;
        switch (ev.type) {
        case EventType::TrayLeftClick:       leftClicked(); return true;
        case EventType::TrayLeftDoubleClick: leftDoubleClicked(); return true;
        case EventType::TrayRightClick:      rightClicked(); return true;
        case EventType::TrayMiddleClick:     middleClicked(); return true;
        default:                             return false;
        }
    }

    heliosview_tray_t* m_tray = nullptr;
    App::SinkId m_sink = 0;
};

} // namespace helios
