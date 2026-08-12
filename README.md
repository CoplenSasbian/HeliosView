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

## Memory allocation

HeliosView routes **all of its allocations through a single configurable
allocator**, so they can come from a pool, arena, or other allocator instead of
the process heap. This is one mechanism shared by the C API and the C++
wrapper: configure it once and everything the library allocates follows.

**The rule of thumb:** whatever you configure, **allocate and free must use the
same allocator**, and you must configure it **before creating anything**
(changing it while objects are alive is undefined behavior). Object
construction and destruction still run normally (placement-new + destructor);
only the underlying memory comes from the allocator.

- **C** — `heliosview_set_allocator(&heliosview_allocator_t)` replaces the
  library's default `malloc`/`free` with your own:

  ```c
  static void* my_alloc(size_t size, void* ctx) { (void)ctx; return pool_alloc(size); }
  static void  my_free (void* p,    void* ctx) { (void)ctx; pool_free(p); }

  int main(void) {
      const heliosview_allocator_t a = { my_alloc, my_free, NULL };
      heliosview_set_allocator(&a);   /* set once, before creating anything */
      /* ... */
  }
  ```

  Passing `NULL` to `heliosview_set_allocator` restores the default `malloc`/`free`.

- **C++** — the wrapper builds on the same C allocator, and where it must store
  state whose lifetime outlives a single call it uses the **default PMR memory
  resource** (`std::pmr::get_default_resource()`). Point it at a pool with
  `std::pmr::set_default_resource(...)` to control those allocations. Bindings
  that point at a user-owned object are allocation-free.

> **One line:** call `heliosview_set_allocator` (C) and point the PMR default
> resource (C++) at your allocator before creating anything, and every
> HeliosView allocation goes through it.

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
| `HeliosViewWebViewEventsDemo` | `examples/webview_events_demo.cpp` | navigation events, local folder mapping, folder dialog |

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
| `async.connect(host, port, cb)`      | `co_await async.connectAsync(...)` | `IoError`                     |
| `async.write(socket, buf, len, cb)`  | `co_await async.writeAsync(...)`  | `IoError`                     |
| `async.read/Stop`, `readAsync` | `co_await async.readAsync(...)` | `IoError`                     |

The sender APIs throw `helios::IoError` at the `co_await` point on failure
(`IoError::code()` holds the negated platform error code):

```cpp
std::execution::task<void> pipeline(helios::Async& async)
{
    // schedule: run on the pool after the await point
    co_await std::execution::schedule(async.get_scheduler());

    helios::File f = co_await async.fileOpenAsync("data.bin", /*write=*/true);
    co_await async.fileWriteAsync(f, "hi", 2, 0);

    helios::Socket sock = co_await async.connectAsync("example.com", 80);
    co_await async.writeAsync(sock, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", 36);
    char buf[4096];
    uint32_t n = co_await async.readAsync(sock, buf, sizeof buf);
}

int main()
{
    helios::Async async;
    try { std::execution::sync_wait(pipeline(async)); }
    catch (const helios::IoError& e) { std::println("io error {}", e.code()); }
}
```

The callback style is equally complete (`async.connect(...)` / `async.fileRead(...)`
/ ...), with `int error` + result in the callback; on a synchronous submission
error the callback fires inline on the calling thread. Handles
(`Socket`/`File`) are copyable/refcounted and close automatically when the
last copy dies; the `*Async` senders keep the handle alive for the operation.

**Writing with `Buffer`** — `write` / `fileWrite` (callback) and `writeAsync` /
`fileWriteAsync` (sender) take a `helios::Buffer`, whose construction makes data
ownership explicit (no implicit borrow), so you control whether the write copies
or not:

```cpp
std::vector<char> m_out;      // long-lived member

co_await async.writeAsync(sock, helios::Buffer::copy(m_out));        // allocate + copy (safe)
co_await async.writeAsync(sock, helios::Buffer::copy(m_out.data(), m_out.size()));
co_await async.writeAsync(sock, helios::Buffer::ref(m_out));         // borrow, zero-copy; m_out
                                                                     // MUST outlive the call
co_await async.writeAsync(sock, m_out.data(), m_out.size());         // convenience: copies

async.fileWrite(f, helios::Buffer::ref(m_out), 0, cb);               // callback form: zero-copy
async.write(sock, helios::Buffer::copy(payload), cb);                // callback form: copies
```

- `Buffer::copy(...)` / `Buffer::alloc(n)` / `Buffer::take(pmr::vector)` → **owned**:
  moved into the operation, zero-copy, always safe.
- `Buffer::ref(...)` → **borrowed**: points at your data with no copy; the data must
  stay alive until the write completes. Use it for long-lived members, never temporaries.
- `copy`/`ref` accept `(pointer, size)`, `std::span`, and any contiguous byte container
  (`std::vector<char>` / `std::vector<uint8_t>` / `std::string` / `std::array` /
  `std::string_view`, ...).
- Both the callback APIs (`write`/`fileWrite`) and the sender APIs (`writeAsync`/
  `fileWriteAsync`) accept a `Buffer` with the same semantics.

> **Threading:** callbacks and sender completions run on worker threads and may
> fire concurrently — synchronize shared state. To get back to the UI thread,
> use `app.postTask(...)` (see the async-slot example above).
> **Lifetime:** `Async` must outlive all pending operations (destroying it with
> operations in flight is undefined behavior).

### 4. WebView + JS ↔ native bridge with nlohmann auto-binding

This is the star feature: **`bindJson<Args...>`**. Each of the JS call's
arguments is deserialized into the corresponding `Args` type (nlohmann), the
handler runs as a detached `std::execution::task<Resp>`, and the result is
serialized back to resolve the JS `Promise`. You can bind a single DTO
(`bindJson<AddReq>`) or several scalar/container parameters
(`bindJson<int, int>`), matching the JS call's argument array positionally.

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

Multiple parameters are matched to the JS call's argument array positionally:

```cpp
// JS: window.helios.call("add", 40, 2)  -> 42
window->bindJson<int, int>("add", [](int a, int b) -> std::execution::task<int> {
    co_return a + b;
});

// JS: window.helios.call("greet", "helios", 3)  -> {"msg":"hello, helios (x3)"}
window->bindJson<std::string, int>("repeat", [](std::string s, int n)
                                   -> std::execution::task<helios::JsonResp<std::string>> {
    co_return helios::JsonResp<std::string>{"msg", std::format("hello, {} (x{})", s, n)};
});
```

`bindJson` / `subscribeJson` also accept a **member function** — pass the object
pointer (usually `this`) and the member pointer instead of a lambda:

```cpp
struct Service {
    std::execution::task<std::string> repeat(RepeatReq req) { co_return req.s; }
    void onStatus(MsgReq req) { /* ... */ }
};

auto service = std::make_shared<Service>();
window->bindJson<RepeatReq>("repeat", service.get(), &Service::repeat);       // task<Resp> (Obj::*)(Req)
window->subscribeJson<MsgReq>("status", service.get(), &Service::onStatus);   // void (Obj::*)(Req)
```

The member function's signature is the same as the lambda form (a
`std::execution::task<Resp>` for `bindJson`, `void` for `subscribeJson`); the
object is captured by pointer and must outlive the binding. const and
non-const members both work.

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

### 4.5 WebView events, local resources, and native dialogs

Three additions for building real UI on top of the bridge:

- **`navigationCompleted`** — a signal on `WebViewWindow` that fires on the UI
  thread when a page load completes (`error == 0`) or fails (negated platform
  error code). The app can start initializing after the first successful load
  and show an error state on failure:

  ```cpp
  window->navigationCompleted.connect([](int error) {
      if (error == 0)
          window->eval("app.bootstrap();");   // page ready for JS
  });
  ```

- **`mapLocalFolder(host, folder)`** — serves a local folder over a virtual
  `https://<host>/` host so the page can load files that are not part of the
  frontend (game banners, avatars, downloaded assets...). WebView2 restricts
  the host name to the `.local` suffix; call before navigating (reload for
  changes):

  ```cpp
  window->mapLocalFolder("assets.local", "C:/cache/game_images");
  // page: <img src="https://assets.local/header_123.jpg">
  ```

- **`helios::selectFolder(parent, title, out_path)`** — a native folder-picker
  dialog (modal, optional parent from `Window::nativeHandle()`), typically
  exposed to the page through a `bindJson` handler:

  ```cpp
  window->bindJson<BrowseReq>("browseFolder", [win = window.get()](BrowseReq req)
                              -> std::execution::task<helios::JsonResp<std::string>> {
      std::string path;
      const bool ok = helios::selectFolder(win->nativeHandle(), req.title.c_str(), path);
      co_return helios::JsonResp<std::string>{ok ? "path" : "cancelled", ok ? path : ""};
  });
  ```

The underlying C API for all three is `heliosview_webview_set_navigation_callback`,
`heliosview_webview_map_local_folder`, and `heliosview_select_folder`.

### 5. Tray icon + popup / context menu

`helios::Tray` shows an icon in the OS notification area; `helios::Menu` is a
popup / context menu. Both are attached to a **created (shown)** native window
via `window.nativeHandle()` and respond through signals. They are decoupled from
the `Window` wrapper — they only need the raw handle.

```cpp
helios::Window window(800, 600, L"Tray Demo");
window.show();                              // window must exist first

helios::Menu menu(window.nativeHandle());
helios::Menu::Item* show  = menu.addItem(L"Show / Restore");
helios::Menu::Item* quit  = menu.addItem(L"Quit");
menu.addSeparator();
show->triggered.connect([&] { window.showNormal(); });
quit->triggered.connect([&] { app.quit(); });

helios::Tray tray(window.nativeHandle(), L"Tray Demo");   // tooltip
tray.leftClicked.connect([] { /* ... */ });
tray.rightClicked.connect([&] { menu.show(window.nativeHandle()); }); // context menu at cursor
tray.leftDoubleClicked.connect([&] { app.quit(); });

window.mouseButtonPressed.connect([&](int x, int y, helios::MouseButton b) {
    menu.show(window.nativeHandle());       // also on a right-click in the window
});
```

- `Tray` signals: `leftClicked`, `leftDoubleClicked`, `rightClicked`,
  `middleClicked`. The icon is loaded from an `.ico`/`.cur` path (UTF-16);
  pass `nullptr` for the default application icon.
- `Menu::addItem` / `addSeparator` / `addSubmenu`, and `menu.show(window)` pops it
  at the current cursor position. Submenus are owned by the parent.
- Tray and menu handle their own events via the App's extension-sink registry,
  so they need no C++ `Window` wrapper. **Destroy the tray/menu before their
  window.**

### 6. The C API

Every C++ feature is a thin wrapper over `include/HeliosView/heliosview.h` — a
pure C header (`extern "C"`, POD types, no C++ objects or exceptions across the
ABI). The C API is the porting boundary, and it is complete enough to build an
app **without any C++**:

- window + events (`heliosview_run` message loop, `heliosview_poll`/`wait`,
  `heliosview_window_*`)
- the WebView bridge (`heliosview_webview_*`): JS ⇄ native calls, eval,
  bidirectional BroadcastChannel, all over JSON strings
- async I/O (`heliosview_loop_*`, `heliosview_socket_*`, `heliosview_file_*`)

This is a C translation of the C++ tutorial above (C99, `printf`-style).
See **Memory allocation** at the top of this page for how the C allocator works.

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

##### Tray icon + menu, and native-message conversion

The C layer exposes the same tray/menu primitives, plus a **window routing
registry** that lets you associate a routing id with your own `userdata` so the
default conversion resolves tray/menu/custom messages back to that object
(`ev.userdata`):

```c
/* tray: show an icon, then route click events via the normal queue */
heliosview_tray_t* tray = heliosview_tray_create(win, L"tooltip", NULL, my_ctx);
/* HELIOSVIEW_EVENT_TRAY_LEFT_CLICK / _LEFT_DOUBLE_CLICK / _RIGHT_CLICK / _MIDDLE_CLICK
   arrive on the queue with ev.window_id = win's id and ev.userdata = my_ctx */
heliosview_tray_destroy(tray);

/* popup menu: add items; choosing one posts HELIOSVIEW_EVENT_MENU_SELECT
   with ev.menu_item = the item id */
heliosview_menu_t* menu = heliosview_menu_create(win, my_ctx);
uint32_t quit_id;
heliosview_menu_add_item(menu, L"Quit", &quit_id);
heliosview_menu_show(menu, win);
```

`heliosview_window_add_item(win, userdata)` / `heliosview_window_remove_item`
allocate/free routing ids on a window; the id becomes a WM_APP message id
(tray) or a WM_COMMAND LOWORD (menu) that `default_native_convert` maps back to
`userdata`. Register your own ids for the same routing.

Custom native-message → event conversion uses an ordered, id-keyed registry
(the library's built-in conversion always runs **first**, then registered
handlers in registration order; the first returning 1/0 wins):

```c
uint32_t id = heliosview_add_native_handler(my_convert);   /* replaces set_native_handler */
heliosview_remove_native_handler(id);
```

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
static void on_connect(int error, heliosview_socket_t* tcp, void* userdata)
{
    if (error) { printf("connect failed: %d\n", error); return; }
    static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    heliosview_socket_write(tcp, req, sizeof req - 1, on_write, NULL);
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
    heliosview_socket_connect(loop, "example.com", 80, on_connect, NULL);

    heliosview_file_open(loop, "data.bin", 1, on_open, NULL);  /* write mode */

    heliosview_loop_run(loop);   /* blocks until loop_stop */
    return 0;
}
```

`heliosview_file_*` offsets are absolute (`int64_t`); the handle must be closed
with `heliosview_file_close`/`heliosview_socket_close`. See
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
  Dialogs.h                           native dialog helpers (folder picker)
  Tray.h                              system notification-area (tray) icon + signals
  Menu.h                              popup / context menu + signals
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
- More WebView events (`WebViewWindow` signals for URL changes / history).
- Install/package rules (`cmake --install`, CMake config files).
