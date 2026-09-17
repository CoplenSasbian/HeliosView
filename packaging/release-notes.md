# HeliosView v1.1.0

Canvas 2D Graphics Engine & Retained UI Release — introducing multi-engine 2D drawing surfaces, zero-copy `PixelView` & `BufferPresenter`, a lightweight retained UI system, and categorized examples.

## What's new

**Canvas 2D Graphics Engine (`HeliosViewCore/Canvas.h`)**
- High-performance, engine-independent 2D canvas API with zero window dependency: pure memory drawing surface, transforms, paths, clipping, antialiased primitives, text measurement, and image encoding/decoding (PNG/JPEG/BMP/TGA).
- Pluggable multi-backend architecture:
  - **Blend2D** (`HELIOSVIEW_ENGINE_BUILTIN`): JIT-accelerated cross-platform renderer.
  - **GDI+** (`HELIOSVIEW_ENGINE_NATIVE`): Windows native fallback with 256-color grayscale palette support and format wrapping.
  - **Direct2D** (`HELIOSVIEW_ENGINE_ACCELERATED`): Hardware-accelerated GPU renderer with multi-threaded factory support.
- Full cross-engine drawing: seamless drawing from any engine's canvas into any other engine's canvas with automatic pixel format conversion.

**PixelView & BufferPresenter (`HeliosViewCore/PixelView.h`, `BufferPresenter.h`)**
- `PixelView`: Zero-copy, lightweight view representing contiguous or strided 2D pixel buffers with subview slicing.
- `BufferPresenter`: Window presentation middleware providing flicker-free double-buffered bit-blit, `WM_ERASEBKGND` interception, automatic repaint caching, and Win32 `AlphaBlend` compositing for premultiplied alpha buffers (`BGRA8_PREMUL`).

**Retained UI Component System (`HeliosView/heliosview_ui.h`)**
- Lightweight, canvas-driven UI tree mounted into window child viewports (`heliosview_host`).
- Pure memory widgets: labels, buttons, vertical/horizontal layout stacks, custom widget descriptors, and damage-based dirty repainting.

**Examples Reorganization**
- Restructured into 6 dedicated subprojects:
  - `01_console_core`: Thread pool, asynchronous I/O, timers, JSON RPC, HTTP client.
  - `02_window_shell`: Top-level windowing, frameless styles, menus, tray icons.
  - `03_webview_bridge`: Embedded WebView2, bidirectional RPC bridge.
  - `04_canvas_ui`: Retained UI controls, reactive counters, layout stacks.
  - `05_studio_hybrid`: Dual viewport showcase (retained UI toolbar + live WebView2).
  - `06_c_api`: Pure C99 ABI usage.

**Breaking Changes**
- `Paint` -> `Canvas`: Renamed `heliosview_paint_*` to `heliosview_canvas_*`, `helios::Paint` to `helios::Canvas`, and unified headers under `HeliosViewCore/Canvas.h`.

---

# HeliosView v1.1.0（中文）

Canvas 2D 图形引擎与保留式 UI 版本 —— 引入多后端 2D 绘图引擎、零拷贝 `PixelView` 与 `BufferPresenter`、轻量级保留式 UI 系统，以及模块化示例重构。

## 新增内容

**Canvas 2D 图形引擎（`HeliosViewCore/Canvas.h`）**
- 高性能、跨引擎统一的 2D 画布接口，完全脱离窗口依赖：纯内存绘图表面、矩阵变换、矢量路径、区域裁剪、抗锯齿图元、文本测量及图像编解码（PNG/JPEG/BMP/TGA）。
- 可插拔多后端引擎架构：
  - **Blend2D**（`HELIOSVIEW_ENGINE_BUILTIN`）：JIT 编译加速的跨平台高性能渲染器。
  - **GDI+**（`HELIOSVIEW_ENGINE_NATIVE`）：Windows 原生渲染后端，支持 256 色灰度调色板与自动像素格式封装。
  - **Direct2D**（`HELIOSVIEW_ENGINE_ACCELERATED`）：硬件 GPU 加速渲染器，采用多线程工厂保证线程安全。
- 完善的跨引擎绘制支持：任意引擎的画布均可直接作为源图像绘制到另一引擎画布，底层自动执行像素格式转换与回写。

**PixelView 与 BufferPresenter（`HeliosViewCore/PixelView.h`，`BufferPresenter.h`）**
- `PixelView`：轻量级零拷贝像素视图，支持跨行步长（stride）与子区域切片（subview）。
- `BufferPresenter`：窗口像素呈现中间件，接管窗口背景擦除与重绘消息，实现无闪烁双缓冲呈现，并通过 Win32 `AlphaBlend` 正确合成预乘 Alpha 缓冲（`BGRA8_PREMUL`）。

**保留式 UI 组件系统（`HeliosView/heliosview_ui.h`）**
- 基于 Canvas 的轻量级控件树，挂载于子视口（`heliosview_host`）。
- 纯内存控件：标签（Label）、按钮（Button）、垂直/水平布局栈（VStack/HStack）、自定义控件描述符表以及局部重绘机制。

**示例架构重构**
- 重构为 6 个独立的子目录示例工程：
  - `01_console_core`：线程池、异步 I/O、定时器、JSON RPC、HTTP 客户端。
  - `02_window_shell`：原生窗口壳、无边框样式、系统菜单、托盘图标。
  - `03_webview_bridge`：内嵌 WebView2、双向 RPC 桥接。
  - `04_canvas_ui`：保留式 UI 控件、计数器、自动布局栈。
  - `05_studio_hybrid`：双视口混合应用（保留式 UI 工具栏与 WebView2 视口并存）。
  - `06_c_api`：纯 C99 ABI 接口消费示例。

**破坏性变更（Breaking Changes）**
- `Paint` -> `Canvas` 命名重整：`heliosview_paint_*` 重命名为 `heliosview_canvas_*`，C++ 封装统一为 `helios::Canvas`，头文件收拢于 `HeliosViewCore/Canvas.h`。
