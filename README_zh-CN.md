# HeliosView

> **[English](README.md) | 简体中文**

一个 C++ **WebView** 库：嵌入一个 webview，从 C++ 或 C 驱动它，并围绕它构建应用的其余部分（窗口、托盘、异步 I/O、HTTP）。分两层：

- **`HeliosView.dll`** — 纯 **C API**（ABI 稳定）。提供跨平台的窗口、事件、WebView、异步 I/O（线程池 + TCP/文件 + 一次性定时器）、以及异步 HTTP 客户端等原语。平台相关代码放在各平台的 backend（当前为 `src/win32/`：Win32 窗口/消息循环、WebView2、IOCP 线程池）。
- **`HeliosView.Core`** — 构建在 C API 之上的 **纯头文件 C++ 封装**：带 **nlohmann 自动绑定**（`bindJson`）的 WebView 桥接、信号/槽、C++23 协程（`std::execution` sender/receiver），以及异步 HTTP 客户端。

只需包含一个头文件、链接一个 CMake target：

```cpp
#include <HeliosViewCore/HeliosView.h>
```

```cmake
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

当前全部基于 Windows (win32)；C API 是移植边界 —— 其他平台在它后面重新实现 `src/<platform>/`。

---

## 内存分配

HeliosView 把**所有分配都路由到一个可配置的分配器**，因此内存可以来自 pool、arena 或其他分配器，而不是进程堆。这是 C API 和 C++ 封装共享的同一机制：配置一次，库分配的所有内存都走它。

**经验法则：** 无论你配置什么，**分配和释放必须使用同一个分配器**，并且必须在**创建任何对象之前**完成配置（在对象存活期间更换分配器是未定义行为）。对象的构造与析构仍照常执行（placement-new + 析构函数）；只有底层内存来自分配器。

- **C** — `heliosview_set_allocator(&heliosview_allocator_t)` 把库默认的 `malloc`/`free` 换成你自己的：

  ```c
  static void* my_alloc(size_t size, void* ctx) { (void)ctx; return pool_alloc(size); }
  static void  my_free (void* p,    void* ctx) { (void)ctx; pool_free(p); }

  int main(void) {
      const heliosview_allocator_t a = { my_alloc, my_free, NULL };
      heliosview_set_allocator(&a);   /* 在创建任何对象之前设置一次 */
      /* ... */
  }
  ```

  向 `heliosview_set_allocator` 传 `NULL` 恢复默认的 `malloc`/`free`。

- **C++** — 封装建立在同一个 C 分配器之上；凡是需要存储生命周期超出单次调用的状态，都使用**默认 PMR 内存资源**（`std::pmr::get_default_resource()`）。用 `std::pmr::set_default_resource(...)` 指向某个 pool 即可控制这些分配。指向用户自有对象的绑定不产生分配。

> **一句话：** 在创建任何对象之前，C 侧调用 `heliosview_set_allocator`、C++ 侧把 PMR 默认资源指向你的分配器，HeliosView 的所有分配就都会走它。

---

## 构建

需要 CMake ≥ 4.3 和 C++23 编译器。`https://` 的 TLS 使用 Windows SChannel；其余依赖要么内置（vendored）要么在配置时自动获取：

| 依赖             | 版本                           | 来源                                       | 用途                               |
| ---------------- | ----------------------------- | ------------------------------------------ | ---------------------------------- |
| TLS (SChannel)   | 系统自带                       | Windows                                    | 异步 HTTP 客户端的 TLS（`https://`） |
| `nlohmann/json`  | 3.12                          | vendored（`third_party/json/`）             | WebView 桥接的自动绑定（`bindJson`） |
| WebView2 SDK     | 1.0.4129.50                   | 配置时从 NuGet 下载                         | 内嵌 WebView（win32）              |
| `stdexec`        | 固定 commit b783aac（2024-03） | vendored（`third_party/stdexec/`）          | C++23 协程（sender/receiver）       |
| http-parser      | 2.9.4                         | vendored（`third_party/http-parser/`）      | HTTP/1.1 响应解析                  |

`stdexec`、http-parser、nlohmann/json 都按上表版本内置，因此不依赖任何包管理器。WebView2 SDK 是唯一在配置时获取的东西（`.nupkg` 其实就是个包含头文件和 WebView2Loader 库的 zip），并且缓存在构建目录中 —— 离线环境下，可从之前一次 configure 预置 `build/webview2-sdk/`。

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

DLL 和 demo 一起落在 `build/bin/`，无需配置 `PATH`。

`HELIOSVIEW_BUILD_EXAMPLES=ON`（默认）会构建演示程序：

| demo                        | 文件                        | 演示内容                                        |
| --------------------------- | --------------------------- | ----------------------------------------------- |
| `HeliosViewDemo`            | `examples/main.cpp`         | 信号 + 异步槽（后台线程池 ↔ UI）                 |
| `HeliosViewWindowDemo`      | `examples/window_demo.cpp`  | 基础窗口 + 信号/槽                               |
| `HeliosViewAppDemo`         | `examples/app_demo.cpp`     | `Window` 子类化、窗口样式、成员函数槽            |
| `HeliosViewCoroDemo`        | `examples/coro_demo.cpp`    | 协程：线程池上的文件 I/O + TCP                   |
| `HeliosViewHttpDemo`        | `examples/http_demo.cpp`    | 异步 HTTP 客户端：GET/POST/PUT/DELETE、JSON/XML、TLS、回调 + sender API |
| `HeliosViewWebViewDemo`     | `examples/webview_demo.cpp` | **WebView + `bindJson` 自动绑定**               |
| `HeliosViewWebViewEventsDemo` | `examples/webview_events_demo.cpp` | 导航事件、本地文件夹映射、文件夹对话框    |

---

## 教程

教程按依赖顺序展开：**App**（消息循环）→ **Signals** → **Window** → **WebView**（库的核心），然后是其余特性 —— 异步线程池、HTTP 客户端、托盘/菜单 —— 最后是支撑这一切的 C API。

### 1. App + 消息循环

`helios::App` 是进程唯一的应用对象：它拥有消息循环和 UI 线程任务队列。其他一切（窗口、webview…）都经由它分发。

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main()
{
    helios::App app;                       // 每个进程恰好一个 App

    // postTask(fn)：可从任意线程调用；fn 在消息循环空闲时于 UI 线程上执行
    // —— 这是后台工作回到 UI 线程的途径。
    app.postTask([] { std::println("hello from the UI thread"); });

    // UI 线程同时也是一个 std::execution::scheduler：
    //   std::execution::schedule(app.get_scheduler()) | std::execution::then(fn);

    return app.exec();                     // 消息循环；quit() 或
                                           // 最后一个窗口关闭时返回
}
```

`exec()` 运行消息循环（正常退出返回 0）；`quit()` 请求退出，可从任意线程调用。底层队列访问：`pollEvent` / `waitEvent` / `postEvent`；可重写 `event()` 处理应用级的未处理事件。

### 2. 信号与槽

信号/槽是所有 UI 对象（Window、WebView、Tray…）对外暴露的事件机制。`helios::Signal<Args...>` 持有槽并在发射时调用它们；`connect` 返回槽 id，供 `disconnect(id)` 使用：

```cpp
helios::Signal<int32_t, int32_t> resized;          // 参数为 (w, h) 的信号

// 同步槽：立即执行，运行在发射线程（UI 线程）上
resized.connect([](int32_t w, int32_t h) { std::println("resized {}x{}", w, h); });

resized(800, 600);                                 // 发射
```

槽有三种形式 —— 同步可调用对象、成员函数（`connect(&MyWindow::onKeyPressed, this)`，同步或异步），以及**异步槽**：返回 sender（协程 `task` 或某个 Async 操作）的可调用对象，在发射时 fire-and-forget 启动：

```cpp
helios::App app;
helios::Async async;                       // 后台线程池（§5，win32 上为 IOCP）

helios::Signal<std::string> onRequest;

// 同步槽：运行在 UI 线程
onRequest.connect([](std::string s) { /* ... */ });

// 异步槽：槽返回一个 std::execution::task 协程，
// 在发射时 fire-and-forget 启动
onRequest.connect([&](std::string s) -> std::execution::task<void> {
    co_await std::execution::schedule(async.get_scheduler());   // 跳到线程池
    // ... 在工作线程上做耗时操作 ...
    app.postTask([&] { /* 回到 UI 线程 */ });
});
```

> 异步槽的 task 可能比发射它的信号活得久：**自己持有捕获的对象**（`shared_ptr`、`std::make_shared<App>`/`Async`…），不要只捕获栈对象的引用。在协程内部处理错误。

Window、WebView、Tray… 把事件暴露为现成的信号（`window.keyPressed`、`window.resized`、`window->navigationCompleted`…），下文各处都会用到。

### 3. 窗口

`helios::Window` 是由 App 消息循环驱动的顶层窗口。信号上报输入（信号/槽：§2）：

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main()
{
    helios::App app;
    helios::Window window(800, 600, L"Hello");
    window.show();

    window.resized.connect([](int32_t w, int32_t h) { std::println("resized {}x{}", w, h); });
    window.keyPressed.connect([&](helios::KeyCode key) {
        std::println("key {}", static_cast<int>(key));
        if (key == helios::KeyCode::Escape)
            window.close();          // 最后一个窗口关闭 -> 循环退出
    });

    return app.exec();
}
```

`Window` 还提供 `showMinimized/Maximized/Normal`、`move/resize`、`position/size/geometry`、`setTitle`、`center`、`setOpacity`、`focus`、`showState`、`requestClose`，以及可通过构造函数指定的 `WindowStyle::{Normal, Borderless, Frameless}`。

### 4. WebView —— 核心：JS ↔ 原生桥接

**这是库的核心。** `WebViewWindow` 是嵌入了 WebView2 浏览器的 `Window` 子类；`createWebView()` 负责挂载（初始化是异步的，期间的导航请求会被排队）。在它之上，**`bindJson<Args...>`** 是主打特性：JS 调用的每个参数都被反序列化为对应的 `Args` 类型（nlohmann），处理器以分离的 `std::execution::task<Resp>` 运行（协程机制见 §5），结果再序列化回去 resolve 对应的 JS `Promise`。下面的例子绑定了一个 DTO 和几个标量参数。

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
    auto window = std::make_shared<helios::WebViewWindow>(900, 640, L"WebView Demo");
    window->show();
    window->createWebView();

    window->bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<int> {
        co_return req.a + req.b;
    });

    window->bindJson<GreetReq>("greet", [](GreetReq req)
                               -> std::execution::task<helios::JsonResp<std::string>> {
        co_return helios::JsonResp<std::string>{"msg", std::format("hello, {}", req.name)};
    });

    // 多参数：按位置对应到 JS 调用的参数数组
    window->bindJson<std::string, int>("repeat", [](std::string s, int n)
                                       -> std::execution::task<helios::JsonResp<std::string>> {
        co_return helios::JsonResp<std::string>{"msg", std::format("hello, {} (x{})", s, n)};
    });

    window->navigateHtml(
        "<html><body>"
        "<button onclick=\"go()\">add</button>"
        "<script>"
        "async function go() {"
        "  const r = await window.helios.call('add', {a: 40, b: 2});"  // -> 42
        "  const m = await window.helios.call('greet', {name: 'helios'});" // -> {msg:"hello, helios"}
        "  const x = await window.helios.call('repeat', 'helios', 3);"     // -> {"msg":"hello, helios (x3)"}
        "}</script>"
        "</body></html>");

    window->eval("console.log('hi from native');");   // 运行 JS
    return app->exec();
}
```

`bindJson` / `subscribeJson` 也接受**成员函数** —— 传入对象指针（通常是 `this`）和成员指针，代替 lambda：

```cpp
struct Service {
    std::execution::task<std::string> repeat(RepeatReq req) { co_return req.s; }
    void onStatus(MsgReq req) { /* ... */ }
};

auto service = std::make_shared<Service>();
window->bindJson<RepeatReq>("repeat", service.get(), &Service::repeat);       // task<Resp> (Obj::*)(Req)
window->subscribeJson<MsgReq>("status", service.get(), &Service::onStatus);   // void (Obj::*)(Req)
```

成员函数的签名与 lambda 形式相同（`bindJson` 为 `std::execution::task<Resp>`，`subscribeJson` 为 `void`）；对象以指针方式捕获，必须比绑定活得更久。const 与非 const 成员均可。

桥接 shim 自动注入每个页面，并暴露：

- **`window.helios.call(name, ...args)` → `Promise`** — 调用用 `bind`/`bindJson` 绑定的原生函数；用返回值 resolve，或用错误 reject。
- **`new BroadcastChannel(name)`** — 标准 API，**双向**：
  - 原生 → JS：`window->broadcast(name, json)` 向页面上的 `BroadcastChannel(name)` 实例投递 `message` 事件（任意线程）。
  - JS → 原生：页面的 `channel.postMessage(value)` 被转发到该频道上的原生订阅 —— `window->subscribeJson<Req>(name, cb)`（或原始 C 风格 `subscribe`）—— 并且按规范要求，*同样*投递给其他同源的标签页/实例。

两个方向共存：shim 子类化 `BroadcastChannel`，因此页面的 `postMessage` 会同时到达原生**和**标准频道。

**响应形态**（处理函数 task 的值类型）：

| 处理器返回                    | JS 收到的结果                           |
| ----------------------------- | --------------------------------------- |
| `Resp`（DTO / 数字 / 字符串） | `resolve(Resp)`                         |
| `nlohmann::json`              | 直接 `resolve(j)`                       |
| `JsonResp<T>` `{"key", value}` | `resolve({"key": value})`              |
| `JsonError<T>` `{"key", value}` | `reject({"key": value})`              |
| `void`                        | `resolve(null)`                         |
| 抛出异常 / `set_error`        | `reject({"error": <what()>})`           |

也可以使用原始 C 风格桥接：`bind`（参数为 JSON 字符串）、`resolve`/`reject`、`eval`、`evalAsync`、`broadcast`、`subscribe`/`unsubscribe`。`resolve`/`reject`/`broadcast` 线程安全（会 marshal 到 UI 线程）；`bind`/`subscribe`/`eval` 是 UI 线程调用（其他线程会自动 marshal）。

所有桥接名字（`bind`/`bindJson`/`subscribe`/`subscribeJson`/`broadcast`）必须是 **C 标识符** `[A-Za-z_][A-Za-z0-9_]*` —— 这与原生函数的命名一致，也保证内部信封（envelope）帧格式可解析。非法名字在注册时被拒绝（C API 返回 `-2`；其上的 C++ 层也会拒绝）。

**事件、本地资源与原生对话框** —— 在桥接之上构建真实 UI 所需的补充：

- **导航事件** — `WebViewWindow` 上的四个信号，都在 UI 线程上触发：

  - **`navigationStarting`** — 在导航开始前触发（初始加载、链接、`navigate()`、浏览器前进/后退、重定向），携带 `(uri, isRedirected, isUserInitiated)`。要**取消**导航，设置 `navigationStartingGate` 这个 std::function，返回 `true` 表示否决（`Signal` 不能返回值）：

    ```cpp
    window->navigationStarting.connect([](std::string uri, bool isRedirected, bool isUserInitiated) {
        // 只监听，不取消
    });
    // 或者把成员函数连成槽：
    window->connectStarting(nav_info, this);

    window->navigationStartingGate = [](const std::string& uri, bool isRedirected, bool) {
        return uri.starts_with("https://external.example/");  // 阻止外部链接
    };
    ```

  - **`urlChanged`** — 当 WebView 当前 URL 变化时触发 `(uri, isNewDocument)`。`isNewDocument` 为 true 表示来自新文档加载，false 表示文档内变化（`pushState`/fragment）。
  - **`titleChanged`** — 当页面 `<title>` 变化时触发（`title`）。
  - **`navigationCompleted`** — 页面加载完成（`error == 0`）或失败（取反的平台错误码）时触发。用它来判断页面何时可以 `eval()`，或展示错误状态：

    ```cpp
    window->navigationCompleted.connect([](int error) {
        if (error == 0)
            window->eval("app.bootstrap();");   // 页面已就绪，可执行 JS
    });
    ```

- **`mapLocalFolder(host, folder)`** — 通过一个虚拟的 `https://<host>/` 主机服务本地文件夹，让页面可以加载不属于前端的文件（游戏横幅、头像、下载的资源…）。WebView2 要求主机名以 `.local` 结尾；在导航前调用（改动后需要刷新）：

  ```cpp
  window->mapLocalFolder("assets.local", "C:/cache/game_images");
  // 页面：<img src="https://assets.local/header_123.jpg">
  ```

- **`helios::selectFolder(parent, title, out_path)`** — 原生文件夹选择对话框（模态，可传 `Window::nativeHandle()` 作为父窗口），通常通过一个 `bindJson` 处理器暴露给页面：

  ```cpp
  window->bindJson<BrowseReq>("browseFolder", [win = window.get()](BrowseReq req)
                              -> std::execution::task<helios::JsonResp<std::string>> {
      std::string path;
      const bool ok = helios::selectFolder(win->nativeHandle(), req.title.c_str(), path);
      co_return helios::JsonResp<std::string>{ok ? "path" : "cancelled", ok ? path : ""};
  });
  ```

这些功能的底层 C API 是 `heliosview_webview_set_navigation_callback`、`heliosview_webview_set_navigation_starting_callback`、`heliosview_webview_set_source_changed_callback`、`heliosview_webview_set_title_changed_callback`、`heliosview_webview_map_local_folder` 和 `heliosview_select_folder`。

> **生命周期：** 只在没有 `bindJson` task 或 `evalAsync` 调用仍在执行时销毁 `WebViewWindow`。WebView 必须在它的父窗口之前销毁。

### 5. 协程 + 异步 I/O（`Async`）

`helios::Async` 是**后台线程池 + 平台多路复用器**（win32 上为 IOCP）—— 是 C 层 `heliosview_loop` 的 C++ 封装。`Async` 与 C API 驱动同一个线程池：

```cpp
helios::Async async;              // 0 = 与硬件并发数相同的工作线程
helios::Async async(4);           // 或指定数量
async.run();                      // 阻塞调用线程直到 stop()
async.stop();                     // 工人线程处理完已投递任务后退出
```

**线程池是一个 `std::execution::scheduler`：**

```cpp
// 在某个工作线程上运行 fn
std::execution::schedule(async.get_scheduler()) | std::execution::then(fn);

// 向线程池投递一个裸任务（不走 sender 机制）
async.post([] { /* worker thread */ });
```

**一次性定时器** — `postAfter(delay, fn)` 在延时后于工作线程上运行一次 `fn`。一个线程池的所有定时器由单个内部定时器线程跟踪（一个 deadline 最小堆），因此待触发的定时器**不占用工作线程**，创建定时器也从不阻塞调用者：

```cpp
async.postAfter(3000, [] { std::println("3s later, on a worker thread"); });
```

线程池能做的所有事都有两种可互换的风格：

| 回调风格                                 | sender 风格（co_await）              | 错误投递                 |
| ---------------------------------------- | ------------------------------------ | ------------------------ |
| `async.post(fn)`                         | `schedule(async.get_scheduler())`    | —                        |
| `async.postAfter(ms, fn)`                | —（fire-and-forget）                 | —                        |
| `async.fileOpen(path, write, cb)`        | `co_await async.fileOpenAsync(...)`  | `IoError` at the `co_await` |
| `async.fileRead(file, buf, len, off, cb)` | `co_await async.fileReadAsync(...)`  | `IoError`                |
| `async.fileWrite(file, buf, len, off, cb)` | `co_await async.fileWriteAsync(...)` | `IoError`                |
| `async.connect(host, port, cb)`          | `co_await async.connectAsync(...)`   | `IoError`                |
| `async.write(socket, buf, len, cb)`      | `co_await async.writeAsync(...)`     | `IoError`                |
| `async.read/Stop`、`readAsync`           | `co_await async.readAsync(...)`      | `IoError`                |

sender API 失败时会在 `co_await` 处抛出 `helios::IoError`（`IoError::code()` 持有取反的平台错误码）：

```cpp
std::execution::task<void> pipeline(helios::Async& async)
{
    // schedule：在 await 之后于线程池上运行
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

回调风格同样完备（`async.connect(...)` / `async.fileRead(...)` / …），回调里携带 `int error` + 结果；如果同步提交就出错，回调会在调用线程上同步触发。句柄（`Socket`/`File`）可拷贝/引用计数，最后一个副本销毁时自动关闭；`*Async` sender 会在操作期间保持句柄存活。

**用 `Buffer` 写入** — `write` / `fileWrite`（回调）和 `writeAsync` / `fileWriteAsync`（sender）接受 `helios::Buffer`，它的构造让数据所有权变得明确（没有隐式借用），由你决定写入是拷贝还是不拷贝：

```cpp
std::vector<char> m_out;      // 长期存活成员

co_await async.writeAsync(sock, helios::Buffer::copy(m_out));        // 分配 + 拷贝（安全）
co_await async.writeAsync(sock, helios::Buffer::copy(m_out.data(), m_out.size()));
co_await async.writeAsync(sock, helios::Buffer::ref(m_out));         // 借用，零拷贝；m_out
                                                                     // 必须比调用活得更久
co_await async.writeAsync(sock, m_out.data(), m_out.size());         // 便捷写法：拷贝

async.fileWrite(f, helios::Buffer::ref(m_out), 0, cb);               // 回调形式：零拷贝
async.write(sock, helios::Buffer::copy(payload), cb);                // 回调形式：拷贝
```

- `Buffer::copy(...)` / `Buffer::alloc(n)` / `Buffer::take(pmr::vector)` → **owned**：移动进操作，零拷贝，始终安全。
- `Buffer::ref(...)` → **borrowed**：指向你的数据，不拷贝；数据必须存活到写入完成。只用于长期存活成员，绝不要用临时对象。
- `copy`/`ref` 接受 `(pointer, size)`、`std::span`，以及任何连续字节容器（`std::vector<char>` / `std::vector<uint8_t>` / `std::string` / `std::array` / `std::string_view`…）。
- 回调 API（`write`/`fileWrite`）与 sender API（`writeAsync`/`fileWriteAsync`）接受 `Buffer` 的语义相同。

> **线程：** 回调和 sender 完成事件运行在工作线程上，可能并发触发 —— 需要同步共享状态。要回到 UI 线程，用 `app.postTask(...)`（见上面异步槽的例子）。
> **生命周期：** `Async` 必须比所有挂起操作活得久（在操作进行中销毁它是未定义行为）。

### 6. 异步 HTTP 客户端（`HttpClient`）

`helios::HttpClient` 是跑在 Async 线程池（§5）上的异步 HTTP/1.1 客户端：DNS + TCP 连接 + 读写都走线程池的 IOCP 套接字层，HTTPS 使用 Windows SChannel（证书校验对照 Windows 系统证书库），响应用 http-parser 解析。整个交换是一个回调驱动的状态机 —— 不阻塞任何调用线程，请求超时由线程池的定时器服务跟踪（无轮询、无每请求线程）。回调与 sender（`co_await`）两种风格都提供：

```cpp
helios::Async async;
helios::HttpClient http(async);
http.setTimeout(15000);                  // 每个请求 15s；0 = 不超时

// 便捷 sender：co_await 它们；传输失败抛出 helios::IoError
std::execution::task<void> fetch(helios::HttpClient& http)
{
    helios::HttpResponse resp = co_await http.get("https://api.example.com/todos/1");
    if (resp.ok())                       // 状态码 2xx
        std::println("json: {}", resp.json()["title"].dump());
}

// 完整请求形式：自定义头 + 便捷方法覆盖不到的内容
helios::HttpRequest req("POST", "https://api.example.com/items");
req.setJsonBody({{"name", "helios"}, {"n", 42}});   // body + Content-Type
req.addHeader("X-Trace", "abc");
helios::HttpResponse r = co_await http.requestAsync(std::move(req));

// 回调风格：恰好触发一次，在工作线程上；失败时 status == 0
http.request(helios::HttpRequest("GET", "https://api.example.com/ping"),
             [](helios::HttpResponse resp) { std::println("status {}", resp.status); });
```

**便捷 sender** — `get`、`post`、`put`、`del` 是 `requestAsync(HttpRequest(...))` 的简写（错误语义相同）。`post`/`put` 接受原始字符串 body（带可选 `Content-Type`），**或任何 nlohmann 能序列化的值** —— `nlohmann::json`、带 `NLOHMANN_DEFINE_TYPE` / ADL `to_json` 的 DTO、map、vector、数字… —— 会以 JSON body 发送（`Content-Type: application/json`）。每个动词都可以带一个可选的尾部参数指定请求头：

```cpp
struct AddItemReq { std::string name; int n; };   // + NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AddItemReq, name, n)

co_await http.get("https://api.example.com/todos/1");
co_await http.get("https://api.example.com/todos/1", {{"Accept", "application/json"}});
co_await http.post("https://api.example.com/items", "<xml>...</xml>", "application/xml");
co_await http.post("https://api.example.com/items", AddItemReq{"helios", 42});        // -> JSON body
co_await http.post("https://api.example.com/items", AddItemReq{"helios", 42},
                   {{"Authorization", "Bearer ..."}});                                // JSON + headers
co_await http.put("https://api.example.com/items/1", "raw", "text/plain",
                  {{"X-Trace", "abc"}});                                              // raw + CT + headers
co_await http.del("https://api.example.com/items/1");
```

`HttpRequest` 携带 `method` / `url` / `headers` / `body`；`HttpResponse` 携带 `status`（0 = 传输失败）、`headers`、`body`，并有 `ok()` / `header(name)` / `json()` 辅助方法。完整走查见 `examples/http_demo.cpp`（GET/POST/PUT/DELETE、JSON/XML body、查询字符串、404、两种 API）。

**超时与取消** — 每请求超时覆盖连接 + 整个交换过程；超时时回调得到 `error == HELIOSVIEW_HTTP_TIMEOUT`（sender API 抛 `IoError`）。`heliosview_http_request_cancel`（C 层）会让请求以 `HELIOSVIEW_HTTP_CANCELLED` 立即完成。

**当前限制** — 每个请求一条连接（`Connection: close`；没有 keep-alive 或连接池，因此每次交换都要完整付出 DNS + TCP（+ TLS）建立成本）、不跟随重定向、不支持 IPv6 字面量或 userinfo URL、响应体完整缓冲且无大小上限。响应头会去重（后者胜出），这会合并多个 `Set-Cookie` 这类合法重复头。sender API 不传播取消 —— 等待中的 task 不得在请求完成前被销毁。

### 7. 托盘图标 + 弹出 / 右键菜单

`helios::Tray` 在系统通知区显示一个图标；`helios::Menu` 是弹出 / 右键菜单。两者都通过 `window.nativeHandle()` 挂到**已创建（显示）**的原生窗口上，并通过信号响应。它们与 `Window` 封装解耦 —— 只需要原始句柄。

```cpp
helios::Window window(800, 600, L"Tray Demo");
window.show();                              // 窗口必须先存在

helios::Menu menu(window.nativeHandle());
helios::Menu::Item* show  = menu.addItem(L"Show / Restore");
helios::Menu::Item* quit  = menu.addItem(L"Quit");
menu.addSeparator();
show->triggered.connect([&] { window.showNormal(); });
quit->triggered.connect([&] { app.quit(); });

helios::Tray tray(window.nativeHandle(), L"Tray Demo");   // tooltip
tray.leftClicked.connect([] { /* ... */ });
tray.rightClicked.connect([&] { menu.show(window.nativeHandle()); }); // 在光标处弹右键菜单
tray.leftDoubleClicked.connect([&] { app.quit(); });

window.mouseButtonPressed.connect([&](int x, int y, helios::MouseButton b) {
    menu.show(window.nativeHandle());       // 在窗口内右键时也弹出
});
```

- `Tray` 信号：`leftClicked`、`leftDoubleClicked`、`rightClicked`、`middleClicked`。图标从 `.ico`/`.cur` 路径加载（UTF-16）；传 `nullptr` 使用默认应用图标。
- `Menu::addItem` / `addSeparator` / `addSubmenu`，`menu.show(window)` 在光标当前位置弹出。子菜单归父级所有。
- 托盘和菜单通过 App 的 extension-sink 注册表自行处理事件，因此不需要 C++ `Window` 封装。**在窗口之前销毁托盘/菜单。**

### 8. C API

每个 C++ 特性都是对 `include/HeliosView/heliosview.h` 的薄封装 —— 纯 C 头（`extern "C"`、POD 类型、ABI 上不跨 C++ 对象或异常）。C API 是移植边界，而且足够完整，可以**完全不写 C++** 就构建应用：

- 窗口 + 事件（`heliosview_run` 消息循环、`heliosview_poll`/`wait`、`heliosview_window_*`）
- WebView 桥接（`heliosview_webview_*`）：JS ⇄ 原生调用、eval、双向 BroadcastChannel，全部走 JSON 字符串
- 异步 I/O（`heliosview_loop_*`、`heliosview_socket_*`、`heliosview_file_*`、一次性定时器 `heliosview_timer_*`）
- 异步 HTTP 客户端（`heliosview_http_*`）：基于 http(s) 的 GET/POST/…、超时、取消

这是上面 C++ 教程的 C 翻译（C99、`printf` 风格）。C 分配器的工作方式见本页顶部的**内存分配**。

#### 窗口 + 消息循环

```c
#include <heliosview.h>

int main(void)
{
    heliosview_window_t* win = heliosview_window_create(800, 600, L"C demo");
    heliosview_window_show(win);

    heliosview_window_t* win2 = heliosview_window_create(320, 200, L"second");
    heliosview_window_show(win2);
    heliosview_window_set_position(win2, 40, 40);      /* move */
    heliosview_window_set_opacity(win2, 0.8f);

    heliosview_run(NULL, NULL);   /* message loop; NULL callback = idle loop */
    return 0;
}
```

事件通过循环回调或队列投递：

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

`heliosview_window_from_id(ev.window_id)` 把事件里的窗口 id 映射回不透明句柄。最后一个窗口销毁后循环继续运行 —— 用 `heliosview_quit()` 结束它。

##### 托盘图标 + 菜单，以及原生消息转换

C 层暴露同样的托盘/菜单原语，外加一个**窗口路由注册表**：可以把一个路由 id 关联到自己的 `userdata`，让默认转换把托盘/菜单/自定义消息解析回那个对象（`ev.userdata`）：

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

`heliosview_window_add_item(win, userdata)` / `heliosview_window_remove_item` 在窗口上分配/释放路由 id；该 id 变成 `default_native_convert` 会映射回 `userdata` 的 WM_APP 消息 id（托盘）或 WM_COMMAND LOWORD（菜单）。也可以注册自己的 id 做同样的路由。

自定义原生消息 → 事件转换使用按 id 排序的注册表（库内建的转换总是**先**运行，然后是按注册顺序的已注册处理器；最先返回 1/0 的胜出）：

```c
uint32_t id = heliosview_add_native_handler(my_convert);   /* 注册自定义转换器 */
heliosview_remove_native_handler(id);
```

#### WebView + JS ⇄ 原生桥接

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
    heliosview_window_t* win = heliosview_window_create(900, 640, L"webview");
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

`subscribe` 回调收到的 `data_json` 就是 `channel.postMessage(...)` 投递的原始 JSON 值。在 C 侧，载荷是 **JSON 字符串** —— nlohmann 糖（`bindJson`/`subscribeJson`）是 C++ 的便利封装，并非必需。

#### 异步 I/O（线程池 + TCP + 文件）

`heliosview_loop` **就是**线程池：`heliosview_loop_create` 派生工作线程（win32 上为 IOCP），C++ 的 `helios::Async` 包装同一个句柄 —— C API 与 C++ API 驱动同一个线程池。所有回调都运行在工作线程上，可能并发触发；错误码为 `0` = 成功，负数 = 取反的平台错误码。

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

`heliosview_file_*` 的偏移是绝对偏移（`int64_t`）；句柄必须用 `heliosview_file_close`/`heliosview_socket_close` 关闭。参考实现见 `src/win32/heliosview_io_win32.cpp`。

##### 一次性定时器

`heliosview_timer_create` 在 `delay_ms` 之后于工作线程上调度一次回调。一个 loop 的所有定时器共享单个内部定时器线程（deadline 最小堆），因此挂起的定时器不占用工作线程。在它仍挂起时销毁句柄即可取消（之后回调不会触发）：

```c
static void on_timeout(int error, void* userdata)
{
    (void)error;
    printf("3s elapsed\n");
}

heliosview_timer_t* t = heliosview_timer_create(loop, 3000, on_timeout, NULL);
/* ... */
heliosview_timer_destroy(t);   /* 0 = cancelled before firing; 1 = already fired */
```

##### 异步 HTTP 客户端

```c
static void on_response(heliosview_http_request_t* req, int error,
                        const heliosview_http_response_t* resp, void* userdata)
{
    if (error)
        printf("transport error: %d\n", error);   /* e.g. HELIOSVIEW_HTTP_TIMEOUT */
    else
        printf("status=%d, %zu bytes\n", resp->status_code, resp->body_len);
    heliosview_http_request_destroy(req);         /* the handle is caller-owned */
}

heliosview_http_client_t* http = heliosview_http_client_create(loop);
heliosview_http_client_set_timeout(http, 15000);  /* 0 = no timeout */

heliosview_http_request_t* req = heliosview_http_client_request(
    http, "GET", "https://example.com/", NULL, NULL, 0, on_response, NULL);
if (!req) printf("bad URL\n");

heliosview_loop_run(loop);   /* the callback fires on a worker thread */
```

客户端支持纯 `http://` 与 `https://`（Windows SChannel，对照 Windows 系统证书库校验）；请求头用 `heliosview_http_headers_*` 构建（请求头在提交时拷贝；响应头去重、后者胜出）。回调恰好触发一次，来自 loop 的工作线程，所有响应指针只在回调期间有效。`heliosview_http_request_cancel` 会让请求以 `HELIOSVIEW_HTTP_CANCELLED` 立即完成。

> **线程速记：** `resolve`/`reject`/`broadcast` 可从任意线程调用（会 marshal 到 UI 线程）。`bind`/`subscribe`/`eval`/`eval_async`/`navigate*` 是 UI 线程调用（其他线程会自动 marshal）。`loop`/`tcp`/`file` 回调总是运行在工作线程上。

---

## 仓库结构

```
include/HeliosView/heliosview.h       C API（唯一的外部 ABI）
include/HeliosViewCore/               纯头文件 C++ 封装
  HeliosView.h                        汇总头文件
  Signal.h                            信号/槽（同步 + 异步槽）
  Types.h                             事件类型（与 C API 一一对应）
  App.h                               消息循环 + UI 线程 scheduler
  Window.h                            顶层窗口
  Dialogs.h                           原生对话框辅助（文件夹选择）
  Tray.h                              系统通知区（托盘）图标 + 信号
  Menu.h                              弹出 / 右键菜单 + 信号
  Execution.h                         scheduler/sender（stdexec，P2300）
  Async.h                             线程池 + 文件/TCP 异步 API + 一次性定时器
  Http.h                              异步 HTTP 客户端（HttpClient / HttpRequest / HttpResponse）
  WebViewWindow.h                     内嵌 WebView 的窗口
  WebViewJson.h                       bindJson / subscribeJson（nlohmann 自动绑定）
src/heliosview.cpp                    平台无关核心
src/heliosview_internal.h             实现文件间共享的状态
src/win32/                            win32 后端（窗口、WebView2、IOCP、异步 HTTP）
third_party/http-parser/              内置 HTTP/1.1 解析器（单个 C 文件）
third_party/stdexec/                  内置 stdexec（固定 commit，纯头文件）
examples/                             演示程序
```

## 路线图

- 在 C ABI 之后支持更多平台（Linux/macOS 后端）。
- HTTP 连接池 / keep-alive、响应大小上限、跟随重定向。
- 更多 WebView 事件（历史（前进/后退）、页面加载发起的对话框 / 打印 / 右键菜单事件）。
- 安装 / 打包规则（`cmake --install`、CMake config 文件）。
