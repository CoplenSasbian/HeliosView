# HeliosView

> **[English](README.md) | 简体中文**

一个现代、高性能的 C++23 桌面 GUI 开发框架，融合了 **原生 Win32 窗口系统**、**Chromium WebView2 深度集成**、**Blend2D JIT 驱动的 2D 矢量画布**、**保留模式 UI 组件系统** 以及 **C++23 stdexec 异步协程运行时**。

HeliosView 采用清晰的两层架构：

- **`HeliosView.dll`** — 纯 **C99 ABI**（`extern "C"`，标准 POD 类型，零 C++ 异常跨 DLL 边界）。涵盖顶层窗口、子视口宿主（`heliosview_host_t`）、WebView2 引擎桥接、JIT 加速 2D 矢量画布（`heliosview_canvas_t`）、保留模式 UI 组件调度（`heliosview_ui.h`）、系统托盘、菜单、原生文件对话框、WinRT 系统通知、任务栏进度与 Win11 DWM 背景材质（Mica、Acrylic、深色模式）。
- **`HeliosView.Core`** — 构建在 C API 之上的**纯头文件 C++23 框架**：
  - **子视口宿主架构（Child Viewport Host）**：在同一个顶层窗口内无缝混编原生 2D 自绘画布视口（`UIHost`）与 Web 视口（`WebViewHost`）。
  - **Blend2D 矢量直绘引擎**：集成 JIT 编译的软件光栅化器（`Canvas`、`Painter`、`BufferPresenter`、`PixelView`），支持高质量亚像素抗锯齿、仿射变换、渐变笔刷、贝塞尔路径与文字排版。
  - **保留模式 UI 组件树**：可自由嵌套与组合的控件体系（`Widget`、`Button`、`Slider`、`Switch`、`Checkbox`、`ProgressBar`、`SegmentedControl`、`Card`、`VStack`、`HStack`、`CustomWidget`），内建自动布局与事件冒泡机制。
  - **双向 WebView2 RPC 桥**：基于 Boost.Describe 的自动参数类型推导绑定（`bindJson`）、强类型 DTO 映射、`broadcast`/`subscribe` 发布订阅机制，以及免本地端口的虚拟 URI 资源映射。
  - **现代异步与协程运行时**：集成 `stdexec`（P2300 Senders/Receivers）协程体系（`std::execution::task`）、Boost.Asio 后台工作线程池（`Async`），以及支持 Keep-Alive 连接池的 Boost.Beast HTTP/1.1 客户端。

只需包含一个头文件、链接一个 CMake target：

```cpp
#include <HeliosViewCore/HeliosView.h>
```

```cmake
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

---

## 核心架构设计

```
+-----------------------------------------------------------------------------------+
|                                  HeliosView.Core                                  |
|                                (纯头文件 C++23 框架)                               |
+------------------------+------------------------+---------------------------------+
|      现代原生窗口壳    |      子视口宿主架构    |        现代异步与协程执行       |
|  - 窗口样式、无边框    |  - UIHost (2D自绘画布) |  - stdexec (P2300 Senders)      |
|  - Mica, Acrylic, 深色 |  - WebViewHost (Web视口|  - Boost.Asio 后台工作线程池    |
|  - 托盘、菜单、对话框  |  - 旗舰双视口混合 Studio|  - Boost.Beast Keep-Alive HTTP  |
+------------------------+------------------------+---------------------------------+
|   WebView2 RPC 桥接    |    Blend2D 矢量直绘    |        保留模式 UI 组件树       |
|  - bindJson 自动推导   |  - JIT x86_64 光栅化器 |  - 控件继承树、自动布局容器     |
|  - 强类型 DTO 与协程   |  - Painter、路径、文字 |  - Button, Slider, Switch 等    |
|  - broadcast/subscribe |  - BufferPresenter GDI |  - 事件捕获、冒泡与自定义控件   |
+------------------------+------------------------+---------------------------------+
|                                   HeliosView.dll                                  |
|                         (稳定 C99 ABI 与底层跨平台移植边界)                       |
+-----------------------------------------------------------------------------------+
```

---

## 线程模型

**所有涉及 UI 操作的 API（窗口、视口宿主、WebView、画布自绘、UI 控件、托盘、菜单、对话框以及事件循环）必须在消息循环线程调用** —— 即执行 `App::exec()` 的线程（C 接口中即调用 `heliosview_run` 的线程）。在其他工作线程直接调用 UI 相关 API 属于未定义行为。

**例外（可在任意线程安全调用）：**
- `App::postTask(fn)` —— 将工作函数投递回 UI 线程队列安全执行（跨线程唤醒官方途径）。
- `app.quit()` —— 请求退出消息循环。
- WebView 异步响应与广播：`resolve`、`reject`、`broadcast`。
- 操作系统 Toast 通知（`notificationShow` / `heliosview_notification_show`）。
- 后台计算线程池任务（`helios::Async`）。
- 内存释放：`heliosview_free`。

```cpp
helios::App app;

// 在后台线程执行耗时计算，并安全切回 UI 线程更新界面：
std::thread worker([app] {
    do_heavy_computation();
    app->postTask([] {
        // 安全在 UI 主消息循环线程执行
        update_window_ui();
    });
});
worker.detach();

return app.exec();
```

---

## 功能特性全景

| 领域 | C API | C++ Core API | 关键能力 |
| --- | --- | --- | --- |
| **窗口系统** | `heliosview_window_*` | `helios::Window` | 普通、无边框、完全自定义标题栏；Mica/Acrylic 亚克力材质；DWM 深色模式；标题栏拖拽区域；Per-Monitor v2 DPI 缩放；几何约束、最小/最大尺寸、模态禁用锁定、任务栏进度条 |
| **子视口宿主** | `heliosview_host_*` | `helios::UIHost`, `helios::WebViewHost` | 宿主窗口内多视口无缝嵌入；自绘画布与 Web 视口同屏分屏混合应用；动态排版定位与显隐控制 |
| **2D 矢量画布** | `heliosview_canvas_*` | `helios::Canvas`, `Painter`, `BufferPresenter` | Blend2D JIT 软件光栅化引擎；亚像素级高质量抗锯齿；路径变换、渐变填充、文字渲染；GDI 快速双缓冲直刷呈现 |
| **保留模式 UI** | `heliosview_ui_*` | `helios::ui::Widget`, `VStack`, `HStack` | 内置 `Button`、`Slider`、`Switch`、`Checkbox`、`ProgressBar`、`SegmentedControl`、`Card`、`CustomWidget`；树状层次布局与鼠标命中分发 |
| **现代 WebView2** | `heliosview_webview_*` | `helios::WebViewWindow`, `WebViewHost` | Chromium 内核；页面 DOM 完整支持；虚拟本地资源服务器映射（`localUrl`）；开发者工具、页面缩放、低资源占用模式 |
| **RPC 桥接** | `heliosview_webview_bind` | `bindJson`, `subscribeJson`, `broadcast` | 基于 Boost.Describe 参数类型全自动推导；`std::execution::task` 协程处理器；双向发布订阅消息流 |
| **异步与协程** | `heliosview_run` | `helios::Async`, `std::execution` | Boost.Asio 线程池；P2300 Senders/Receivers 标准流水线；C++23 协程原生支持（`co_await`, `co_return`） |
| **HTTP 客户端** | — | `helios::http::Client` | Boost.Beast Keep-Alive 长连接池；幂等请求空闲断连自动重试；SSL/TLS 证书校验 |
| **托盘与系统菜单** | `heliosview_tray_*`, `heliosview_menu_*` | `helios::Tray`, `Menu`, `MenuBar`, `Action` | 系统通知区托盘图标与气泡；上下文弹出右键菜单；跨平台窗口菜单栏；标准系统角色与快捷键加速器 |
| **系统对话框与辅助** | `heliosview_dialog_*`, `heliosview_system_*` | `Dialogs.h`, `System.h` | 原生打开/保存文件选择（单选/多选/格式过滤）；文件夹选择；原生消息弹窗；剪贴板读写；系统浏览器打开；资源管理器定位；全局热键 |
| **Toast 系统通知** | `heliosview_notification_*` | `Notification.h` | WinRT 现代横幅通知；点击事件回调；权限查询；任意线程可安全触发 |

---

## 内存分配模型

HeliosView 内部所有的动态内存分配均统一路由到一个可配置的内存分配器中。这保证了跨 DLL 动态链接边界时的内存安全，并支持直接嵌入定制化内存池或游戏引擎 Arena：

- **C 接口**：在**创建任何对象之前**调用 `heliosview_set_allocator(&allocator)`。库向用户代码返回的字符串（如文件对话框返回的路径、剪贴板获取的文本）必须统一使用 **`heliosview_free`** 进行释放，切勿直接调用系统 CRT 的 `free()`。
- **C++ 接口**：C++ 包装层自动管理底层内存；返回给用户的字符串均为标准 UTF-8 `std::string`，无需手动释放。

```c
char* selected_folder = NULL;
if (heliosview_select_folder(NULL, "选择项目目录", &selected_folder) == 1) {
    printf("用户选择: %s\n", selected_folder);
    heliosview_free(selected_folder); // 必须使用 heliosview_free 释放库返回的字符串
}
```

---

## 构建与依赖说明

环境要求：**CMake ≥ 4.3** 与 **支持完整 C++23 的编译器**（如 MSVC 19.38+ / Visual Studio 2022+）。

所有外部第三方依赖均已通过 Git Submodule 内置或配置时自动拉取 —— **无需 vcpkg、Conan 或安装任何全局第三方开发包**：

| 依赖库 | 版本 | 来源方式 | 核心用途 |
| --- | --- | --- | --- |
| **WebView2 SDK** | 1.0.4129.50 | CMake 配置时自动从 NuGet 拉取 | Win32 Chromium WebView2 运行时加载器 |
| **Blend2D** | v0.21.3 | Git submodule (`third_party/blend2d`) | 内建 JIT 2D 矢量光栅化画布引擎 |
| **asmjit** | 固定 commit `dffd8b1` | Git submodule (`third_party/asmjit`) | Blend2D 的 x86/ARM JIT 汇编器后端 |
| **stdexec** | 固定 commit `758f41f4` | Git submodule (`third_party/stdexec`) | P2300 Senders/Receivers 与 C++23 协程执行模型 |
| **Boost** | 1.92.0 | Git submodule (`third_party/boost`) | Asio（线程池）、Beast（HTTP）、JSON（RPC 自动绑定） |

### 源码克隆与构建

```sh
# 克隆仓库与子模块
git clone --recurse-submodules https://github.com/CoplenSasbian/HeliosView.git
cd HeliosView

# 使用 Ninja 配置构建工程
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# 一键编译 DLL、静态库、模块化示例与测试集
cmake --build build
```

编译产出的动态库、示例与测试程序均统一输出在 `build/bin/` 目录下，直接双击或命令行即可运行，无需额外配置 `PATH`。

---

## 模块化 Showcase 示例与测试集

工程在 `examples/` 目录下提供 6 个结构清晰的模块化演示程序，并在 `tests/` 目录下提供核心单元测试：

| 目标名称 | 源码路径 | 核心展示内容 |
| --- | --- | --- |
| **`HeliosView_ConsoleCoreDemo`** | `examples/01_console_core/` | 纯控制台无窗口应用：展示 `helios::Async` 线程池、`stdexec` 协程流水线与 Boost.Asio HTTP Keep-Alive 本地客户端/服务端通信。 |
| **`HeliosView_WindowShellDemo`** | `examples/02_window_shell/` | 原生 Win32 现代窗口外壳：自定义无边框窗口、Mica / Acrylic 材质、DWM 深色模式、原生菜单、系统托盘、文件对话框、Toast 通知与任务栏进度条。 |
| **`HeliosView_WebViewBridgeDemo`** | `examples/03_webview_bridge/` | 现代 WebView2 桥接全特性：毛玻璃 HTML5 仪表盘、`bindJson` 自动推导强类型 DTO 协程通信、`broadcast`/`subscribe` 广播流与免端口虚拟本地资源映射。 |
| **`HeliosView_CanvasUiDemo`** | `examples/04_canvas_ui/` | 高性能 2D 矢量直绘与保留模式 UI 画廊：4 选项卡包含 表单交互控件（Button, Slider, Switch, ProgressBar）、实时分析折线图与十字标尺、矢量贝塞尔路径与旋转齿轮组、半圆汽车仪表盘与 60FPS CRT 双通道示波器。 |
| **`HeliosView_StudioHybridDemo`** | `examples/05_studio_hybrid/` | 旗舰双视口混合 Studio：单窗口内无缝集成顶部原生工具栏、左侧 DirectDraw 自绘侧边栏（`UIHost`）与右侧现代 WebView2 网页宿主（`WebViewHost`）。 |
| **`HeliosView_CApiDemo`** | `examples/06_c_api/` | 纯 C99 ABI 接口巡览：无需任何 C++ 编译器，使用纯 C 完成窗口创建、事件循环分发、系统托盘、上下文菜单与原生对话框调用。 |
| **`HeliosView_CanvasTest`** | `tests/canvas_test.cpp` | 无头 2D 矢量渲染单元测试：验证 Blend2D 矢量路径、矩阵变换、像素填充与图片编码解码正确性。 |
| **`HeliosView_PresenterTest`** | `tests/presenter_test.cpp` | 图像呈现单元测试：验证 `PixelView` 内存切片与 `BufferPresenter` 像素呈现逻辑。 |

直接启动任意演示程序：

```sh
./build/bin/HeliosView_StudioHybridDemo.exe
```

---

## SDK 发布包使用指南

HeliosView 每次版本发布均提供面向 Windows x64 的独立 **SDK 压缩包**（`HeliosView-<version>-win64-SDK.zip`）：

```
bin/        HeliosView.dll + WebView2Loader.dll + 预编译示例可执行程序
lib/        HeliosView.lib + libboost_json.lib + CMake 导入配置文件
include/    C API 头文件 (HeliosView/), C++ 封装 (HeliosViewCore/), stdexec, Boost
examples/   独立可编译的示例代码
```

在第三方 CMake 项目中直接引入：

```cmake
find_package(HeliosView REQUIRED) # 指定 -DCMAKE_PREFIX_PATH=<SDK解压路径>
target_link_libraries(my_desktop_app PRIVATE HeliosView::Core)
```

---

## 核心教程与架构指南

### 1. App 与消息循环调度

`helios::App` 统管进程主 UI 线程、操作系统事件循环派发与异步任务投递：

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main() {
    helios::App app;

    // 向 UI 线程投递异步执行任务
    app.postTask([] {
        std::println("UI 线程正在执行投递的任务！");
    });

    // 运行操作系统消息循环（所有窗口关闭或调用 app.quit() 时退出）
    return app.exec();
}
```

`App` 同时也实现了 P2300 `std::execution::scheduler` 调度器接口：
```cpp
auto ui_sender = std::execution::schedule(app.get_scheduler())
               | std::execution::then([] { std::println("已调度在 UI 循环执行"); });
```

---

### 2. 信号与槽（Signals & Slots）

框架提供现代轻量级 C++23 信号槽组件，原生支持 Lambda 表达式、成员函数指针与异步 Sender：

```cpp
helios::Signal<int, int> onWindowResized;

// Lambda 槽函数
auto slotId = onWindowResized.connect([](int w, int h) {
    std::println("窗口尺寸变更: {}x{}", w, h);
});

// 成员函数槽
onWindowResized.connect(&MyController::handleResize, this);

// 触发信号
onWindowResized(1280, 720);

// 断开连接
onWindowResized.disconnect(slotId);
```

---

### 3. 原生现代窗口外壳

`helios::Window` 封装了 Win32 顶层窗口，原生支持 Windows 11 现代视效：

```cpp
#include <HeliosViewCore/HeliosView.h>

int main() {
    helios::App app;

    // 创建无边框窗口
    helios::Window window(1024, 640, "HeliosView 外壳", helios::WindowStyle::Frameless);
    
    // 启用 Win11 Acrylic 亚克力或 Mica 云母材质
    window.setBackdrop(helios::BackdropStyle::Acrylic);
    window.setDarkMode(true);

    // 注册自定义标题栏拖拽区域（顶部 40px）
    window.addDragRegion(0, 0, 1024, 40);

    // 监听窗口生命周期事件
    window.resized.connect([](int w, int h) { /* 处理尺寸调整 */ });
    window.closeRequested.connect(&helios::Window::close, &window);

    window.show();
    return app.exec();
}
```

能力支持：
- **外观风格**：`WindowStyle::Normal`（普通带边框）、`Borderless`（无边框）、`Frameless`（完全自定义客户区无原生标题栏）。
- **DWM 材质**：`BackdropStyle::None`、`Mica`、`Acrylic`、`Tabbed`。
- **高 DPI 适配**：Per-Monitor DPI aware v2（`enableDpiAwareness()`、`window.dpi()`）。
- **任务栏进度**：`setProgress(state, value)`（正常、不确定动画、错误红条、暂停黄条）。

---

### 4. 现代 WebView2 与 RPC 桥

`helios::WebViewWindow` 嵌入 Chromium WebView2，并提供基于 **`bindJson`** 的自动类型推导双向 RPC：

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <boost/describe.hpp>

// 1. 定义强类型 DTO 结构体
struct CalculateRequest {
    int a;
    int b;
    std::string operation;
};
BOOST_DESCRIBE_STRUCT(CalculateRequest, (), (a, b, operation))

int main() {
    helios::App app;
    auto window = std::make_shared<helios::WebViewWindow>(1000, 700, "WebView RPC 演示");
    window->show();
    window->createWebView();

    // 2. 绑定 C++ 协程 RPC 处理器（入参类型自动推导，无需手动反序列化！）
    window->bindJson("calculate", [](CalculateRequest req) -> std::execution::task<int> {
        if (req.operation == "add") co_return req.a + req.b;
        if (req.operation == "mul") co_return req.a * req.b;
        co_return 0;
    });

    // 3. 前端交互
    window->navigateHtml(R"html(
        <!DOCTYPE html>
        <html>
        <body>
            <button onclick="run()">点击调用原生 C++</button>
            <script>
                async function run() {
                    const res = await window.helios.call('calculate', {
                        a: 21, b: 2, operation: 'mul'
                    });
                    alert('原生执行结果: ' + res); // 42
                }
            </script>
        </body>
        </html>
    )html");

    return app.exec();
}
```

特性支持：
- **参数类型自动推导**：自动将前端传入的 JSON 数据反序列化为 Boost.Describe 标注的结构体。
- **双向广播机制**：`window->broadcast("event_name", payload)` 原生向 Web 广播，以及 `window->subscribeJson(...)` 监听 Web 消息。
- **免端口虚拟本地资源目录**：通过 `mapLocalFolder("assets", "D:/app/dist")`，前端可直接通过 `https://assets/...` 访问本地静态页面与静态资源，无本地端口占用或跨域困扰。

---

### 5. 2D 矢量画布与直接绘制

HeliosView 核心直接集成了 **Blend2D** 矢量渲染引擎，提供毫秒级高性能纯 CPU/JIT 2D 矢量图形渲染：

```cpp
#include <HeliosViewCore/Canvas.h>

// 创建 800x600 的 32 位 RGBA 离屏画布
helios::Canvas canvas(800, 600);
helios::Painter painter(canvas);

// 清空背景色
painter.clear(helios::Rgba32(24, 26, 32));

// 绘制抗锯齿圆角矩形与带边框的圆形
painter.fillRoundRect(50, 50, 200, 100, 16, 16, helios::Rgba32(64, 128, 255));
painter.strokeCircle(400, 300, 80, helios::Rgba32(255, 180, 0), 4.0);

// 绘制复杂贝塞尔矢量轮廓
helios::Path path;
path.moveTo(300, 100);
path.cubicTo(350, 50, 450, 50, 500, 100);
path.lineTo(400, 200);
path.close();
painter.fillPath(path, helios::Rgba32(46, 204, 113));

// 直接编码保存为本地 PNG/JPEG 图片，或快速直刷到窗口呈现
canvas.writeToFile("render.png");
```

---

### 6. 保留模式 UI 组件体系与布局

`HeliosViewCore/UI/Widget.h` 提供了基于 2D 画布的保留模式组件系统：

```cpp
#include <HeliosViewCore/UI/Widget.h>

// 1. 创建自动布局容器
auto root = std::make_shared<helios::ui::VStack>(20 /*内边距*/, 12 /*间距*/);
root->setBounds(0, 0, 400, 600);

// 2. 添加内置交互控件
auto title = std::make_shared<helios::ui::Label>("参数设置面板", 20.0f, helios::Rgba32(240, 240, 245), true);
root->addChild(title);

auto slider = std::make_shared<helios::ui::Slider>(0.0f, 100.0f, 45.0f, 260, 24);
slider->onValueChanged = [](float val) {
    std::println("滑块数值: {:.1f}", val);
};
root->addChild(slider);

auto toggle = std::make_shared<helios::ui::Switch>(true);
toggle->onToggled = [](bool checked) {
    std::println("开关状态: {}", checked);
};
root->addChild(toggle);

auto button = std::make_shared<helios::ui::Button>("应用配置", 140, 36, helios::ui::ButtonStyle::Primary);
button->onClick = [] {
    std::println("按钮已点击！");
};
root->addChild(button);
```

内置组件列表：
- `Button`（Primary 主按钮 / Normal 常规样式，悬停与按下态反馈）
- `Slider`（平滑浮点范围滑动条）
- `Switch`（现代 iOS / WinUI 风格平滑切换开关）
- `Checkbox`（复选框与文字标签）
- `ProgressBar`（0.0 ~ 1.0 范围进度条）
- `SegmentedControl`（多段分段选项卡栏）
- `Card`（带边框与圆角背景的卡片容器）
- `Label`（高清晰抗锯齿文本）
- `VStack` 与 `HStack`（垂直与水平自适应布局容器）
- `CustomWidget`（支持任意自定义绘制与事件分发的通用扩展组件）

---

### 7. 子视口宿主与混合架构应用

基于 HeliosView 的**子视口宿主架构（Child Viewport Host Architecture）**，开发者可在同一个窗口中同时嵌入原生 2D 自绘画布组件与 Chromium WebView2 页面：

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>

int main() {
    helios::App app;
    helios::Window window(1280, 720, "混合桌面应用", helios::WindowStyle::Normal);

    // 左侧视口：原生 2D 自绘 UI 宿主（宽度 320）
    auto uiHost = window.createUIHost(0, 0, 320, 720);
    auto sidePanel = std::make_shared<helios::ui::VStack>(16, 12);
    sidePanel->addChild(std::make_shared<helios::ui::Button>("原生侧边栏", 200, 36));
    uiHost->setRootWidget(sidePanel);

    // 右侧视口：现代 WebView2 网页宿主（宽度 960）
    auto webHost = window.createWebViewHost(320, 0, 960, 720);
    webHost->navigate("https://github.com");

    // 监听窗口尺寸变化，动态重排两个视口
    window.resized.connect([&](int w, int h) {
        uiHost->setBounds(0, 0, 320, h);
        webHost->setBounds(320, 0, w - 320, h);
    });

    window.show();
    return app.exec();
}
```

---

### 8. 异步线程池与 HTTP 客户端（长连接复用）

HeliosView 内建多工作线程池（`helios::Async`）与高性能 HTTP 客户端（`helios::http::Client`），支持 HTTP/1.1 Keep-Alive 连接池与 SSL/TLS 加密：

```cpp
#include <HeliosViewCore/Async.h>
#include <HeliosViewCore/Http.h>

helios::Async async(4); // 4 个后台工作线程
helios::http::Client client(async);

// 在 stdexec 协程内发起异步 HTTP GET 请求
auto task = [] (helios::http::Client& cli) -> std::execution::task<void> {
    auto res = co_await cli.get("https://api.github.com/zen");
    std::println("HTTP 状态码: {}, 返回内容: {}", res.status, res.body);
};
```

---

### 9. 桌面系统能力集成

```cpp
// 1. 系统托盘图标
helios::Tray tray("我的桌面应用");
helios::Menu trayMenu;
trayMenu.addItem("显示主窗口")->triggered.connect([&] { window.showNormal(); });
trayMenu.addItem("退出程序")->triggered.connect([&] { app.quit(); });
tray.setMenu(trayMenu);

// 2. 原生文件夹选择
std::string selectedDir;
if (helios::selectFolder(window.nativeHandle(), "选择存储目录", selectedDir)) {
    std::println("目录路径: {}", selectedDir);
}

// 3. 系统原生 Toast 通知
helios::notificationShow("同步完成", "所有文件已成功上传。");
```

---

### 10. 纯 C99 ABI 与跨语言绑定

HeliosView 的所有底层能力均可通过纯 C 接口直接调用：

```c
#include <HeliosView/heliosview.h>

int main(void) {
    // 创建原生窗口
    heliosview_window_t* win = heliosview_window_create(800, 600, "纯 C 窗口");
    heliosview_window_show(win);

    // 创建托盘图标并弹出通知
    heliosview_tray_t* tray = heliosview_tray_create(win, "托盘图标", NULL, NULL);
    heliosview_tray_notify(tray, "HeliosView", "正在通过 C99 运行", HELIOSVIEW_TRAY_NOTIFY_INFO, 3000);

    // 运行主事件循环
    heliosview_run(NULL, NULL);

    heliosview_tray_destroy(tray);
    heliosview_window_destroy(win);
    return 0;
}
```

---

## 仓库目录结构

```
HeliosView/
├── include/
│   ├── HeliosView/                   # 纯 C99 ABI 头文件（动态库导出接口）
│   │   ├── heliosview.h              # C 总头文件
│   │   ├── heliosview_canvas.h       # 2D 画布与 Blend2D 纯 C 接口
│   │   ├── heliosview_host.h         # 子视口宿主接口（UIHost, WebViewHost）
│   │   ├── heliosview_ui.h           # 保留模式 UI 控件 ABI 与调度
│   │   └── ...                       # 窗口、托盘、菜单、对话框、系统、通知
│   └── HeliosViewCore/               # 纯头文件 C++23 框架封装
│       ├── HeliosView.h              # C++ 总头文件
│       ├── App.h                     # App 单例与 UI 线程调度器
│       ├── Window.h                  # 顶层窗口封装
│       ├── WebViewWindow.h           # WebView2 窗口与视口宿主（UIHost, WebViewHost）
│       ├── Canvas.h                  # C++ Canvas, Painter, Path, Matrix RAII
│       ├── BufferPresenter.h         # 像素双缓冲呈现器
│       ├── UI/
│       │   └── Widget.h              # 保留模式 UI 组件（Button, Slider, Switch 等）
│       ├── WebViewJson.h             # 自动类型推导 bindJson 与序列化
│       ├── Async.h                   # Boost.Asio 线程池与 execution 集成
│       └── Http.h                    # Beast 驱动的 Keep-Alive HTTP 客户端
├── src/
│   ├── heliosview.cpp                # C ABI 核心实现与分发
│   ├── heliosview_canvas_blend2d.cpp # Blend2D JIT 画布后端实现
│   └── win32/                        # 平台特定后端（Win32 窗口、WebView2、DWM）
├── examples/                         # 结构化模块演示程序
│   ├── 01_console_core/              # 异步核心、stdexec 与 HTTP 演示
│   ├── 02_window_shell/              # Win32 窗口壳、无边框、Mica/Acrylic、深色模式
│   ├── 03_webview_bridge/            # WebView2 毛玻璃 UI 与双向 RPC 桥
│   ├── 04_canvas_ui/                 # 2D 自绘画布与 UI 控件画廊
│   ├── 05_studio_hybrid/             # 旗舰双视口混合桌面 Studio
│   └── 06_c_api/                     # 纯 C99 ABI 接口演示
├── tests/                            # 自动化单元测试集
│   ├── canvas_test.cpp               # 无头 2D 矢量画布测试
│   └── presenter_test.cpp            # PixelView 与 BufferPresenter 渲染呈现测试
└── third_party/                      # 内置第三方依赖
    ├── blend2d/                      # Blend2D 矢量光栅化引擎
    ├── asmjit/                       # AsmJit x86/ARM JIT 汇编器
    ├── stdexec/                      # P2300 Senders/Receivers 参考实现
    └── boost/                        # Boost（Asio, Beast, Describe, JSON）
```

---

## 路线图

- [x] 高性能 Blend2D 2D 矢量自绘画布引擎。
- [x] 子视口宿主架构（`UIHost` + `WebViewHost` 混编支持）。
- [x] 保留模式 UI 组件体系（`Button`、`Slider`、`Switch`、`Card`、`VStack`、`HStack`）。
- [x] 模块化示例工程群（`01_console_core` 至 `06_c_api`）。
- [ ] 硬件加速 GPU 渲染后端接入（Direct2D / Vulkan / WebGPU）。
- [ ] 跨平台平台层实现（macOS Cocoa + WKWebView，Linux GTK4 + WebKitGTK）。
- [ ] 无障碍访问支持（UI Automation / 屏幕阅读器树节点集成）。

---

## 开源协议

HeliosView 基于 Apache License 2.0 协议开源。内置第三方子模块保持各自的宽松开源许可证（Blend2D: Zlib, asmjit: Zlib, stdexec: Apache 2.0, Boost: BSL-1.0）。
