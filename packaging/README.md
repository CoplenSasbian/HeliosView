# HeliosView — Windows SDK package

**HeliosView** is a C++ **WebView** windowing library for Windows: embed a
WebView2 browser, drive it from C++ or C, and build the desktop shell around it
— windows, tray icons, menus, native dialogs, notifications, taskbar progress,
system helpers.

This zip is a complete, self-contained SDK (version in the zip name): runtime,
import library, headers, CMake package config, and the example sources.

---

## What's inside

| path | contents |
| --- | --- |
| `bin/` | `HeliosView.dll` (the C API runtime), `WebView2Loader.dll`, and the prebuilt demo executables |
| `lib/` | `HeliosView.lib` (import library) + `cmake/HeliosView/` (CMake package config for `find_package(HeliosView)`) |
| `include/` | `HeliosView/heliosview.h` (C API), `HeliosViewCore/` (header-only C++ wrapper), and the vendored `stdexec` + `nlohmann/json` headers the wrapper needs |
| `examples/` | the demo **sources** — build them standalone against this SDK (see below) |
| `README.md` | this file |

## Requirements

- **Runtime:** Windows 10 or 11, x64. A WebView2 Runtime (preinstalled on
  Windows 11; [download](https://developer.microsoft.com/microsoft-edge/webview2/)
  if missing).
- **To build:** CMake ≥ 4.3 and a C++23 compiler (MSVC 19.4x+ / VS 2022 or
  newer). The C demo needs a C99 compiler.

## Run the demos (no build needed)

Extract the zip and launch any executable in `bin\` — the DLLs sit next to the
exes, so no `PATH` setup is needed:

```
bin\HeliosViewDemo.exe          # WebView master demo (every feature)
bin\HeliosViewWindowDemo.exe    # basic window + signals + tray + menu
bin\HeliosViewAppDemo.exe       # Window subclassing / styles
bin\HeliosViewSystemDemo.exe    # dialogs, clipboard, toasts, taskbar
bin\HeliosViewWebViewDemo.exe   # WebView + bindJson auto-binding
bin\HeliosViewWebViewEventsDemo.exe
bin\HeliosViewCDemo.exe         # pure C consumer
```

## Use from C

1. Add `include\` to your include path.
2. `#include <heliosview.h>` (the umbrella header is `HeliosView/heliosview.h`).
3. Link `lib\HeliosView.lib`.
4. Copy `bin\HeliosView.dll` and `bin\WebView2Loader.dll` next to your
   executable.

```c
#include <heliosview.h>

int main(void)
{
    heliosview_window_t* win = heliosview_window_create(800, 600, "C demo");
    heliosview_window_show(win);
    heliosview_run(NULL, NULL);          /* message loop */
    heliosview_window_destroy(win);
    return 0;
}
```

All strings are UTF-8; free every library-returned string with
`heliosview_free`.

## Use from C++ (CMake, recommended)

The package exports `HeliosView::Core` (C++ wrapper, header-only) and
`HeliosView::HeliosView` (the DLL import target):

```cmake
cmake_minimum_required(VERSION 4.3)
project(MyApp CXX)

find_package(HeliosView REQUIRED)       # finds this SDK's lib/cmake/HeliosView

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE HeliosView::Core)
```

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main()
{
    helios::App app;
    helios::Window window(800, 600, "Hello");
    window.show();
    return app.exec();
}
```

Configure with the SDK root as the prefix path, then copy the two DLLs from
`bin\` next to your exe before running:

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=<path-to-extracted-sdk>
cmake --build build
```

## Build the examples against this SDK

`examples\` is its own CMake project — point it at the SDK and build:

```
cmake -S examples -B examples\build -DCMAKE_PREFIX_PATH=<path-to-extracted-sdk>
cmake --build examples\build
```

The resulting exes live in `examples\build\bin\`; copy `HeliosView.dll` and
`WebView2Loader.dll` from the SDK's `bin\` next to them to run.

## Notes

- **Threading:** all window / WebView / tray / menu / dialog / event-queue APIs
  must be called on the message-loop thread (the one running `App::exec()`, or
  in C the thread that called `heliosview_run`). Safe from any thread:
  `App::postTask`, WebView `resolve`/`reject`/`broadcast`, notifications,
  `heliosview_free`.
- **Allocator:** call `heliosview_set_allocator` before creating anything;
  allocate and free with the same allocator.
- The demos and DLL are x64 Release builds (MSVC). The C API
  (`heliosview.h`) is the stable ABI boundary; the C++ wrapper is header-only.

---

# HeliosView — Windows SDK 包

**HeliosView** 是一个面向 Windows 的 C++ **WebView** 窗口库：内嵌 WebView2、
从 C++ 或 C 驱动它，并围绕它构建桌面壳——窗口、托盘图标、菜单、原生对话框、
通知、任务栏进度、系统集成。

这个 zip 是一个完整自足的 SDK（版本号见 zip 文件名）：运行时、导入库、头文件、
CMake 包配置和示例源码。

## 目录说明

| 路径 | 内容 |
| --- | --- |
| `bin/` | `HeliosView.dll`（C API 运行时）、`WebView2Loader.dll` 和预编译的示例 exe |
| `lib/` | `HeliosView.lib`（导入库）+ `cmake/HeliosView/`（`find_package(HeliosView)` 用的 CMake 包配置） |
| `include/` | `HeliosView/heliosview.h`（C API）、`HeliosViewCore/`（纯头文件的 C++ 封装）、以及它依赖的 `stdexec` + `nlohmann/json` 头文件 |
| `examples/` | 示例**源码**——可独立针对本 SDK 构建（见下） |
| `README.md` | 本文件 |

## 环境要求

- **运行：** Windows 10 或 11（x64），需要 WebView2 Runtime（Win11 自带；缺失时从
  [微软官网](https://developer.microsoft.com/microsoft-edge/webview2/) 下载）。
- **编译：** CMake ≥ 4.3 和 C++23 编译器（MSVC 19.4x+ / VS 2022 或更新）；C 示例需要 C99 编译器。

## 直接运行示例（无需编译）

解压后直接运行 `bin\` 里的 exe 即可——DLL 与 exe 同目录，无需配置 PATH：

```
bin\HeliosViewDemo.exe          # WebView 综合演示（全部功能）
bin\HeliosViewWindowDemo.exe    # 基础窗口 + 信号槽 + 托盘 + 菜单
bin\HeliosViewAppDemo.exe       # Window 子类化 / 窗口样式
bin\HeliosViewSystemDemo.exe    # 对话框、剪贴板、通知、任务栏
bin\HeliosViewWebViewDemo.exe   # WebView + bindJson 自动绑定
bin\HeliosViewWebViewEventsDemo.exe
bin\HeliosViewCDemo.exe         # 纯 C 消费者
```

## C 用法

1. 把 `include\` 加入头文件搜索路径。
2. `#include <heliosview.h>`（伞形头文件为 `HeliosView/heliosview.h`）。
3. 链接 `lib\HeliosView.lib`。
4. 把 `bin\HeliosView.dll` 和 `bin\WebView2Loader.dll` 拷到你的 exe 旁边。

```c
#include <heliosview.h>

int main(void)
{
    heliosview_window_t* win = heliosview_window_create(800, 600, "C demo");
    heliosview_window_show(win);
    heliosview_run(NULL, NULL);          /* 消息循环 */
    heliosview_window_destroy(win);
    return 0;
}
```

所有字符串均为 UTF-8；库返回的字符串一律用 `heliosview_free` 释放。

## C++ 用法（CMake，推荐）

包导出 `HeliosView::Core`（C++ 封装，纯头文件）和 `HeliosView::HeliosView`
（DLL 导入目标）：

```cmake
cmake_minimum_required(VERSION 4.3)
project(MyApp CXX)

find_package(HeliosView REQUIRED)       # 找到本 SDK 的 lib/cmake/HeliosView

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE HeliosView::Core)
```

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main()
{
    helios::App app;
    helios::Window window(800, 600, "Hello");
    window.show();
    return app.exec();
}
```

把 SDK 根目录作为 prefix path 配置，运行前再把 `bin\` 里的两个 DLL 拷到
exe 旁边：

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=<解压后的SDK路径>
cmake --build build
```

## 针对本 SDK 构建示例

`examples\` 本身就是一个 CMake 工程——指向 SDK 即可构建：

```
cmake -S examples -B examples\build -DCMAKE_PREFIX_PATH=<解压后的SDK路径>
cmake --build examples\build
```

生成的 exe 在 `examples\build\bin\`；运行前把 SDK `bin\` 里的
`HeliosView.dll` 和 `WebView2Loader.dll` 拷过去。

## 注意事项

- **线程模型：** 所有窗口 / WebView / 托盘 / 菜单 / 对话框 / 事件队列 API 都必须在
  消息循环线程调用（即运行 `App::exec()` 的线程；C 里是调用 `heliosview_run` 的线程）。
  任意线程安全的有：`App::postTask`、WebView `resolve`/`reject`/`broadcast`、通知、`heliosview_free`。
- **分配器：** 在创建任何对象前调用 `heliosview_set_allocator`；分配与释放必须使用同一分配器。
- 示例与 DLL 为 x64 Release 构建（MSVC）。C API（`heliosview.h`）是稳定的 ABI 边界；
  C++ 封装为纯头文件。
