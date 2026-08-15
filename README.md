# HeliosView

> **English | [简体中文](README_zh-CN.md)**

A C++ **WebView** windowing library: embed a webview, drive it from C++ or C, and
build the desktop shell around it — windows, tray icons, menus, dialogs,
notifications, taskbar progress, system integrations. Two layers:

- **`HeliosView.dll`** — a pure **C API** (stable ABI). Windows and events, the
  WebView bridge, tray/menu, native dialogs and system helpers (clipboard,
  open-URL, toasts, taskbar progress, DWM backdrop). Platform-specific code
  lives in per-platform backends (currently `src/win32/`: Win32 window/message
  loop, WebView2, IFileDialog, WinRT toasts).
- **`HeliosView.Core`** — a **header-only C++ wrapper** built on the C API:
  a WebView bridge with **nlohmann auto-binding** (`bindJson`), signals/slots,
  a `std::execution` scheduler for the message loop, and thin wrappers for
  every C API.

Include one header and link one CMake target:

```cpp
#include <HeliosViewCore/HeliosView.h>
```

```cmake
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

Everything here is Windows (win32); the C API is the porting boundary — other
platforms re-implement `src/<platform>/` behind it.

---

## Threading model

**All window / WebView / tray / menu / dialog / event-queue APIs must be called
on the message-loop thread** — the thread running `App::exec()` (C: the thread
that called `heliosview_run`). Calling them from another thread is undefined
behavior.

The **exceptions** (safe from any thread):

- `App::postTask(fn)` — the sanctioned way to return to the UI thread from a
  worker (`app.quit()` too).
- WebView `resolve` / `reject` / `broadcast`.
- Notifications (`notificationShow` / `heliosview_notification_show`) — OS
  toasts are thread-agnostic.
- `heliosview_free`.

Everything else: UI thread only.

```cpp
helios::App app;
// a background worker returning to the UI thread:
std::thread worker([app] {
    do_slow_work();
    app->postTask([] { /* runs on the UI thread while the loop is idle */ });
});
```

---

## Feature overview

| area | API (C / C++) |
| --- | --- |
| windows + events | `heliosview_window_*` / `helios::Window` (styles, opacity, icon, topmost, hide, min/max/restore, resizable, min/max size, drag regions, fullscreen, flash, enabled, DPI, focus/move/size events) |
| screen geometry | `heliosview_*_work_area` / `System::screenWorkArea` / `Window::workArea` (multi-monitor) |
| taskbar progress | `heliosview_window_set_progress` / `Window::setProgress` (+ state, overlay-capable) |
| session end | `heliosview_set_session_end_callback` / `System::setSessionEndCallback` (save-on-shutdown) |
| backdrop & dark mode (Win11) | `heliosview_window_set_backdrop/_dark_mode` / `Window::setBackdrop/setDarkMode` |
| WebView + JS bridge | `heliosview_webview_*` / `WebViewWindow` + `bindJson` auto-binding |
| tray icon + menu | `heliosview_tray_*` / `heliosview_menu_*` / `Tray` / `Menu` |
| dialogs | folder/file pickers, message box (`Dialogs.h`) |
| system helpers | clipboard, open-URL, show-in-folder (`System.h`) |
| notifications (toasts) | `heliosview_notification_*` / `Notification.h` (any thread) |
| message loop, `std::execution` scheduler | `heliosview_run` / `App` |

---

## Memory allocation

HeliosView routes **all of its allocations through a single configurable
allocator**, so they can come from a pool, arena, or other allocator instead of
the process heap. This is one mechanism shared by the C API and the C++
wrapper: configure it once and everything the library allocates follows.

**The rule of thumb:** whatever you configure, **allocate and free must use the
same allocator**, and you must configure it **before creating anything**
(changing it while objects are alive is undefined behavior).

- **C** — `heliosview_set_allocator(&heliosview_allocator_t)` replaces the
  library's default `malloc`/`free`. Strings the library hands you (dialog
  paths, clipboard text) must be freed with **`heliosview_free`** — never the
  platform `free()` (CRT heaps may differ across the DLL boundary).

  ```c
  char* path = NULL;
  if (heliosview_select_folder(NULL, "Pick a folder", &path) == 1) {
      printf("folder: %s\n", path);
      heliosview_free(path);   /* always pair library-returned strings with this */
  }
  ```

- **C++** — the wrapper uses the same C allocator; the C++ strings it produces
  are ordinary `std::string` (UTF-8).

> **One line:** call `heliosview_set_allocator` before creating anything, and
> free every library-returned string with `heliosview_free`.

---

## Building

Requires CMake ≥ 4.3 and a C++23 compiler (C99 for the C demo). All third-party
dependencies are vendored or auto-fetched — **no vcpkg, no system package
install**:

| dependency | version | source | used for |
| --- | --- | --- | --- |
| `nlohmann/json` | 3.12 | vendored (`third_party/json/`) | WebView bridge auto-binding (`bindJson`) |
| WebView2 SDK | 1.0.4129.50 | downloaded from NuGet at configure time | embedded WebView (win32) |
| `stdexec` | pinned commit b783aac (Mar 2024) | vendored (`third_party/stdexec/`) | C++23 coroutines (senders/receivers) |

Everything else comes from the OS: windowing, dialogs, toasts (WinRT via the
Windows SDK), DWM backdrop. The WebView2 SDK is the only thing fetched at
configure time (a `.nupkg` is just a zip of headers + the WebView2Loader
library), cached in the build directory.

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

DLLs and demos land in `build/bin/` together, so no `PATH` setup is needed.

`HELIOSVIEW_BUILD_EXAMPLES=ON` (default) builds the demo programs:

| demo | file | shows |
| --- | --- | --- |
| `HeliosViewDemo` | `examples/main.cpp` | signals + the threading contract (worker → `postTask`) |
| `HeliosViewWindowDemo` | `examples/window_demo.cpp` | basic window + signal/slot + tray + menu |
| `HeliosViewAppDemo` | `examples/app_demo.cpp` | `Window` subclassing, window styles, member slots, window APIs |
| `HeliosViewSystemDemo` | `examples/system_demo.cpp` | dialogs, clipboard, toasts, taskbar progress, tray balloon |
| `HeliosViewWebViewDemo` | `examples/webview_demo.cpp` | **WebView + `bindJson` auto-binding** |
| `HeliosViewWebViewEventsDemo` | `examples/webview_events_demo.cpp` | navigation events, local folder mapping, folder dialog |
| `HeliosViewCDemo` | `examples/c_demo.c` | **pure C** consumer |

---

## Tutorial

The tutorial is ordered by dependency: **App** (message loop) → **Signals** →
**Window** → **WebView** (the core of the library), then the system APIs and the
C API behind it all.

### 1. App + message loop

`helios::App` is the process's single application object: it owns the message
loop and the UI-thread task queue. Everything else (windows, webviews, ...)
dispatches through it.

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main()
{
    helios::App app;                       // exactly one App per process

    app.postTask([] { std::println("hello from the UI thread"); });

    return app.exec();                     // message loop; returns on quit()
                                           // or when the last window closes
}
```

`exec()` runs the message loop (returns 0 on normal exit); `quit()` requests
exit and may be called from any thread. `pollEvent` / `waitEvent` /
`postEvent` give raw queue access; override `event()` for app-wide unhandled
events. The message loop is also a `std::execution::scheduler`:
`std::execution::schedule(app.get_scheduler()) | std::execution::then(fn)`.

### 2. Signals and slots

`helios::Signal<Args...>` holds slots and invokes them on emission; `connect`
returns a slot id for `disconnect(id)`:

```cpp
helios::Signal<int32_t, int32_t> resized;
resized.connect([](int32_t w, int32_t h) { std::println("resized {}x{}", w, h); });
resized(800, 600);
```

Slots come in three flavors — sync callables, member functions
(`connect(&MyWindow::onKeyPressed, this)`), and **async slots** (callables
returning a sender, started fire-and-forget). Window, WebView, Tray, ... expose
their events as ready-made signals.

### 3. Window

`helios::Window` is a top-level window driven by the App's message loop.
Signals report input:

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main()
{
    helios::App app;
    helios::Window window(800, 600, "Hello");
    window.show();

    window.resized.connect([](int32_t w, int32_t h) { std::println("resized {}x{}", w, h); });
    window.keyPressed.connect([&](helios::KeyCode key) {
        std::println("key {}", static_cast<int>(key));
        if (key == helios::KeyCode::Escape)
            window.close();          // last window closes -> loop exits
    });

    return app.exec();
}
```

`Window` also offers `showMinimized/Maximized/Normal` (and the convenience
`minimize`/`maximize`/`restore`/`toggleMaximize`), `move/resize`,
`position/size/geometry`, `setTitle`, `center`, `setOpacity`, `focus`, `hide`,
`setTopmost`, `setIcon`, `requestClose`, `setResizable`, `setProgress`
(taskbar), `setBackdrop(Mica/Acrylic)` + `setDarkMode` (Win11), `dpi`, and
`WindowStyle::{Normal, Borderless, Frameless}`. `focused`/`blurred` signals
report activation changes. Titles and strings are UTF-8.

**Frameless dragging.** A frameless/borderless window has no OS title bar, so
register the custom title-bar strips as drag regions — a mouse-down + drag
inside them moves the window like a native title bar (`WM_NCHITTEST →
HTCAPTION`):

```cpp
helios::Window win(480, 320, "Frameless", helios::WindowStyle::Frameless);
win.addDragRegion(0, 0, 480, 40);          // the title-bar strip
win.show();
```

**Built-in control buttons.** The `FramelessWithButtons` style gives a
frameless window (no system title bar, WebView fills the whole client area)
with minimize / maximize / close buttons already drawn at the top-right corner
(MDL2 glyphs — the same ones as the Win10/11 title bar — with hover/pressed
feedback following the light/dark theme), and the top strip drags the window:

```cpp
helios::Window win(480, 320, "Frameless", helios::WindowStyle::FramelessWithButtons);
win.show();
```

**Custom control buttons.** Or draw your own buttons and register their
rectangles — the library wires them to the real title-bar behavior (click =
action, never drags; maximize/restore auto-toggles).

**DPI.** Call `helios::enableDpiAwareness()` once, before creating any window,
to make the process per-monitor DPI aware (v2); `window.dpi()` reports a
window's current DPI.

**Screen geometry.** `System::screenWorkArea`, `Window::workArea`, and
`System::primaryWorkArea` return the monitor's usable area (excluding the
taskbar) in screen coordinates — handy for centering/positioning windows on
multi-monitor setups. `System::cursorPosition` reports the mouse location.

**Size limits, fullscreen, flash, and modal lock.** `setMinimumSize` /
`setMaximumSize` clamp the client size (`WM_GETMINMAXINFO`);
`setFullscreen` covers the whole monitor and restores the previous geometry on
exit; `flash` / `flashUntilFocus` flash the taskbar button (a finished
background task or an urgent notification); `setEnabled(false)` locks a window
against input for modal states. `moved` / `moving` / `sizing` /
`enabledChanged` signals report window state changes.

**Session end.** `System::setSessionEndCallback` runs synchronously on the
message-loop thread before the OS session ends (shutdown / restart / logoff) so
the app can save state; returning non-zero vetoes the shutdown.

### 4. WebView — the core: JS ↔ native bridge

**This is the core of the library.** `WebViewWindow` is a `Window` subclass
that embeds a WebView2 browser; `createWebView()` attaches it (initialization
is asynchronous, navigation requests made meanwhile are queued). On top of it,
**`bindJson<Args...>`** is the star feature: each of the JS call's arguments is
deserialized into the corresponding `Args` type (nlohmann), the handler runs as
a detached `std::execution::task<Resp>` coroutine, and the result is serialized
back to resolve the JS `Promise`:

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <nlohmann/json.hpp>

struct AddReq { int a; int b; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AddReq, a, b)

int main()
{
    auto app    = std::make_shared<helios::App>();
    auto window = std::make_shared<helios::WebViewWindow>(900, 640, "WebView Demo");
    window->show();
    window->createWebView();

    window->bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<int> {
        co_return req.a + req.b;
    });

    window->navigateHtml(
        "<html><body>"
        "<button onclick=\"go()\">add</button>"
        "<script>"
        "async function go() {"
        "  const r = await window.helios.call('add', {a: 40, b: 2});"  // -> 42
        "  alert(r);"
        "}</script>"
        "</body></html>");

    return app->exec();
}
```

`bindJson` / `subscribeJson` also accept a **member function** (pass the object
pointer and the member pointer). The bridge shim exposes
`window.helios.call(name, ...)` → `Promise` and a **bidirectional
`BroadcastChannel`** (`broadcast` native→JS, `subscribe` JS→native). Every
bridge name must be a C identifier `[A-Za-z_][A-Za-z0-9_]*`.

**Events, local resources, and native dialogs**:

- **Navigation events** — four signals on `WebViewWindow`, all fired on the UI
  thread: `navigationStarting` (with the `navigationStartingGate` veto
  `std::function`), `urlChanged`, `titleChanged`, `navigationCompleted`.
- **`mapLocalFolder(host, folder)`** — serves a local folder over a virtual
  `https://<host>/` host for assets outside the packaged frontend.
- **`helios::selectFolder`** and friends — native dialogs exposed to the page
  through a `bindJson` handler.

The raw C-style bridge (`bind` / `resolve` / `reject` / `eval` / `evalAsync` /
`broadcast` / `subscribe`) is also available; `resolve`/`reject`/`broadcast`
are thread-safe.

> **Lifetime:** destroy the `WebViewWindow` only when no `bindJson` task or
> `evalAsync` call is still in flight. The WebView must be destroyed before its
> parent window.

### 5. Threading contract in practice

All UI APIs run on the `App::exec` thread. Background work lives in your own
threads / a thread pool / any async library — and returns to the UI thread
through `App::postTask`:

```cpp
helios::App app;
helios::Window window(800, 600, "Demo");
window.show();

std::thread worker([app] {
    // ... slow work on this thread ...
    app->postTask([app] {
        // back on the UI thread: safe to touch windows/webviews here
        std::println("done");
    });
});
worker.detach();

return app.exec();
```

### 6. Dialogs & system helpers

All native dialogs are modal and must be called on the message-loop thread.
Picked paths are returned as UTF-8 `std::string`:

```cpp
// message box
helios::MessageBoxResult r = helios::messageBox(
    window.nativeHandle(), helios::MessageBoxType::Question,
    helios::MessageBoxButtons::YesNo, "Question", "Continue?");

// folder picker
std::string folder;
if (helios::selectFolder(window.nativeHandle(), "Pick a folder", folder))
    std::println("folder: {}", folder);

// file pickers (single or multi; "Name|*.ext|..." filter format)
auto files = helios::openFiles(window.nativeHandle(), "Pick images",
                               "Images (*.png;*.jpg)|*.png;*.jpg|All files (*.*)|*.*",
                               /*multi=*/true);

// save dialog
std::string path;
if (helios::saveFile(window.nativeHandle(), "Save as", "Text (*.txt)|*.txt", "out.txt", path))
    std::println("saving to {}", path);

// clipboard
helios::clipboardSetText("hello");
std::string clip;
if (helios::clipboardGetText(clip)) { /* ... */ }

// open in browser / reveal in Explorer
helios::openUrl("https://example.com");
helios::showInFolder("C:\\path\\to\\file.txt");
```

### 7. Notifications (toasts)

Modern OS toasts. **Thread-agnostic**: call from any thread. Init once at
startup (registers an AppUserModelID + Start Menu shortcut):

```cpp
helios::notificationInit("MyApp");                    // once, at startup
helios::notificationShow("Download", "Finished");     // any thread
```

### 8. Tray icon + popup / context menu

`helios::Tray` shows an icon in the notification area; `helios::Menu` is a
popup / context menu. Both attach to a **created (shown)** native window and
respond through signals:

```cpp
helios::Menu menu(window.nativeHandle());
helios::Menu::Item* show = menu.addItem("Show / Restore");
helios::Menu::Item* quit = menu.addItem("Quit");
menu.addSeparator();
show->triggered.connect([&] { window.showNormal(); });
quit->triggered.connect([&] { app.quit(); });

helios::Tray tray(window.nativeHandle(), "Tray Demo");
tray.leftClicked.connect([] { /* ... */ });
tray.rightClicked.connect([&] { menu.show(window.nativeHandle()); });
tray.notify("Tray", "Hello");                          // balloon (no setup needed)
```

### 9. The C API

Every C++ feature is a thin wrapper over `include/HeliosView/heliosview.h` — a
pure C header (`extern "C"`, POD types, no C++ objects or exceptions across the
ABI). It is complete enough to build an app **without any C++** (see the C99
`HeliosViewCDemo` example). All strings are UTF-8.

```c
#include <heliosview.h>
#include <stdio.h>

static int frame(void* userdata)
{
    (void)userdata;
    heliosview_event_t ev;
    while (heliosview_poll(&ev)) {
        if (ev.type == HELIOSVIEW_EVENT_KEY_DOWN && ev.key == HELIOSVIEW_KEY_ESCAPE)
            heliosview_window_close(heliosview_window_from_id(ev.window_id));
    }
    return 0;
}

int main(void)
{
    heliosview_window_t* win = heliosview_window_create(800, 600, "C demo");
    heliosview_window_show(win);

    heliosview_tray_t* tray = heliosview_tray_create(win, "C tray", NULL, NULL);
    heliosview_tray_notify(tray, "Tray", "hello", HELIOSVIEW_TRAY_NOTIFY_INFO, 3000);

    heliosview_message_box(win, HELIOSVIEW_MESSAGE_INFO, HELIOSVIEW_MESSAGE_OK,
                           "Info", "Hello from C");

    heliosview_run(frame, NULL);   /* message loop; NULL callback = idle loop */

    heliosview_tray_destroy(tray);
    heliosview_window_destroy(win);
    return 0;
}
```

Library-returned strings (dialog paths, clipboard text) are freed with
`heliosview_free`. The full C surface mirrors the C++ features: window +
events, tray/menu, WebView bridge, dialogs, system helpers, notifications —
see `heliosview.h` for the documented contracts (threading, lifetime, error
codes).

---

## Repository layout

```
include/HeliosView/heliosview.h       C API (the only external ABI)
include/HeliosViewCore/               header-only C++ wrapper
  HeliosView.h                        umbrella include
  Signal.h                            signals/slots (sync + async slots)
  Types.h                             event types (1:1 with the C API)
  App.h                               message loop + UI-thread scheduler
  Window.h                            top-level window + state/drag/DPI/taskbar/backdrop APIs
  Dialogs.h                           native dialogs + message box
  System.h                            clipboard / open-URL / show-in-folder
  Notification.h                      OS toast notifications (thread-safe)
  Tray.h                              system notification-area (tray) icon + signals
  Menu.h                              popup / context menu + signals
  Execution.h                         schedulers/senders (stdexec, P2300)
  WebViewWindow.h                     window embedding a WebView
  WebViewJson.h                       bindJson / subscribeJson (nlohmann auto-binding)
src/heliosview.cpp                    platform-independent core
src/heliosview_internal.h             state shared across implementation files
src/win32/                            win32 backend (windows, WebView2, dialogs, toasts)
third_party/stdexec/                  vendored stdexec (pinned commit, header-only)
examples/                             the demo programs
```

## Roadmap

- More platforms behind the C ABI (Linux/macOS backends).
- More WebView events (history (back/forward) or page-load-initiated dialogs /
  printing / context-menu events).
- Multi-select taskbar overlay, color/font pickers.
