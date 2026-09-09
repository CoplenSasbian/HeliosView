# HeliosView

> **[English](README.md) | 简体中文**

一个 C++ **WebView** 窗口库：嵌入一个 webview，从 C++ 或 C 驱动它，并围绕它构建桌面壳 —— 窗口、托盘图标、菜单、对话框、通知、任务栏进度、系统集成。分两层：

- **`HeliosView.dll`** — 纯 **C API**（ABI 稳定）。窗口与事件、WebView 桥接、托盘/菜单、原生对话框与系统辅助（剪贴板、打开 URL、toast 通知、任务栏进度、DWM 背景材质）。平台相关代码放在各平台的 backend（当前为 `src/win32/`：Win32 窗口/消息循环、WebView2、IFileDialog、WinRT toast）。
- **`HeliosView.Core`** — 构建在 C API 之上的**纯头文件 C++ 封装**：带 **Boost.JSON 自动绑定**（`bindJson`）的 WebView 桥接、信号/槽、`std::execution` 消息循环 scheduler，以及每个 C API 的薄封装。

只需包含一个头文件、链接一个 CMake target：

```cpp
#include <HeliosViewCore/HeliosView.h>
```

```cmake
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

当前全部基于 Windows (win32)；C API 是移植边界 —— 其他平台在它后面重新实现 `src/<platform>/`。

---

## 线程模型

**所有窗口 / WebView / 托盘 / 菜单 / 对话框 / 事件队列 API 必须在消息循环线程调用** —— 即运行 `App::exec()` 的线程（C 侧是调用 `heliosview_run` 的线程）。在其他线程调用是未定义行为。

**例外**（任意线程安全）：

- `App::postTask(fn)` —— 工作线程回到 UI 线程的官方途径（`app.quit()` 也可以）。
- WebView 的 `resolve` / `reject` / `broadcast`。
- 通知（`notificationShow` / `heliosview_notification_show`）—— OS toast 与线程无关。
- `heliosview_free`。

其余一切：仅限 UI 线程。

```cpp
helios::App app;
// 后台线程回到 UI 线程：
std::thread worker([app] {
    do_slow_work();
    app->postTask([] { /* 消息循环空闲时在 UI 线程执行 */ });
});
```

---

## 功能一览

| 领域 | API（C / C++） |
| --- | --- |
| 窗口 + 事件 | `heliosview_window_*` / `helios::Window`（样式、透明度、图标、置顶、隐藏、最小化/最大化/还原、可调整大小、最小/最大尺寸、拖拽区域、全屏、闪烁、禁用、DPI、焦点/移动/尺寸事件） |
| 屏幕几何 | `heliosview_*_work_area` / `System::screenWorkArea` / `Window::workArea`（多显示器） |
| 任务栏进度 | `heliosview_window_set_progress` / `Window::setProgress`（含状态、角标） |
| 会话结束 | `heliosview_set_session_end_callback` / `System::setSessionEndCallback`（关机保存） |
| 背景材质与深色模式（Win11） | `heliosview_window_set_backdrop/_dark_mode` / `Window::setBackdrop/setDarkMode` |
| WebView + JS 桥 | `heliosview_webview_*` / `WebViewWindow` + `bindJson` 自动绑定 |
| 托盘 + 菜单 | `heliosview_tray_*` / `heliosview_menu_*` / `Tray` / `Menu` |
| 对话框 | 文件夹/文件选择、消息框（`Dialogs.h`） |
| 系统辅助 | 剪贴板、打开 URL、资源管理器定位（`System.h`） |
| 通知（toast） | `heliosview_notification_*` / `Notification.h`（任意线程） |
| 消息循环、`std::execution` scheduler | `heliosview_run` / `App` |
| 异步 + HTTP 客户端 | `Async`（asio 线程池：定时器、socket）/ `http::Client`（keep-alive 连接池、TLS） |

---

## 内存分配

HeliosView 把**所有分配都路由到一个可配置的分配器**，因此内存可以来自 pool、arena 或其他分配器，而不是进程堆。这是 C API 和 C++ 封装共享的同一机制：配置一次，库分配的所有内存都走它。

**经验法则：** 无论你配置什么，**分配和释放必须使用同一个分配器**，并且必须在**创建任何对象之前**完成配置（在对象存活期间更换分配器是未定义行为）。

- **C** — `heliosview_set_allocator(&heliosview_allocator_t)` 把库默认的 `malloc`/`free` 换成你自己的。库交到你手里的字符串（对话框路径、剪贴板文本）必须用 **`heliosview_free`** 释放 —— 绝不要用平台的 `free()`（DLL 边界两侧的 CRT 堆可能不同）：

  ```c
  char* path = NULL;
  if (heliosview_select_folder(NULL, "Pick a folder", &path) == 1) {
      printf("folder: %s\n", path);
      heliosview_free(path);   /* 库返回的字符串一律配 heliosview_free */
  }
  ```

- **C++** — 封装使用同一个 C 分配器；它产出的 C++ 字符串是普通 `std::string`（UTF-8）。

> **一句话：** 在创建任何对象之前调用 `heliosview_set_allocator`，库返回的每个字符串都用 `heliosview_free` 释放。

---

## 构建

需要 CMake ≥ 4.3 和 C++23 编译器（C demo 用 C99）。所有第三方依赖要么内置（vendored）要么在配置时自动获取 —— **无 vcpkg、无系统包安装**：

| 依赖 | 版本 | 来源 | 用途 |
| --- | --- | --- | --- |
| WebView2 SDK | 1.0.4129.50 | 配置时从 NuGet 下载 | 内嵌 WebView（win32） |
| `stdexec` | 固定 commit 758f41f4（origin/main，2026-08-15，0.11.0+） | vendored（`third_party/stdexec/`） | C++23 协程（sender/receiver） |
| `Boost`（Asio/Beast/JSON） | 1.92.0（超项目 submodule，所需库配置时自动按需初始化） | vendored（`third_party/boost/`） | 后台线程池（`Async`）、HTTP（Boost.Beast）、WebView 桥接自动绑定（Boost.JSON） |

其余全部来自操作系统：窗口、对话框、toast（经 Windows SDK 的 WinRT）、DWM 背景材质。WebView2 SDK 是唯一在配置时获取的东西（`.nupkg` 其实就是个包含头文件和 WebView2Loader 库的 zip），缓存在构建目录中。Boost 库在配置时按需初始化：在 `third_party/boost/` 里执行**一条** `git submodule update --init --depth 1`，把所有需要的库作为 pathspec 一次传入（git 的输出会实时打印，`--jobs` 并行克隆，因此首次克隆不会看起来像卡住）——不要对整个 HeliosView 执行 `git submodule update --init --recursive`，那会拉取全部约 160 个 Boost 库。

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

DLL 和 demo 一起落在 `build/bin/`，无需配置 `PATH`。

`HELIOSVIEW_BUILD_EXAMPLES=ON`（默认）会构建演示程序：

| demo | 文件 | 演示内容 |
| --- | --- | --- |
| `HeliosViewDemo` | `examples/main.cpp` | **WebView 总演示**：滑块 + 输入框驱动全部功能 |
| `HeliosViewWindowDemo` | `examples/window_demo.cpp` | 基础窗口 + 信号/槽 + 托盘 + 菜单 |
| `HeliosViewAppDemo` | `examples/app_demo.cpp` | `Window` 子类化、窗口样式、成员函数槽、窗口 API |
| `HeliosViewSystemDemo` | `examples/system_demo.cpp` | 对话框、剪贴板、toast、任务栏进度、托盘气泡 |
| `HeliosViewWebViewDemo` | `examples/webview_demo.cpp` | **WebView + `bindJson` 自动绑定** |
| `HeliosViewWebViewEventsDemo` | `examples/webview_events_demo.cpp` | 导航事件、本地文件夹映射、文件夹对话框 |
| `HeliosViewCDemo` | `examples/c_demo.c` | **纯 C** 消费者 |

---

## 发布包 —— SDK

GitHub Release 资产是面向 Windows x64 的完整 **SDK zip**
（`HeliosView-<版本>-win64-SDK.zip`），使用者无需自己构建库：

```
bin/        HeliosView.dll + WebView2Loader.dll + 预编译示例（解压即用）
lib/        HeliosView.lib + libboost_json.lib + CMake 包配置（find_package）
include/    C API 头文件（HeliosView/）、C++ 封装头文件（HeliosViewCore/）、vendored 的 stdexec + Boost
examples/   示例源码——可独立针对 SDK 构建
README.md   使用说明（zip 内已附带）
```

CMake 消费方式：

```cmake
find_package(HeliosView REQUIRED)      # -DCMAKE_PREFIX_PATH=<SDK 根目录>
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

纯 C 消费方式：包含 `include\`、链接 `lib\HeliosView.lib`，并把 `bin\` 里的
`HeliosView.dll` 和 `WebView2Loader.dll` 拷到 exe 旁边。完整说明见
[packaging/README.md](packaging/README.md)，它会作为 zip 内的 `README.md` 一并发布。

---

## 教程

教程按依赖顺序展开：**App**（消息循环）→ **Signals** → **Window** → **WebView**（库的核心），然后是新系统 API 和支撑一切的 C API。

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

    return app.exec();                     // 消息循环；quit() 或
                                           // 最后一个窗口关闭时返回
}
```

`exec()` 运行消息循环（正常退出返回 0）；`quit()` 请求退出，可从任意线程调用。`pollEvent` / `waitEvent` / `postEvent` 提供底层队列访问；可重写 `event()` 处理应用级未处理事件。消息循环同时是一个 `std::execution::scheduler`：`std::execution::schedule(app.get_scheduler()) | std::execution::then(fn)`。

### 2. 信号与槽

`helios::Signal<Args...>` 持有槽并在发射时调用它们；`connect` 返回槽 id，供 `disconnect(id)` 使用：

```cpp
helios::Signal<int32_t, int32_t> resized;
resized.connect([](int32_t w, int32_t h) { std::println("resized {}x{}", w, h); });
resized(800, 600);
```

槽有三种形式 —— 同步可调用对象、成员函数（`connect(&MyWindow::onKeyPressed, this)`），以及**异步槽**（返回 sender 的可调用对象，fire-and-forget 启动）。Window、WebView、Tray… 把事件暴露为现成的信号。

### 3. 窗口

`helios::Window` 是由 App 消息循环驱动的顶层窗口。信号上报输入：

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
            window.close();          // 最后一个窗口关闭 -> 循环退出
    });
    window.closeRequested.connect(&Window::close, &window);  // 关闭按钮（×）触发此信号

    return app.exec();
}
```

`Window` 还提供 `showMinimized/Maximized/Normal`（以及便捷的 `minimize`/`maximize`/`restore`/`toggleMaximize`）、`move/resize`、`position/size/geometry`、`setTitle`、`center`、`setOpacity`、`focus`、`hide`、`setTopmost`、`setIcon`、`requestClose`、`setResizable`、`setProgress`（任务栏）、`setBackdrop(Mica/Acrylic)` + `setDarkMode`（Win11）、`dpi`，以及 `WindowStyle::{Normal, Borderless, Frameless}`。`focused`/`blurred` 信号上报窗口激活状态变化。标题与字符串均为 UTF-8。

**文本输入与修饰键状态。** `keyPressed` 只在首次按下时触发，`keyRepeated` 用于系统自动重复，`keyEvent(const KeyEvent&)` 覆盖每次按下/释放并带 `modifiers`（`helios::mods::Ctrl | helios::mods::Shift | …`，含左右键位与锁定状态）和 `repeat` 标志。`textInput(const std::string&)` 按输入内容投递 UTF-8 文本（包含输入法上屏）；超过事件缓冲区的长文本会拆成连续的多个事件，不会截断。C API 中同样的信息在 `heliosview_event_t` 里（`modifiers`、`flags`、`text`/`text_len`，事件类型 `HELIOSVIEW_EVENT_TEXT_INPUT`）。

**关闭按钮行为。** 点击关闭按钮（×）或按 Alt+F4 **不会**销毁窗口——它只发出 `closeRequested` 信号。需要连接该信号并调用 `close()` 才能真正关闭：

```cpp
window.closeRequested.connect(&Window::close, &window);  // 点击 × → 关闭
```

**无边框窗口拖拽。** 无边框 / 无标题栏窗口没有系统标题栏，因此把自定义标题栏条带注册为拖拽区域即可 —— 在区域内按下并拖动会像原生标题栏一样移动窗口（`WM_NCHITTEST → HTCAPTION`）：

```cpp
helios::Window win(480, 320, "Frameless", helios::WindowStyle::Frameless);
win.addDragRegion(0, 0, 480, 40);          // 标题栏条带
win.show();
```

> **WebView 注意事项。** 拖拽区域依赖宿主窗口的 `WM_NCHITTEST`。全幅 WebView 是覆盖整个客户区的子窗口，其上的命中测试由 WebView 自身的窗口过程应答、永远不会到达宿主——因此 WebView 覆盖范围内注册的拖拽区域**不生效**。使用 `WebViewWindow` 时，应在页面侧注册拖拽区：注入的 `<helios-window-title-bar>` 组件通过 WebView2 原生 `app-region: drag` 支持拖拽（库已启用 `IsNonClientRegionSupportEnabled`），或把 `startDrag()` 绑定到页面回调实现完全自定义的区域。

**Web 绘制标题栏（推荐）。** 因为铺满的 WebView 无法被原生 DWM caption 按钮覆盖（"无可见标题栏"与"系统标题栏按钮"在 Win32 上互斥），标题栏最自然的做法就是在页面里画。桥 shim 在每个页面注入两个 web component：

- `<helios-window-title-bar>` —— 放在页面顶部：通过 WebView2 的**原生 `app-region: drag` 支持**拖动窗口（库已启用 `IsNonClientRegionSupportEnabled`——无桥接往返、无需绑定），双击切换最大化；
- `<helios-window-controls>` —— 放进标题栏里：绘制 Win10/11 最小化 / 最大化 / 关闭字形（hover / 按下反馈，关闭键 hover 变红），调用内置 `__hv.control` / `__hv.state` 桥；最大化时按钮自动切换为还原字形（窗口不可调整大小时最大化按钮禁用）。它会给自己的按钮自动加 `app-region: no-drag`，保证可点击。

```cpp
auto win = std::make_shared<helios::WebViewWindow>(
    900, 640, "App", helios::WindowStyle::Frameless);
win->show();
win->createWebView();
// 就这些——拖拽是原生 app-region，按钮用内置的 __hv.control / __hv.state 桥
// （__hv.state 还返回 titleBarHeight，即 DPI 缩放的标题栏条带高度）。
```

```html
<helios-window-title-bar>
  <span>App</span>
  <helios-window-controls></helios-window-controls>
</helios-window-title-bar>
```

组件可用 CSS 在元素上自定义（标题栏默认 48px flex 行；按钮浮在右上角）。标题栏内其他交互子元素需要加 `app-region: no-drag` 才能保持可点击。

**调整大小。** `Frameless` 风格保留四周一圈非客户区边框（`WM_NCCALCSIZE`），系统会绘制可抓取的边框并原生处理调整大小——即使全幅 WebView 也没关系（边框是非客户区，系统直接命中测试）。页面无需任何改动。

> 原生 DWM 按钮必须保留系统 caption（即"普通窗口"），而铺满的 WebView 覆盖不了它；web 自绘按钮也没有 Win11 snap layouts 悬浮菜单（那需要真实 caption）。

**自定义控制按钮。** 或者自行绘制按钮并注册其矩形 —— 库把它们接到真实的标题栏行为上（点击执行动作且不会触发拖动；最大化 / 还原自动切换）。

**DPI。** 在创建任何窗口之前调用一次 `helios::enableDpiAwareness()`，使进程按显示器感知 DPI（v2）；`window.dpi()` 返回窗口当前 DPI。

**屏幕几何。** `System::screenWorkArea`、`Window::workArea` 与 `System::primaryWorkArea` 返回显示器可用区域（不含任务栏）的屏幕坐标 —— 便于在多显示器环境下居中 / 定位窗口。`System::cursorPosition` 返回鼠标位置。

**尺寸限制、全屏、闪烁与模态锁。** `setMinimumSize` / `setMaximumSize` 限制客户端尺寸（`WM_GETMINMAXINFO`）；`setFullscreen` 铺满整个显示器，退出时恢复之前的几何与样式；`flash` / `flashUntilFocus` 闪烁任务栏按钮（后台任务完成或紧急通知）；`setEnabled(false)` 锁定窗口输入以模拟模态。`moved` / `moving` / `sizing` / `enabledChanged` 信号上报窗口状态变化。

**会话结束。** `System::setSessionEndCallback` 在 OS 会话结束前（关机 / 重启 / 注销）于消息循环线程同步调用，供应用保存状态；返回非零可否决关机。

### 4. WebView —— 核心：JS ↔ 原生桥接

**这是库的核心。** `WebViewWindow` 是嵌入了 WebView2 浏览器的 `Window` 子类；`createWebView()` 负责挂载（初始化是异步的，期间的导航请求会被排队）。在它之上，**`bindJson<Args...>`** 是主打特性：JS 调用的每个参数都被反序列化为对应的 `Args` 类型（Boost.JSON），处理器以分离的 `std::execution::task<Resp>` 协程运行，结果再序列化回去 resolve 对应的 JS `Promise`：

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <boost/describe.hpp>   // BOOST_DESCRIBE_STRUCT（DTO 注解）

struct AddReq { int a; int b; };
BOOST_DESCRIBE_STRUCT(AddReq, (), (a, b))

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

`bindJson` / `subscribeJson` 也接受**成员函数**（传入对象指针和成员指针）。桥接 shim 暴露 `window.helios.call(name, ...)` → `Promise` 和**双向 `BroadcastChannel`**（`broadcast` 原生→JS，`subscribe` JS→原生）。所有桥接名字必须是 C 标识符 `[A-Za-z_][A-Za-z0-9_]*`；库的内置桥使用 **`__hv.` 前缀名**（`__hv.control` / `__hv.state` / `__hv.drag`，供注入的组件调用）——它们含有点号、**不是合法标识符**，应用无法绑定或订阅，永远遮蔽不了内置组件。

**事件、本地资源与原生对话框**：

- **导航事件** — `WebViewWindow` 上的四个信号，都在 UI 线程上触发：`navigationStarting`（配合 `navigationStartingGate` 否决用 std::function）、`urlChanged`、`titleChanged`、`navigationCompleted`。
- **`mapLocalFolder(host, folder)`** + **`localUrl(host, path)`** — 服务前端之外的本地资源文件夹；URL 必须用 `localUrl()` 生成（Windows 是虚拟 `https://<host>/` 主机，其他引擎注册自定义 scheme，别把 URL 写死）。
- **`helios::selectFolder`** 等 — 通过 `bindJson` 处理器暴露给页面的原生对话框。

**WebView2 窗口 chrome 开关** — 调整 WebView 原生外观的几个小开关，`WebViewWindow` 上都有对应方法（C++：`setStatusBarEnabled`、`setContextMenuEnabled`、`setDevToolsEnabled`；C：`heliosview_webview_set_status_bar` / `heliosview_webview_set_context_menu` / `heliosview_webview_set_devtools`）：

- **状态栏** — 悬停链接时左下角显示的 URL 提示；**默认关闭**，用 `setStatusBarEnabled(true)` 重新开启。
- **右键菜单** — WebView2 默认右键菜单（复制 / 粘贴 / 检查）；默认开启，用 `setContextMenuEnabled(false)` 关闭（通过 `ContextMenuRequested` 事件抑制；需要 WebView2 运行时 ≥ 100）。
- **DevTools** — F12 / 右键“检查”；默认开启，用 `setDevToolsEnabled(false)` 关闭（关闭时会同时关掉已打开的 DevTools 窗口）。

每个开关在 WebView 初始化完成后立即生效；初始化期间调用则在其就绪时应用。

原始 C 风格桥接（`bind` / `resolve` / `reject` / `eval` / `evalAsync` / `broadcast` / `subscribe`）也可用；`resolve`/`reject`/`broadcast` 线程安全。

> **生命周期：** 只在没有 `bindJson` task 或 `evalAsync` 调用仍在执行时销毁 `WebViewWindow`。WebView 必须在它的父窗口之前销毁。

### 5. Async + HTTP 客户端（连接池）

`helios::Async` 是一个接入 `std::execution` 的 asio 线程池：定时器、socket 和 HTTP 客户端都跑在同一批工作线程上。`helios::http::Client` 是轻量句柄（可拷贝、可作临时对象），按 origin 维护 **keep-alive 连接**并复用：

```cpp
helios::Async async;                       // app 级成员
helios::http::Client client{async};        // 一个句柄，共享同一个连接池

window->bindJson<Req>("api", [&client](Req r) -> std::execution::task<boost::json::value> {
    auto resp = co_await client.get(r.url);          // 能复用就走池里的连接
    co_return boost::json::value{{"status", resp.status}, {"body", resp.body}};
});
```

连接池参数（都可选；默认空闲 60 秒、每个 origin 最多留 4 条、总共 16 条）：

```cpp
helios::http::PoolOptions opt;
opt.idle_timeout = 30s;        // 空闲超过这个时间就丢弃
opt.max_idle_per_origin = 2;
opt.max_idle_total = 8;
opt.dns_cache_ttl = 60s;       // 解析结果按 origin 缓存
opt.keep_alive = false;        // 关闭池化：发 "Connection: close"，不缓存连接
helios::http::Client client{async, 10s, "cacert.pem", opt};

client.idle_connections();     // 诊断用
client.close_idle();           // 立刻丢弃空闲连接
client.clear_dns_cache();
```

空闲连接可能被服务端单方面关闭：此时**幂等**请求（GET/HEAD/OPTIONS/PUT/DELETE/TRACE）会在新连接上重试一次，而 POST/PATCH 直接报错，避免重复副作用。每一步（解析、连接、TLS 握手、收发）都受客户端超时约束。`https://` 会用 `cacert.pem`（或 OpenSSL 默认路径）校验证书，并且每个 client 只建一次 SSL context。

### 6. 线程契约实战

所有 UI API 运行在 `App::exec` 线程。后台工作在你自己管理的线程 / 线程池 / 任意异步库里进行 —— 通过 `App::postTask` 回到 UI 线程：

```cpp
helios::App app;
helios::Window window(800, 600, "Demo");
window.show();

std::thread worker([app] {
    // ... 在这个线程上做耗时工作 ...
    app->postTask([app] {
        // 回到 UI 线程：这里可以安全地操作窗口/webview
        std::println("done");
    });
});
worker.detach();

return app.exec();
```

### 7. 对话框与系统辅助

所有原生对话框都是模态的，必须在消息循环线程调用。选中的路径以 UTF-8 `std::string` 返回：

```cpp
// 消息框
helios::MessageBoxResult r = helios::messageBox(
    window.nativeHandle(), helios::MessageBoxType::Question,
    helios::MessageBoxButtons::YesNo, "Question", "Continue?");

// 文件夹选择
std::string folder;
if (helios::selectFolder(window.nativeHandle(), "Pick a folder", folder))
    std::println("folder: {}", folder);

// 文件选择（单选或多选；结构化过滤器：名称 + 扩展名规则）
auto files = helios::openFiles(window.nativeHandle(), "Pick images",
                               std::vector<helios::FileFilter>{
                                   {"Images", "png;jpg;jpeg"},
                                   {"All files", "*.*"}
                               },
                               /*multi=*/true);

// 保存对话框
std::string path;
if (helios::saveFile(window.nativeHandle(), "Save as",
                     std::vector<helios::FileFilter>{{"Text", "txt"}}, "out.txt", path))
    std::println("saving to {}", path);

// 剪贴板
helios::clipboardSetText("hello");
std::string clip;
if (helios::clipboardGetText(clip)) { /* ... */ }

// 浏览器打开 / 资源管理器定位
helios::openUrl("https://example.com");
helios::showInFolder("C:\\path\\to\\file.txt");
```

### 8. 通知（toast）

现代 OS toast。**与线程无关**：可从任意线程调用（回调运行在未指定的线程上，碰 UI 前先切回循环线程）。启动时初始化一次（注册应用标识：Windows 是 AppUserModelID + 开始菜单快捷方式，macOS 是 bundle id，Linux 是 desktop id）：

```cpp
helios::App::setAppId("com.example.myapp");           // 也可以直接传给 init
helios::notificationInit();                           // 启动时一次
helios::notificationRequestPermission([](helios::NotificationPermission p) {
    // macOS：系统弹窗已答复；Windows/Linux：直接报告系统设置
    std::println("permission = {}", static_cast<int>(p));
});
helios::notificationSetClickCallback([](const char* title, const char* body) {
    /* 用户点击了通知 */
});
helios::notificationShow("Download", "Finished");     // 任意线程
```

macOS 在用户授权前会静默丢弃通知，所以启动时就申请权限。Windows 上全新应用标识的**首次运行**会返回 `NotificationPermission::Unknown`（系统异步注册），这不代表被拒绝。

### 9. 托盘图标 + 弹出 / 右键菜单

`helios::Tray` 在系统通知区显示一个图标；`helios::Menu` 是弹出 / 右键菜单。菜单是**独立对象**（构建时不需要窗口，`show(window)` 的 owner 可空），它显示的是 **action**：action 拥有命令本身（文本、启用/勾选状态、`triggered`），菜单只拥有布局。同一个 action 可以出现在多个菜单里：

```cpp
helios::Action copy("Copy");                 // 共享命令
copy.triggered.connect(&onCopy, this);

helios::Menu menu;                           // 不需要窗口
helios::Menu::Item* show = menu.addItem("Show / Restore");   // 菜单拥有的 action
helios::Menu::Item* quit = menu.addItem("Quit");
helios::Menu::Item* top = menu.addCheckItem("Toggle Topmost"); // 可勾选
menu.addAction(copy);                                // 共享的 action
menu.addSeparator();
menu.setDefaultAction(*show);                        // 加粗的默认项
show->triggered.connect([&] { window.showNormal(); });
quit->triggered.connect([&] { app.quit(); });
editMenu->addAction(copy);                           // 同一个命令，另一个菜单
copy.setEnabled(false);                              // 两个菜单同时变灰

// 标准 role：文案/快捷键由平台提供，app 做不到的动作由库执行
editMenu->addRole(helios::MenuRole::Cut);            // macOS ⌘X / Windows Ctrl+X
editMenu->addRole(helios::MenuRole::Copy);
editMenu->addRole(helios::MenuRole::Paste);
windowMenu->addRole(helios::MenuRole::Minimize);
appMenu->addRole(helios::MenuRole::Quit);            // macOS "Quit <App>" ⌘Q / Windows "Exit"

helios::Action open("Open…", "Primary+O");           // 可移植快捷键字符串
open.triggered.connect(&onOpen, this);
fileMenu->addAction(open);

// 应用菜单栏：macOS 装进唯一那条全局菜单栏（第一个菜单 = App 菜单）；
// Windows 显示为每个窗口的菜单栏（包括之后创建的窗口）。
// helios::MenuBar 是独立的横向菜单栏类（Win32 是 CreateMenu；macOS 是装成
// NSApp.mainMenu 的那个 NSMenu；Linux 是 GtkMenuBar），里面横向排列一个个
// 垂直的 Menu（通过 bar.addMenu() 添加）。
helios::MenuBar bar;
bar.addMenu("File")->addAction(open);
bar.addMenu("Edit")->addRole(helios::MenuRole::Copy);
bar.setAppMenu();

// action 上的快捷键就是全局加速键：heliosview_run / heliosview_pump_events 会翻译；
// 自带消息循环的 app 在 DispatchMessage 前调 heliosview_translate_accelerator(&msg)

helios::Tray tray("Tray Demo");
tray.setMenu(menu);                                    // 可移植的右键菜单
tray.leftClicked.connect([] { /* ... */ });
tray.notify("Tray", "Hello");                          // 气泡通知（无需配置）

// 用 setMenu 挂菜单，而不是在 rightClicked 里自己弹：Linux 的菜单由桌面环境
// 通过 DBus 导出（应用自己弹不出来），macOS 的 NSStatusItem 菜单任意点击都会
// 打开 —— 所以挂了菜单后点击事件可能根本不会送达。用 tray.setMenu(nullptr) 解除。
```

窗口外观同样可移植：`WindowStyle` 定基线，`WindowFlag` 细化，同一份代码就能得到各平台惯用的样子：

```cpp
// macOS：真标题栏 + 隐藏标题 + 内容延伸到标题栏下，红绿灯浮在页面之上。
// Windows：没有原生标题栏，应用自己画 chrome。
helios::Window w(900, 600, "App", helios::WindowStyle::Normal,
                 helios::WindowFlag::TitleBarHidden
                     | helios::WindowFlag::TitleBarTransparent
                     | helios::WindowFlag::FullSizeContent);
w.scaleFactor();      // 1.0 / 2.0 —— 逻辑单位换算到物理像素
w.flags();            // 创建时传入的 flags
```

### 10. C API

每个 C++ 特性都是对 `include/HeliosView/heliosview.h` 的薄封装 —— 纯 C 头（`extern "C"`、POD 类型、ABI 上不跨 C++ 对象或异常）。它足够完整，可以**完全不写 C++** 就构建应用（见 C99 示例 `HeliosViewCDemo`）。所有字符串都是 UTF-8。

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

    heliosview_run(frame, NULL);   /* 消息循环；NULL 回调 = 空转 */

    heliosview_tray_destroy(tray);
    heliosview_window_destroy(win);
    return 0;
}
```

库返回的字符串（对话框路径、剪贴板文本）用 `heliosview_free` 释放。C 面完整镜像 C++ 特性：窗口 + 事件、托盘/菜单、WebView 桥接、对话框、系统辅助、通知 —— 详见 `heliosview.h` 中记录的契约（线程、生命周期、错误码）。

---

## 仓库结构

```
include/HeliosView/heliosview.h       C API（唯一的外部 ABI）
include/HeliosViewCore/               纯头文件 C++ 封装
  HeliosView.h                        汇总头文件
  Signal.h                            信号/槽（同步 + 异步槽）
  Types.h                             事件类型（与 C API 一一对应）
  App.h                               消息循环 + UI 线程 scheduler
  Window.h                            顶层窗口 + 状态/拖拽/DPI/任务栏/背景材质 API
  Dialogs.h                           原生对话框 + 消息框
  System.h                            剪贴板 / 打开 URL / 资源管理器定位
  Notification.h                      OS toast 通知（线程安全）
  Tray.h                              系统通知区（托盘）图标 + 信号
  Menu.h                              弹出 / 右键菜单 + 信号
  Execution.h                         scheduler/sender（stdexec，P2300）
  WebViewWindow.h                     内嵌 WebView 的窗口
  WebViewJson.h                       bindJson / subscribeJson（Boost.JSON 自动绑定）
src/heliosview.cpp                    平台无关核心
src/heliosview_internal.h             实现文件间共享的状态
src/win32/                            win32 后端（窗口、WebView2、对话框、toast）
third_party/stdexec/                  内置 stdexec（固定 commit，纯头文件）
examples/                             演示程序
```

## 路线图

- 在 C ABI 之后支持更多平台（Linux/macOS 后端）。
- 更多 WebView 事件（历史（前进/后退）、页面加载发起的对话框 / 打印 / 右键菜单事件）。
- 多选任务栏角标、颜色/字体选择器。
