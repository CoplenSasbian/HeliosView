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
 *   - App.h           message loop + event queue
 *   - Window.h        top-level window + signals
 *   - Dialogs.h       native dialog helpers (folder picker, ...)
 *   - Tray.h          system notification-area (tray) icon + signals
 *   - Menu.h          popup / context menu + signals
 *   - Execution.h     C++26 <execution> (P2300) compat layer: unified std::execution namespace
 *   - Async.h         background async I/O: thread pool + platform multiplexer (socket/file, sender-based)
 *   - Http.h          async HTTP client (GET/POST/..., https, sender-based + callback API)
 *   - WebViewWindow.h window embedding a WebView (win32: WebView2)
 *   - WebViewJson.h   nlohmann auto-binding sugar for the WebView bridge (bindJson / subscribeJson)
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
#include <HeliosViewCore/Http.h>
#include <HeliosViewCore/Menu.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/Tray.h>
#include <HeliosViewCore/Types.h>
#include <HeliosViewCore/WebViewJson.h>
#include <HeliosViewCore/WebViewWindow.h>
#include <HeliosViewCore/Window.h>

#include <string>

namespace helios {

/* ---------- misc ---------- */

// The library version as a string, e.g. "0.1.0"
inline std::string version()
{
    return heliosview_version();
}

} // namespace helios
