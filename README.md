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

`helios::Async` is a **background thread pool + platform multiplexer** (IOCP on
win32) — a thin C++ wrapper over the C layer's `heliosview_loop`. There is only
**one** thread pool, owned by the C layer: `heliosview_loop_create` spawns the
worker threads. `Async` just holds that same loop, and stdexec only supplies the
sender/receiver plumbing (`schedule`/`co_await`) — it never creates threads.
So the C API and the C++ API are two faces of the same pool:

```cpp
helios::Async async;              // 0 = hardware-concurrency worker threads
helios::Async async(4);           // or a fixed count
async.run();                      // blocks the calling thread until stop()
async.stop();                     // workers exit after finishing posted tasks
```

**The pool is a `std::execution::scheduler`:**

```cpp
// runs fn on a worker thread
std::execution::schedule(async.get_scheduler()) | std::execution::then(fn);

// post a raw task to the pool (no sender plumbing)
async.post([] { /* worker thread */ });
```

Everything the pool can do comes in two interchangeable styles:

| callback style                          | sender style (co_await)              | error delivery                |
| --------------------------------------- | ------------------------------------ | ----------------------------- |
| `async.post(fn)`                        | `schedule(async.get_scheduler())`    | —                             |
| `async.fileOpen(path, write, cb)`       | `co_await async.fileOpenAsync(...)`  | `IoError` at the `co_await`   |
| `async.fileRead(file, buf, len, off, cb)` | `co_await async.fileReadAsync(...)` | `IoError`                     |
| `async.fileWrite(file, buf, len, off, cb)`| `co_await async.fileWriteAsync(...)`| `IoError`                     |
| `async.tcpConnect(host, port, cb)`      | `co_await async.tcpConnectAsync(...)` | `IoError`                     |
| `async.tcpWrite(socket, buf, len, cb)`  | `co_await async.tcpWriteAsync(...)`  | `IoError`                     |
| `async.tcpReadStart/Stop`, `tcpReadAsync` | `co_await async.tcpReadAsync(...)` | `IoError`                     |

The sender APIs throw `helios::IoError` at the `co_await` point on failure
(`IoError::code()` holds the negated platform error code):

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

The callback style is equally complete (`async.tcpConnect(...)` / `async.fileRead(...)`
/ ...), with `int error` + result in the callback; on a synchronous submission
error the callback fires inline on the calling thread. Handles
(`TcpSocket`/`File`) are copyable/refcounted and close automatically when the
last copy dies; the `*Async` senders keep the handle alive for the operation.

> **Threading:** callbacks and sender completions run on worker threads and may
> fire concurrently — synchronize shared state. To get back to the UI thread,
> use `app.postTask(...)` (see the async-slot example above).
> **Lifetime:** `Async` must outlive all pending operations (destroying it with
> operations in flight is undefined behavior).

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
- **`new BroadcastChannel(name)`** — the standard API, **bidirectional**:
  - native → JS: `window->broadcast(name, json)` delivers a `message` event
    to the page's `BroadcastChannel(name)` instances (any thread).
  - JS → native: the page's `channel.postMessage(value)` is forwarded to a
    native subscription on that channel — `window->subscribeJson<Req>(name, cb)`
    (or the raw C-style `subscribe`) — and is *also* delivered to other
    same-origin tabs/instances, as the spec requires.

Both directions coexist: the shim subclasses `BroadcastChannel` so a page
`postMessage` reaches native **and** the standard channel simultaneously.

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
`resolve`/`reject`, `eval`, `evalAsync`, `broadcast`, `subscribe`/`unsubscribe`.
`resolve`/`reject`/`broadcast` are thread-safe (marshal to the UI thread);
`bind`/`subscribe`/`eval` are UI-thread calls (other threads are marshalled).

> **Lifetime:** destroy the `WebViewWindow` only when no `bindJson` task or
> `evalAsync` call is still in flight. The WebView must be destroyed before its
> parent window.

### 5. The C API

Every C++ feature is a thin wrapper over `include/HeliosView/heliosview.h` — a
pure C header (`extern "C"`, POD types, no C++ objects or exceptions across the
ABI). The C API is the porting boundary, and it is complete enough to build an
app **without any C++**:

- window + events (`heliosview_run` message loop, `heliosview_poll`/`wait`,
  `heliosview_window_*`)
- the WebView bridge (`heliosview_webview_*`): JS ⇄ native calls, eval,
  bidirectional BroadcastChannel, all over JSON strings
- async I/O (`heliosview_loop_*`, `heliosview_tcp_*`, `heliosview_file_*`)

This is a C translation of the C++ tutorial above (C99, `printf`-style).

#### Window + message loop

```c
#include <heliosview.h>

int main(void)
{
    heliosview_window_t* win = heliosview_window_create(800, 600, "C demo");
    heliosview_window_show(win);

    heliosview_window_t* win2 = heliosview_window_create(320, 200, "second");
    heliosview_window_show(win2);
    heliosview_window_set_position(win2, 40, 40);      /* move */
    heliosview_window_set_opacity(win2, 0.8f);

    heliosview_run(NULL, NULL);   /* message loop; NULL callback = idle loop */
    return 0;
}
```

Events are delivered via the loop callback or the queue:

```c
/* per-frame: poll the queue; window_id identifies the window (from heliosview_window_id) */
static int frame(void* userdata)
{
    heliosview_event_t ev;
    while (heliosview_poll(&ev)) {
        switch (ev.type) {
        case HELIOSVIEW_EVENT_KEY_DOWN:
            if (ev.key == HELIOSVIEW_KEY_ESCAPE)
                heliosview_window_destroy(heliosview_window_from_id(ev.window_id));
            break;
        case HELIOSVIEW_EVENT_WINDOW_RESIZE:
            printf("resize %d x %d\n", ev.width, ev.height);
            break;
        default: break;
        }
    }
    return 0;                       /* non-zero exits the loop */
}

heliosview_run(frame, NULL);
```

`heliosview_window_from_id(ev.window_id)` maps an event's window id back to the
opaque handle. When the last window is destroyed the loop keeps running — call
`heliosview_quit()` to end it.

#### WebView + JS ⇄ native bridge

```c
/* bind: native function callable from JS via window.helios.call("add", 40, 2) -> Promise */
struct add_ctx { int a; };

static void on_add(heliosview_webview_t* wv, uint64_t call_id, const char* name,
                   const char* args_json, void* userdata)
{
    (void)name;
    struct add_ctx* ctx = (struct add_ctx*)userdata;
    /* args_json = "[40,2]" — parse it with your favorite JSON parser and reply: */
    char result[64];
    snprintf(result, sizeof result, "%d", ctx->a + 2);   /* "42" */
    heliosview_webview_resolve(wv, call_id, result);      /* resolve the Promise */
}

static void add_ctx_free(void* p) { free(p); }

static void on_status(heliosview_webview_t* wv, const char* name,
                      const char* data_json, void* userdata)
{
    printf("JS broadcast on %s: %s\n", name, data_json);
}

static void eval_cb(int error, const char* result_json, void* userdata)
{
    printf("eval -> error=%d result=%s\n", error, result_json);
}

int main(void)
{
    heliosview_window_t* win = heliosview_window_create(900, 640, "webview");
    heliosview_window_show(win);

    heliosview_webview_t* wv = heliosview_webview_create(win);
    struct add_ctx* ctx = malloc(sizeof *ctx);
    ctx->a = 40;
    heliosview_webview_bind(wv, "add", on_add, ctx, add_ctx_free); /* dtor runs on destroy/replace */

    heliosview_webview_navigate_html(wv,
        "<button onclick='window.helios.call(\"add\", 40, 2).then(alert)'>add</button>");

    /* run JS / get a value back */
    heliosview_webview_eval(wv, "console.log('hi');");
    heliosview_webview_eval_async(wv, "1 + 1", eval_cb, NULL);  /* callback(result_json) */

    /* broadcast (native -> JS) and subscribe (JS -> native) on one channel */
    heliosview_webview_broadcast(wv, "status", "{\"n\":1}");
    heliosview_webview_subscribe(wv, "status", on_status, NULL, NULL);

    heliosview_run(NULL, NULL);
    heliosview_webview_destroy(wv);   /* before the parent window */
    heliosview_window_destroy(win);
    return 0;
}
```

The `data_json` handed to a `subscribe` callback is the raw JSON value posted by
`channel.postMessage(...)`. On the C side the payloads are **JSON strings** — the
nlohmann sugar (`bindJson`/`subscribeJson`) is a C++ convenience, not required.

#### Async I/O (thread pool + TCP + file)

`heliosview_loop` **is** the thread pool: `heliosview_loop_create` spawns the
worker threads (IOCP on win32). The C++ `helios::Async` wraps exactly this
handle, and stdexec only adds sender/receiver sugar on top — the C API and the
C++ API drive the same pool. All callbacks run on the worker threads and may
fire concurrently; error codes are `0` = success, negative = negated platform
error.

```c
static void on_connect(int error, heliosview_tcp_t* tcp, void* userdata)
{
    if (error) { printf("connect failed: %d\n", error); return; }
    static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    heliosview_tcp_write(tcp, req, sizeof req - 1, on_write, NULL);
}

static void on_write(int error, uint32_t bytes, void* userdata)
{
    if (!error) printf("sent %u bytes\n", bytes);
}

static void on_open(int error, heliosview_file_t* file, void* userdata)
{
    if (error) { printf("open failed: %d\n", error); return; }
    const char msg[] = "hello\n";
    heliosview_file_write(file, msg, sizeof msg - 1, 0, on_write, NULL);
}

int main(void)
{
    heliosview_loop_t* loop = heliosview_loop_create(0);  /* 0 = hardware concurrency */
    heliosview_tcp_connect(loop, "example.com", 80, on_connect, NULL);

    heliosview_file_open(loop, "data.bin", 1, on_open, NULL);  /* write mode */

    heliosview_loop_run(loop);   /* blocks until loop_stop */
    return 0;
}
```

`heliosview_file_*` offsets are absolute (`int64_t`); the handle must be closed
with `heliosview_file_close`/`heliosview_tcp_close`. See
`src/win32/heliosview_io_win32.cpp` for the reference implementation.

> **Threading recap:** `resolve`/`reject`/`broadcast` may be called from any
> thread (they marshal to the UI thread). `bind`/`subscribe`/`eval`/
> `eval_async`/`navigate*` are UI-thread calls (other threads are marshalled
> automatically). The `loop`/`tcp`/`file` callbacks always run on worker threads.

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
  WebViewJson.h                       bindJson / subscribeJson (nlohmann auto-binding)
src/heliosview.cpp                    platform-independent core
src/heliosview_internal.h             state shared across implementation files
src/win32/                            win32 backend (windows, WebView2, IOCP)
examples/                             the five demo programs
```

## Roadmap

- More platforms behind the C ABI (Linux/macOS backends).
- WebView events (`WebViewWindow` signals for load/url/message).
- Install/package rules (`cmake --install`, CMake config files).
