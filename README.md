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
  a WebView bridge with **Boost.JSON auto-binding** (`bindJson`), signals/slots,
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
| WebView right-click | `heliosview_webview_set_context_menu` + `..._set_context_menu_callback` / `setContextMenuEnabled` + `contextMenuGate` (one switch for the engine's menu, plus page/native interception) |
| tray icon + menu | `heliosview_tray_*` / `heliosview_menu_*` / `Tray` / `Menu` |
| dialogs | folder/file pickers, message box (`Dialogs.h`) |
| system helpers | clipboard, open-URL, show-in-folder, run-program, standard folders, OS info (`System.h`) |
| global hotkeys | `heliosview_hotkey_register` / `System::hotkeyRegister` (fire while unfocused) |
| notifications (toasts) | `heliosview_notification_*` / `Notification.h` (any thread) |
| message loop, `std::execution` scheduler | `heliosview_run` / `App` |
| async + HTTP client | `Async` (asio thread pool: timers, sockets) / `http::Client` (keep-alive connection pool, TLS) |

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
| WebView2 SDK | 1.0.4129.50 | downloaded from NuGet at configure time | embedded WebView (win32) |
| `stdexec` | pinned commit 758f41f4 (origin/main, 2026-08-15, 0.11.0+) | vendored (`third_party/stdexec/`) | C++23 coroutines (senders/receivers) |
| `Boost` (Asio/Beast/JSON) | 1.92.0 (superproject submodule, needed libs auto-initialized) | vendored (`third_party/boost/`) | background thread pool (`Async`), HTTP via Boost.Beast, WebView bridge auto-binding via Boost.JSON |

Everything else comes from the OS: windowing, dialogs, toasts (WinRT via the
Windows SDK), DWM backdrop. The WebView2 SDK is the only thing fetched at
configure time (a `.nupkg` is just a zip of headers + the WebView2Loader
library), cached in the build directory. Boost libs are initialized
selectively at configure time: a single `git submodule update --init --depth 1`
inside `third_party/boost/` with every needed lib as a pathspec (git output is
streamed and `--jobs` clones them in parallel, so a fresh clone never looks
stalled) — do not run `git submodule update --init --recursive`, it would fetch
all ~160 Boost libraries.

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

DLLs and demos land in `build/bin/` together, so no `PATH` setup is needed.

`HELIOSVIEW_BUILD_EXAMPLES=ON` (default) builds the demo programs:

| demo | file | shows |
| --- | --- | --- |
| `HeliosViewDemo` | `examples/main.cpp` | **the master demo**: a WebView page drives every feature through `bindJson` + `broadcast` (read this one first) |
| `HeliosViewWebViewDemo` | `examples/webview_demo.cpp` | the JS ↔ native bridge on its own: typed DTOs, member-function handlers, `subscribeJson`, Async pool + HTTP from a handler |
| `HeliosViewWebViewEventsDemo` | `examples/webview_events_demo.cpp` | navigation events + the veto gate, `mapLocalFolder`/`localUrl`, a native dialog called from the page |
| `HeliosViewWindowDemo` | `examples/window_demo.cpp` | windows: styles/flags, state APIs, signals (member slots *and* lambdas), menu bar + popup menu, tray |
| `HeliosViewSystemDemo` | `examples/system_demo.cpp` | OS integration: file dialogs, clipboard, toasts, global hotkey, standard folders, session end |
| `HeliosViewAsyncHttpDemo` | `examples/async_http_demo.cpp` | console (no window): the `Async` pool (schedulers, timers, sockets) + the pooled `http::Client` |
| `HeliosViewCDemo` | `examples/c_demo.c` | **pure C**: the C API end to end (window, loop, tray, menu, dialog) |

---

## Releases — SDK package

GitHub release assets are a self-contained **SDK zip** for Windows x64
(`HeliosView-<version>-win64-SDK.zip`), so consumers don't have to build the
library themselves:

```
bin/        HeliosView.dll + WebView2Loader.dll + prebuilt demos (runnable as-is)
lib/        HeliosView.lib + libboost_json.lib + CMake package config (find_package)
include/    C API header (HeliosView/), C++ wrapper (HeliosViewCore/), vendored stdexec + Boost
examples/   demo sources — build standalone against the SDK
README.md   usage instructions (also shipped inside the zip)
```

Consume from CMake:

```cmake
find_package(HeliosView REQUIRED)      # -DCMAKE_PREFIX_PATH=<sdk root>
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

or from plain C: include `include\`, link `lib\HeliosView.lib`, and copy
`HeliosView.dll` + `WebView2Loader.dll` from `bin\` next to your exe. Full
instructions live in [packaging/README.md](packaging/README.md), which is
shipped as the zip's `README.md`.

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
    window.closeRequested.connect(&Window::close, &window);  // close button (×) triggers this

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

**Text and modifier state.** `keyPressed` fires only for the initial press,
`keyRepeated` for OS auto-repeat, and `keyEvent(const KeyEvent&)` for every key
down/up with `modifiers` (`helios::mods::Ctrl | helios::mods::Shift | …`,
including LEFT_/RIGHT_ bits and lock state) and a `repeat` flag.
`textInput(const std::string&)` delivers UTF-8 text as typed — IME commits
included; input longer than the event buffer arrives as consecutive events, so
nothing is ever truncated. In the C API the same information is in
`heliosview_event_t` (`modifiers`, `flags`, `text`/`text_len`, and
`HELIOSVIEW_EVENT_TEXT_INPUT`).

**Close button behavior.** Clicking the close button (×) or pressing Alt+F4
does **not** destroy the window — it only emits the `closeRequested` signal.
Connect to it and call `close()` to actually close:

```cpp
window.closeRequested.connect(&Window::close, &window);  // click × → closes
```

**Frameless dragging.** A frameless/borderless window has no OS title bar, so
register the custom title-bar strips as drag regions — a mouse-down + drag
inside them moves the window like a native title bar (`WM_NCHITTEST →
HTCAPTION`):

```cpp
helios::Window win(480, 320, "Frameless", helios::WindowStyle::Frameless);
win.addDragRegion(0, 0, 480, 40);          // the title-bar strip
win.show();
```

> **WebView caveat.** Drag regions are implemented through the host window's
> `WM_NCHITTEST`. A full-bleed WebView is a child window covering the entire
> client area, so hit-testing over it is answered by the WebView's own window
> procedure and never reaches the host — registered drag regions are ineffective
> while the WebView covers them. With a `WebViewWindow`, register the drag area
> on the page instead: the injected `<helios-window-title-bar>` component drags
> through WebView2's native `app-region: drag` support (enabled by the library
> via `IsNonClientRegionSupportEnabled`), or bind `startDrag()` to a page
> callback for a fully custom area.

**Web-drawn title bar (recommended).** Because a full-bleed WebView cannot be
covered by native DWM caption buttons ("no visible title bar" and "system
caption buttons" are mutually exclusive on Win32), the natural way is to draw
the title bar in the page. The bridge shim injects two web components on every
page:

- `<helios-window-title-bar>` — place it at the top of the page: it drags the
  window through WebView2's **native `app-region: drag` support** (the library
  enables `IsNonClientRegionSupportEnabled` — no bridge round-trip, nothing to
  bind) and toggles maximize on double-click.
- `<helios-window-controls>` — put it inside the title bar: the Win10/11
  min/max/close glyphs (hover/pressed feedback, close turns red), calling the
  built-in `__hv.control` / `__hv.state` bridge; the maximize button shows the
  restore glyph while maximized (and is disabled while the window is not
  resizable). It opts its buttons out of the drag region
  (`app-region: no-drag`) automatically.

```cpp
auto win = std::make_shared<helios::WebViewWindow>(
    900, 640, "App", helios::WindowStyle::Frameless);
win->show();
win->createWebView();
// that's it — no bindJson for drag or buttons: dragging is native app-region,
// the buttons use the built-in __hv.control / __hv.state bridge (__hv.state
// also reports titleBarHeight, the DPI-scaled title-bar strip height).
```

```html
<helios-window-title-bar>
  <span>App</span>
  <helios-window-controls></helios-window-controls>
</helios-window-title-bar>
```

The components are restyled via CSS on the elements (the title bar defaults to
a 48 px flex row; the buttons float top-right). Other interactive children
inside the title bar need `app-region: no-drag` to stay clickable.

**Resize.** The `Frameless` style keeps a small non-client border on all four
sides (`WM_NCCALCSIZE`), so the system draws a grab-able frame and resizing
works natively — even with a full-bleed WebView, because the border is
non-client and the system hit-tests it directly. Nothing to add to the page.

> Native DWM buttons would require keeping the system caption (a "normal
> window"), which a full-bleed WebView cannot cover — and web-drawn buttons have
> no Win11 snap-layouts popup (that needs a real caption).

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
**`bindJson`** is the star feature: each of the JS call's arguments is
deserialized into the corresponding parameter type (Boost.JSON), the handler runs
as a detached `std::execution::task<Resp>` coroutine, and the result is serialized
back to resolve the JS `Promise`. The parameter types are **deduced from the
handler**, so the explicit `bindJson<Args...>` list is optional:

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <boost/describe.hpp>   // BOOST_DESCRIBE_STRUCT (DTO annotations)

struct AddReq { int a; int b; };
BOOST_DESCRIBE_STRUCT(AddReq, (), (a, b))

int main()
{
    auto app    = std::make_shared<helios::App>();
    auto window = std::make_shared<helios::WebViewWindow>(900, 640, "WebView Demo");
    window->show();
    window->createWebView();

    window->bindJson("add", [](AddReq req) -> std::execution::task<int> {
        co_return req.a + req.b;
    });

    window->bindJson<AddReq>("add2", [](AddReq req) -> std::execution::task<int> {  // explicit, same thing
        co_return req.a + req.b;
    });

    window->bindJson("sum", [](int a, int b) -> std::execution::task<int> {  // several arguments
        co_return a + b;
    });

    window->bindJson("ping", []() -> std::execution::task<bool> {  // no arguments
        co_return true;
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

**How the deduction works.** `bindJson` reads the parameter types off the
handler's own signature (`&Fn::operator()` for a lambda/functor, the function
type for a free function, the member pointer for the member-function overload)
and decays them to values (`const Req` → `Req`), so the JS argument is
deserialized with `value_to<Req>`. It needs a **single non-template signature**:
a generic lambda (`[](auto req) { ... }`), an overloaded/templated `operator()`,
or a `std::function` has none, so those must spell the types out
(`bindJson<AddReq>(name, handler)`), and a clear `static_assert` says so.
`subscribeJson` deduces its single `Req` the same way from a
`(Req) -> void` callback (the callback must take exactly one parameter).

Handlers take their parameters **by value**: the handler is a *lazy*
`std::execution::task` whose body only starts once the sender is started, so the
argument deserialized from the JS call is already destroyed by then and a
reference parameter would dangle. The deduced form therefore rejects a
by-reference parameter with a `static_assert` (`write [](Req req) instead of
[](const Req& req)`); the explicit `bindJson<Req>` form keeps its historical
behavior. `subscribeJson` callbacks are plain synchronous calls, so they may
take their value by value or by reference.

`bindJson` / `subscribeJson` also accept a **member function** (pass the object
pointer and the member pointer; the parameter types come from the member's
signature when omitted). The bridge shim exposes
`window.helios.call(name, ...)` → `Promise` and a **bidirectional
`BroadcastChannel`** (`broadcast` native→JS, `subscribe` JS→native). Every
bridge name must be a C identifier `[A-Za-z_][A-Za-z0-9_]*`; the library's
internal bridge uses `__hv.`-prefixed names (`__hv.control`, `__hv.state`,
`__hv.drag` — called by the injected components), which contain a dot and are
therefore **not valid identifiers**, so applications cannot bind or subscribe
them and can never shadow the built-in components.

**Events, local resources, and native dialogs**:

- **Navigation events** — four signals on `WebViewWindow`, all fired on the UI
  thread: `navigationStarting` (with the `navigationStartingGate` veto
  `std::function`), `urlChanged`, `titleChanged`, `navigationCompleted`.
- **`mapLocalFolder(host, folder)`** + **`localUrl(host, path)`** — serves a local
  folder for assets outside the packaged frontend; build the URL with `localUrl()`
  (Windows: a virtual `https://<host>/` host; other engines register a custom
  scheme, so never hard-code the URL).
- **`helios::selectFolder`** and friends — native dialogs exposed to the page
  through a `bindJson` handler.

**WebView2 window-chrome settings** — small toggles that shape the WebView's
native chrome, each available on `WebViewWindow` (C++: `setStatusBarEnabled`,
`setContextMenuEnabled`, `setDevToolsEnabled`; C: `heliosview_webview_set_status_bar`
/ `heliosview_webview_set_context_menu` / `heliosview_webview_set_devtools`):

- **Status bar** — the hovered-link URL hint at the bottom-left; **disabled by
  default**, re-enable with `setStatusBarEnabled(true)`.
- **Right-click** — one switch decides whether the engine's own menu (copy/paste,
  save image, inspect) may open, and the application can intercept a click from two
  independent sides — the page, or native code:

  ```cpp
  win.setContextMenuEnabled(false);   // never let the engine's menu open (true = default)

  // Native interception: consulted for every right click; true keeps the engine's
  // menu closed for that click, false falls through to the switch. What to show is
  // the app's business - here a helios::Menu popped at the cursor.
  win.contextMenuGate = [&win, &menu](const helios::ContextMenuInfo& info) {
      if (info.has(helios::ContextMenuTarget::Editable))
          return false;               // e.g. let the engine offer paste/spell check
      // The gate runs inside the engine's event dispatch and a popup runs a modal
      // loop: intercept now, show ours on the next UI turn.
      helios::App::instance()->postTask([&] { menu.show(win.nativeHandle()); });
      return true;
  };
  ```

  The page is the other door: its DOM `contextmenu` event fires either way, so
  `preventDefault()` plus an HTML menu is the fully portable answer (and the only
  one where the engine has no native hook). `ContextMenuInfo` carries the target
  flags (`Link`, `Image`, `Media`, `Selection`, `Editable`, `Page`), the link
  URL/text, the selected text, the page URL and the position; which fields are
  filled is up to the engine (empty = it cannot report it), so treat the struct as
  best-effort. `setContextMenuEnabled(false)` returns a negative error
  (`HELIOSVIEW_ERROR_UNSUPPORTED`) where the engine cannot suppress its menu — e.g.
  a WebView2 runtime older than 100 — so the app can fall back to the page route.
- **DevTools** — F12 / right-click Inspect; enabled by default, disable with
  `setDevToolsEnabled(false)` (disabling closes an already-open DevTools
  window).

Each applies immediately once the WebView is initialized; calls made during
initialization take effect when it becomes ready.

The raw C-style bridge (`bind` / `resolve` / `reject` / `eval` / `evalAsync` /
`broadcast` / `subscribe`) is also available; `resolve`/`reject`/`broadcast`
are thread-safe.

> **Lifetime:** destroy the `WebViewWindow` only when no `bindJson` task or
> `evalAsync` call is still in flight. The WebView must be destroyed before its
> parent window.

### 5. Async + HTTP client (connection pooling)

`helios::Async` is one asio thread pool plugged into `std::execution`: timers,
sockets and the HTTP client all run on the same workers. `helios::http::Client`
is a lightweight handle (copyable, may be a temporary) that keeps **keep-alive
connections per origin** and reuses them:

```cpp
helios::Async async;                       // app-scoped member
helios::http::Client client{async};        // one handle, shared pool

window->bindJson<Req>("api", [&client](Req r) -> std::execution::task<boost::json::value> {
    auto resp = co_await client.get(r.url);          // pooled connection when possible
    co_return boost::json::value{{"status", resp.status}, {"body", resp.body}};
});
```

Pool tuning (all optional; defaults reuse connections for 60 s and keep up to 4
idle per origin, 16 overall):

```cpp
helios::http::PoolOptions opt;
opt.idle_timeout = 30s;        // drop idle connections older than this
opt.max_idle_per_origin = 2;
opt.max_idle_total = 8;
opt.dns_cache_ttl = 60s;       // resolved endpoints are cached per origin
opt.keep_alive = false;        // opt out: "Connection: close", nothing pooled
helios::http::Client client{async, 10s, "cacert.pem", opt};

client.idle_connections();     // diagnostics
client.close_idle();           // drop idle connections now
client.clear_dns_cache();
```

A pooled connection can be closed by the server while idle; an idempotent
request (GET/HEAD/OPTIONS/PUT/DELETE/TRACE) is then retried once on a fresh
connection, while POST/PATCH reports the error instead of risking a duplicate
side effect. Every step (resolve, connect, TLS handshake, exchange) is bounded
by the client timeout. `https://` verifies certificates against `cacert.pem`
(or the OpenSSL default paths) and reuses one SSL context per client.

### 6. Threading contract in practice

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

### 7. Dialogs & system helpers

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

// file pickers (single or multi; structured filters: name + extensions)
auto files = helios::openFiles(window.nativeHandle(), "Pick images",
                               std::vector<helios::FileFilter>{
                                   {"Images", "png;jpg;jpeg"},
                                   {"All files", "*.*"}
                               },
                               /*multi=*/true);

// save dialog
std::string path;
if (helios::saveFile(window.nativeHandle(), "Save as",
                     std::vector<helios::FileFilter>{{"Text", "txt"}}, "out.txt", path))
    std::println("saving to {}", path);

// clipboard
helios::clipboardSetText("hello");
std::string clip;
if (helios::clipboardGetText(clip)) { /* ... */ }

// open in browser / reveal in Explorer
helios::openUrl("https://example.com");
helios::showInFolder("C:\\path\\to\\file.txt");
```

### 8. Notifications (toasts)

Modern OS toasts. **Thread-agnostic**: call from any thread (the callbacks run on
an unspecified thread — marshal back to the loop thread before touching UI).
Init once at startup (registers the application id: Windows AppUserModelID +
Start Menu shortcut, macOS bundle id, Linux desktop id):

```cpp
helios::App::setAppId("com.example.myapp");           // or pass it to init
helios::notificationInit();                           // once, at startup
helios::notificationRequestPermission([](helios::NotificationPermission p) {
    // macOS: the system prompt was answered; Windows/Linux: the OS setting
    std::println("permission = {}", static_cast<int>(p));
});
helios::notificationSetClickCallback([](const char* title, const char* body) {
    /* the user clicked the toast */
});
helios::notificationShow("Download", "Finished");     // any thread
```

macOS silently drops toasts until the user allows them, so ask for permission at
startup. On Windows the first run of a brand-new app id reports
`NotificationPermission::Unknown` (the OS registers it asynchronously); that is
not a denial.

### 9. Tray icon + popup / context menu

`helios::Tray` shows an icon in the notification area; `helios::Menu` is a
popup / context menu. A menu is **standalone** (no window needed to build it;
`show(window)` takes an optional owner) and displays **actions** — the action
owns the command (label, enabled/checked state, `triggered`), the menu owns the
layout. The same action can be shown by several menus:

```cpp
helios::Action copy("Copy");                 // shared command
copy.triggered.connect(&onCopy, this);

helios::Menu menu;                           // no window needed
helios::Menu::Item* show = menu.addItem("Show / Restore");   // menu-owned action
helios::Menu::Item* quit = menu.addItem("Quit");
helios::Menu::Item* top = menu.addCheckItem("Toggle Topmost"); // checkable
helios::Menu::Item* off = menu.addItem("Unavailable");
off->setEnabled(false);                              // grayed out, not selectable
menu.addAction(copy);                                // shared action
menu.addSeparator();
menu.setDefaultAction(*show);                        // bold default item
show->triggered.connect([&] { window.showNormal(); });
quit->triggered.connect([&] { app.quit(); });
top->triggered.connect([&] {                    // toggle the checkmark
    top->setChecked(!top->checked());
    /* ... apply the state ... */
});
editMenu->addAction(copy);                           // same command, another menu
copy.setEnabled(false);                              // both menus gray out

// Standard roles: the platform supplies the label and shortcut, and the library
// performs the action where the app cannot (edit / window commands).
editMenu->addRole(helios::MenuRole::Cut);            // ⌘X on macOS, Ctrl+X on Windows
editMenu->addRole(helios::MenuRole::Copy);
editMenu->addRole(helios::MenuRole::Paste);
windowMenu->addRole(helios::MenuRole::Minimize);
appMenu->addRole(helios::MenuRole::Quit);            // "Quit <App>" ⌘Q / "Exit"

helios::Action open("Open…", "Primary+O");           // portable shortcut string
open.triggered.connect(&onOpen, this);
fileMenu->addAction(open);

// Application menu bar: macOS installs the one global bar (first menu = App
// menu); Windows shows it as the bar of every window, including future ones.
// helios::MenuBar represents the horizontal menu bar (Win32 CreateMenu,
// NSApp.mainMenu on macOS, GtkMenuBar on Linux) holding vertical Menu instances.
helios::MenuBar bar;
bar.addMenu("File")->addAction(open);
bar.addMenu("Edit")->addRole(helios::MenuRole::Copy);
bar.setAppMenu();

// A shortcut on an action is an application-wide accelerator: heliosview_run /
// heliosview_pump_events translate it; an app running its own loop calls
// heliosview_translate_accelerator(&msg) before DispatchMessage.

helios::Tray tray("Tray Demo");
tray.setMenu(menu);                                    // portable context menu
tray.leftClicked.connect([] { /* ... */ });
tray.notify("Tray", "Hello");                          // balloon (no setup needed)

// Attach the menu instead of popping it up from rightClicked: on Linux the
// shell owns the menu (it is exported over DBus, the app cannot show it) and on
// macOS an NSStatusItem menu opens on any click — so with a menu attached the
// click events may not be delivered at all. Detach with tray.setMenu(nullptr).
```

Window chrome is portable too: `WindowStyle` picks the baseline, `WindowFlag`
refines it, and the same code gives each OS its idiomatic look:

```cpp
// macOS: real title bar, hidden title, content underneath, traffic lights over
// the page. Windows: no native caption, the app draws its own chrome.
helios::Window w(900, 600, "App", helios::WindowStyle::Normal,
                 helios::WindowFlag::TitleBarHidden
                     | helios::WindowFlag::TitleBarTransparent
                     | helios::WindowFlag::FullSizeContent);
w.scaleFactor();      // 1.0 / 2.0 — logical units -> device pixels
w.flags();            // the flags it was created with
```

### 10. The C API

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
        if (ev.type == HELIOSVIEW_EVENT_WINDOW_CLOSE)
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
  WebViewJson.h                       bindJson / subscribeJson (Boost.JSON auto-binding)
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
