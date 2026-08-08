# HeliosView

A C++ viewer library. Two layers:

- **`HeliosView.dll`** — a pure **C API** (stable ABI). Cross-platform primitives
  for windows, events, a WebView, and async I/O. Platform-specific code lives in
  per-platform backends (currently `src/win32/`: Win32 window/message loop,
  WebView2, IOCP thread pool).
- **`HeliosView.Core`** — a **header-only C++ wrapper** built on the C API:
  signals/slots, C++23 coroutines (`std::execution` senders/receivers), and a
  WebView bridge with **nlohmann auto-binding** (`bindJson`).

Include one header and link one CMake target:

```cpp
#include <HeliosViewCore/HeliosView.h>
```

```cmake
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

Everything here is Windows (win32) at the moment; the C API is the porting
boundary — other platforms re-implement `src/<platform>/` behind it.

---

## Building

Requires CMake ≥ 4.3 and a C++23 compiler. Dependencies are vendored
automatically via `FetchContent` (no vcpkg/conan):

| dependency    | version           | used for                                  |
| ------------- | ----------------- | ----------------------------------------- |
| `stdexec`     | pinned main commit | P2300 senders/receivers (C++23 stand-in) |
| `nlohmann/json` | v3.11.3         | WebView bridge auto-binding (`bindJson`)  |
| WebView2 SDK  | NuGet 1.0.3800.47 | embedded WebView (win32)                  |

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

DLLs and demos land in `build/bin/` together, so no `PATH` setup is needed.

`HELIOSVIEW_BUILD_EXAMPLES=ON` (default) builds five demos:

| demo                     | file                   | shows                                             |
| ------------------------ | ---------------------- | ------------------------------------------------- |
| `HeliosViewDemo`         | `examples/main.cpp`    | signals + async slots (background pool ↔ UI)      |
| `HeliosViewWindowDemo`   | `examples/window_demo.cpp` | basic window + signal/slot                   |
| `HeliosViewAppDemo`      | `examples/app_demo.cpp`   | `Window` subclassing, window styles, member slots |
| `HeliosViewCoroDemo`     | `examples/coro_demo.cpp`  | coroutines: file I/O + TCP on the thread pool  |
| `HeliosViewWebViewDemo`  | `examples/webview_demo.cpp` | **WebView + `bindJson` auto-binding**       |

---

## Tutorial

### 1. A window and a message loop

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

    return app.exec();               // message loop
}
```

`Window` also offers `showMinimized/Maximized/Normal`, `move/resize`,
`position/size/geometry`, `setTitle`, `center`, `setOpacity`, `focus`,
`showState`, `requestClose`, and `WindowStyle::{Normal, Borderless, Frameless}`
via the constructor.

### 2. Signals and slots (sync and async)

```cpp
helios::App app;
helios::Async async;                       // background thread pool (IOCP on win32)

helios::Window window(800, 600, "async");

// sync slot: runs on the UI thread
window.keyPressed.connect([](helios::KeyCode key) { /* ... */ });

// async slot: the slot returns a std::execution::task coroutine,
// started fire-and-forget on emission (on a separate thread)
window.mouseButtonPressed.connect([&](int32_t x, int32_t y, helios::MouseButton b)
                                  -> std::execution::task<void> {
    co_await std::execution::schedule(async.get_scheduler());   // hop to the pool
    // ... slow work on a worker thread ...
    app.postTask([&] { /* back on the UI thread */ });
});

window.show();
return app.exec();
```

> The async slot's task may outlive the window/signal that emitted it: **own any
> captured objects** (`shared_ptr`, `std::make_shared<App>`/`Async`, ...), never
> capture only references to stack objects. Handle errors inside the coroutine.

### 3. Coroutines + async I/O (`Async`)

The thread pool is a `std::execution::scheduler`; the `*Async` sender APIs throw
`helios::IoError` at the `co_await` point on failure.

```cpp
std::execution::task<void> pipeline(helios::Async& async)
{
    // schedule: run on the pool after the await point
    co_await std::execution::schedule(async.get_scheduler());

    helios::File f = co_await async.fileOpenAsync("data.bin", /*write=*/true);
    co_await async.fileWriteAsync(f, "hi", 2, 0);

    helios::TcpSocket sock = co_await async.tcpConnectAsync("example.com", 80);
    co_await async.tcpWriteAsync(sock, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", 36);
    char buf[4096];
    uint32_t n = co_await async.tcpReadAsync(sock, buf, sizeof buf);
}

int main()
{
    helios::Async async;
    try { std::execution::sync_wait(pipeline(async)); }
    catch (const helios::IoError& e) { std::println("io error {}", e.code()); }
}
```

Callback style and sender style are both available (`fileOpen`/`fileRead`/...
vs. `fileOpenAsync`/`fileReadAsync`/...). Handles are copyable/refcounted and
close automatically when the last copy dies.

### 4. WebView + JS ↔ native bridge with nlohmann auto-binding

This is the star feature: **`bindJson<Req>`**. The JS call's first argument is
deserialized into a `Req` DTO (nlohmann), the handler runs as a detached
`std::execution::task<Resp>`, and the result is serialized back to resolve the
JS `Promise`.

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <nlohmann/json.hpp>

struct AddReq { int a; int b; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AddReq, a, b)     // std::string / DTO / nlohmann::json ...

struct GreetReq { std::string name; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GreetReq, name)

int main()
{
    auto app    = std::make_shared<helios::App>();
    auto window = std::make_shared<helios::WebViewWindow>(900, 640, "WebView Demo");
    window->show();
    window->createWebView();

    window->bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<int> {
        co_return req.a + req.b;
    });

    window->bindJson<GreetReq>("greet", [](GreetReq req)
                               -> std::execution::task<helios::JsonResp<std::string>> {
        co_return helios::JsonResp<std::string>{"msg", std::format("hello, {}", req.name)};
    });

    window->navigateHtml(
        "<html><body>"
        "<button onclick=\"go()\">add</button>"
        "<script>"
        "async function go() {"
        "  const r = await window.helios.call('add', {a: 40, b: 2});"  // -> 42
        "  const m = await window.helios.call('greet', {name: 'helios'});" // -> {msg:"hello, helios"}
        "}</script>"
        "</body></html>");

    window->eval("console.log('hi from native');");   // run JS
    return app->exec();
}
```

The bridge shim is injected into every page automatically and exposes:

- **`window.helios.call(name, ...args)` → `Promise`** — invokes a native
  function bound with `bind`/`bindJson`; resolves with the return value or
  rejects with the error.
- **`new BroadcastChannel(name)`** — the standard API; native code can push to
  it via `window->broadcast(name, json)` from any thread.

**Response shapes** (the handler task's value type):

| handler returns                      | JS receives                                |
| ------------------------------------ | ------------------------------------------ |
| `Resp` (a DTO / number / string)     | `resolve(Resp)`                            |
| `nlohmann::json`                     | `resolve(j)` directly                      |
| `JsonResp<T>` `{"key", value}`       | `resolve({"key": value})`                  |
| `JsonError<T>` `{"key", value}`      | `reject({"key": value})`                   |
| `void`                               | `resolve(null)`                            |
| thrown exception / `set_error`       | `reject({"error": <what()>})`              |

You can also use the raw C-style bridge: `bind` (JSON string of args),
`resolve`/`reject`, `eval`, `evalAsync`, `broadcast`. `resolve`/`reject`/
`broadcast` are thread-safe (marshal to the UI thread); `bind`/`eval` are
UI-thread calls.

> **Lifetime:** destroy the `WebViewWindow` only when no `bindJson` task or
> `evalAsync` call is still in flight. The WebView must be destroyed before its
> parent window.

### 5. The C API

Every C++ feature is a thin wrapper over `include/HeliosView/heliosview.h` (a
pure C header). The C API is useful directly when you want no C++ dependency:

```c
heliosview_window_t* win = heliosview_window_create(800, 600, "C demo");
heliosview_window_show(win);
heliosview_run(NULL, NULL);   /* message loop */
```

See `src/win32/` for the reference implementation of the ABI.

---

## Repository layout

```
include/HeliosView/heliosview.h       C API (the only external ABI)
include/HeliosViewCore/               header-only C++ wrapper
  HeliosView.h                        umbrella include
  Signal.h                            signals/slots (sync + async slots)
  Types.h                             event types (1:1 with the C API)
  App.h                               message loop + UI-thread scheduler
  Window.h                            top-level window
  Execution.h                         C++26 <execution> compat (stdexec on C++23)
  Async.h                             thread pool + file/TCP async APIs
  WebViewWindow.h                     window embedding a WebView
  WebViewJson.h                       bindJson (nlohmann auto-binding)
src/heliosview.cpp                    platform-independent core
src/heliosview_internal.h             state shared across implementation files
src/win32/                            win32 backend (windows, WebView2, IOCP)
examples/                             the five demo programs
```

## Roadmap

- More platforms behind the C ABI (Linux/macOS backends).
- WebView events (`WebViewWindow` signals for load/url/message).
- Install/package rules (`cmake --install`, CMake config files).
