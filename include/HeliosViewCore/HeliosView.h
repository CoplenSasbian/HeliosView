#pragma once

/**
 * HeliosView.Core -- header-only C++ wrapper layer (single entry point).
 *
 * ABI stability comes from the underlying HeliosView.dll C interface;
 * this header provides a type-safe wrapper. Features live in separate
 * headers, so you can include only what you need, or this file (recommended):
 *
 *   - Signal.h        signals/slots (std::function + C++23 std::flat_set)
 *   - Types.h         event types and structures (1:1 with the C interface)
 *   - String.h        UTF-8 helpers (and UTF-8 <-> wchar_t conversion)
 *   - App.h           message loop + UI-thread scheduler (std::execution)
 *   - Window.h        top-level window + signals + taskbar/backdrop APIs
 *   - Dialogs.h       native dialogs (folder/file pickers, message box)
 *   - System.h        system helpers (open URL, show in folder, clipboard)
 *   - Notification.h  OS toast notifications (thread-safe)
 *   - Tray.h          system notification-area (tray) icon + signals + balloon
 *   - Menu.h          popup / context menu + signals
 *   - Execution.h     C++26 <execution> (P2300) compat layer: unified std::execution namespace
 *   - WebViewWindow.h window embedding a WebView (win32: WebView2)
 *   - WebViewJson.h   Boost.JSON auto-binding sugar for the WebView bridge (bindJson / subscribeJson)
 *
 * Threading: every Window / WebView / Tray / Menu / Dialog / event API must be
 * called on the message-loop thread (the thread running App::exec). The
 * exceptions -- safe from any thread -- are App::postTask, App::quit,
 * WebView resolve/reject/broadcast, and the notification functions.
 *
 * Usage (signals/slots):
 *   helios::App app;
 *   helios::Window win(800, 600, "title");
 *   win.keyPressed.connect(...);
 *   win.show();
 *   return app.exec();   // message loop; exits after the last window closes
 *
 * All functions are inline; no extra library to link (except HeliosView.dll,
 * which comes in via the HeliosView::Core CMake target automatically).
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Async.h>
#include <HeliosViewCore/Dialogs.h>
#include <HeliosViewCore/Execution.h>
#include <HeliosViewCore/Menu.h>
#include <HeliosViewCore/Notification.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/String.h>
#include <HeliosViewCore/System.h>
#include <HeliosViewCore/Tray.h>
#include <HeliosViewCore/Types.h>
#include <HeliosViewCore/WebViewJson.h>
#include <HeliosViewCore/WebViewWindow.h>
#include <HeliosViewCore/Window.h>

#include <string>

namespace helios {

/* ---------- misc ---------- */

// The library version as a string, e.g. "1.0.0"
inline std::string version()
{
    return heliosview_version();
}

} // namespace helios
