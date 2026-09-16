# HeliosView

> **English | [简体中文](README_zh-CN.md)**

A modern, high-performance C++23 desktop GUI framework combining **native Win32 windowing**, **Chromium WebView2 integration**, **Blend2D-powered 2D vector canvas**, **retained UI components**, and **C++23 stdexec async execution**.

HeliosView is architected in two clean layers:

- **`HeliosView.dll`** — A pure **C99 ABI** (`extern "C"`, POD types, zero C++ exceptions across DLL boundaries). Covers top-level windows, child viewport hosts (`heliosview_host_t`), the WebView2 engine bridge, JIT-accelerated 2D vector canvas (`heliosview_canvas_t`), retained UI widget dispatch (`heliosview_ui.h`), system tray, menus, native file dialogs, WinRT toast notifications, taskbar progress, and DWM backdrop materials (Mica, Acrylic, Dark Mode).
- **`HeliosView.Core`** — A **header-only C++23 framework** built over the C API:
  - **Child Viewport Host Architecture**: Host native 2D canvas viewports (`UIHost`) and web viewports (`WebViewHost`) concurrently inside the same top-level window.
  - **Blend2D Vector DirectDraw**: JIT-compiled software rasterization (`Canvas`, `Painter`, `BufferPresenter`, `PixelView`) with subpixel antialiasing, affine transforms, gradients, paths, and text.
  - **Retained UI Component Tree**: Composable widget hierarchy (`Widget`, `Button`, `Slider`, `Switch`, `Checkbox`, `ProgressBar`, `SegmentedControl`, `Card`, `VStack`, `HStack`, `CustomWidget`) with layout and event propagation.
  - **Bidirectional WebView2 RPC Bridge**: Automatic type-deducing JSON binding (`bindJson`), typed DTOs via Boost.Describe, `broadcast`/`subscribe` message passing, and custom URI scheme mappings.
  - **Modern Async & Execution Engine**: `stdexec` (P2300 senders/receivers) coroutine integration (`std::execution::task`), Boost.Asio thread pool (`Async`), and Boost.Beast HTTP/1.1 client with keep-alive connection pooling.

Include one header and link one CMake target:

```cpp
#include <HeliosViewCore/HeliosView.h>
```

```cmake
target_link_libraries(my_app PRIVATE HeliosView::Core)
```

---

## Key Pillars

```
+-----------------------------------------------------------------------------------+
|                                  HeliosView.Core                                  |
|                                (Header-only C++23)                                |
+------------------------+------------------------+---------------------------------+
|   Modern Native Shell  |  Child Viewport Hosts  |    Async & Execution Core       |
|  - Window, Frameless   |  - UIHost (DirectDraw) |  - stdexec (P2300 Senders)      |
|  - Mica, Acrylic, Dark |  - WebViewHost         |  - Boost.Asio ThreadPool        |
|  - Tray, Menus, Dialogs|  - Dual-Viewport Studio|  - Boost.Beast Keep-Alive HTTP  |
+------------------------+------------------------+---------------------------------+
|   WebView2 RPC Bridge  |  2D Canvas DirectDraw  |     Retained UI Framework       |
|  - bindJson (Deduce)   |  - Blend2D JIT Engine  |  - Widget Hierarchy, Containers |
|  - Typed DTOs, Senders |  - Painter, Paths, Text|  - Button, Slider, Switch, etc. |
|  - broadcast/subscribe |  - BufferPresenter GDI |  - Layout & Event Propagation   |
+------------------------+------------------------+---------------------------------+
|                                   HeliosView.dll                                  |
|                         (Stable C99 ABI & Platform Porting)                       |
+-----------------------------------------------------------------------------------+
```

---

## Threading Model

**All UI operations (windows, viewport hosts, webviews, canvas rendering, widgets, tray, menus, dialogs, and event loops) must execute on the message-loop thread** — the thread executing `App::exec()` (or in C, `heliosview_run`). Invoking UI APIs from worker threads is undefined behavior.

**Safe from any thread:**
- `App::postTask(fn)` — Enqueue work onto the UI thread event queue (thread-safe wake-up).
- `app.quit()` — Request message loop termination.
- WebView `resolve`, `reject`, and `broadcast`.
- OS Toasts (`notificationShow` / `heliosview_notification_show`).
- Background worker tasks via `helios::Async` thread pool.
- `heliosview_free`.

```cpp
helios::App app;

// Safely dispatching from a background worker thread back to the UI:
std::thread worker([app] {
    do_heavy_computation();
    app->postTask([] {
        // Executed safely on the UI message-loop thread
        update_window_ui();
    });
});
worker.detach();

return app.exec();
```

---

## Feature Overview

| Area | C API | C++ Core API | Key Capabilities |
| --- | --- | --- | --- |
| **Window Shell** | `heliosview_window_*` | `helios::Window` | Normal, Borderless, Frameless; Mica/Acrylic backdrop; DWM dark mode; titlebar dragging; DPI awareness; geometry, min/max limits, modal lock, taskbar progress |
| **Viewport Hosts** | `heliosview_host_*` | `helios::UIHost`, `helios::WebViewHost` | Child viewports embedded in host windows; side-by-side hybrid studio layouts; dynamic resizing and visibility |
| **2D Canvas** | `heliosview_canvas_*` | `helios::Canvas`, `Painter`, `BufferPresenter` | Blend2D JIT x86_64 rasterizer; paths, transforms, gradients, text, clipping; high-performance software blitting via GDI DIB |
| **Retained UI** | `heliosview_ui_*` | `helios::ui::Widget`, `VStack`, `HStack` | Built-in `Button`, `Slider`, `Switch`, `Checkbox`, `ProgressBar`, `SegmentedControl`, `Card`, `CustomWidget`; layout & hit testing |
| **WebView2** | `heliosview_webview_*` | `helios::WebViewWindow`, `WebViewHost` | Modern Chromium engine; full DOM integration; virtual scheme mapping (`localUrl`); DevTools, zoom, low-footprint mode |
| **RPC Bridge** | `heliosview_webview_bind` | `bindJson`, `subscribeJson`, `broadcast` | Automatic parameter type deduction via Boost.Describe; asynchronous `std::execution::task` handlers; bi-directional messaging |
| **Async & Coroutines** | `heliosview_run` | `helios::Async`, `std::execution` | Boost.Asio thread pool; P2300 Senders & Receivers; C++23 coroutines (`co_await`, `co_return`) |
| **HTTP Client** | — | `helios::http::Client` | Boost.Beast keep-alive connection pool; automatic retries for idempotent requests; TLS/SSL verification |
| **Tray & Menus** | `heliosview_tray_*`, `heliosview_menu_*` | `helios::Tray`, `Menu`, `MenuBar`, `Action` | Notification area icon & balloon; context/popup menus; application menu bar; standard platform roles & accelerators |
| **Dialogs & OS** | `heliosview_dialog_*`, `heliosview_system_*` | `Dialogs.h`, `System.h` | File open/save (single & multi-filter); folder picker; message box; clipboard; open URL; reveal in Explorer; global hotkeys |
| **Notifications** | `heliosview_notification_*` | `Notification.h` | WinRT toast notifications; click callbacks; permission querying; thread-agnostic |

---

## Memory Allocation

HeliosView routes all internal dynamic memory allocations through a single configurable allocator. This guarantees safe allocation across DLL boundaries and allows embedding in custom memory pools or game engine arenas:

- **C**: Call `heliosview_set_allocator(&allocator)` **before creating any object**. Strings returned by the library (file dialog paths, clipboard buffers) must always be freed with **`heliosview_free`**.
- **C++**: The C++ wrapper automatically uses the underlying allocator; strings returned to C++ code are standard UTF-8 `std::string` instances.

```c
char* selected_path = NULL;
if (heliosview_select_folder(NULL, "Choose Directory", &selected_path) == 1) {
    printf("Selected: %s\n", selected_path);
    heliosview_free(selected_path); // Always release library strings with heliosview_free
}
```

---

## Building & Dependencies

Requires **CMake ≥ 4.3** and a **C++23 conforming compiler** (MSVC 19.38+ / Visual Studio 2022+).

All third-party dependencies are vendored as submodules or auto-fetched — **no vcpkg or system-wide package managers required**:

| Dependency | Version | Source | Purpose |
| --- | --- | --- | --- |
| **WebView2 SDK** | 1.0.4129.50 | NuGet (auto-downloaded at configure time) | Win32 Chromium WebView2 runtime loader |
| **Blend2D** | v0.21.3 | Git submodule (`third_party/blend2d`) | Built-in JIT 2D vector rasterization engine |
| **asmjit** | pinned `dffd8b1` | Git submodule (`third_party/asmjit`) | JIT assembler backend for Blend2D |
| **stdexec** | pinned `758f41f4` | Git submodule (`third_party/stdexec`) | P2300 Senders/Receivers & C++23 execution |
| **Boost** | 1.92.0 | Git submodule (`third_party/boost`) | Asio (thread pool), Beast (HTTP), JSON (RPC auto-binding) |

### Clone & Build

```sh
# Clone repository with submodules
git clone --recurse-submodules https://github.com/CoplenSasbian/HeliosView.git
cd HeliosView

# Configure build with Ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Compile DLL, static libs, demos, and tests
cmake --build build
```

Compiled binaries, DLLs, demos, and tests are placed together in `build/bin/` — ready to run with no `PATH` configuration.

---

## Showcase Demos & Tests

The project includes 6 modular showcase applications under `examples/` and a unit test suite under `tests/`:

| Target | Source Path | Description |
| --- | --- | --- |
| **`HeliosView_ConsoleCoreDemo`** | `examples/01_console_core/` | Headless console application demonstrating `helios::Async` thread pool, `stdexec` coroutine pipelines, and Asio HTTP Keep-Alive client/server. |
| **`HeliosView_WindowShellDemo`** | `examples/02_window_shell/` | Modern native Win32 window shell: frameless windowing, Acrylic/Mica blur, DWM dark mode, native menus, tray icons, toasts, and dialogs. |
| **`HeliosView_WebViewBridgeDemo`** | `examples/03_webview_bridge/` | Full-fledged WebView2 integration: glassmorphic HTML5 dashboard, `bindJson` typed DTO RPC, `broadcast`/`subscribe`, and custom virtual URI schemes. |
| **`HeliosView_CanvasUiDemo`** | `examples/04_canvas_ui/` | High-performance 2D DirectDraw & Retained UI gallery: 4 tabs with Form Controls (Button, Slider, Switch, Progress), Area Charts, Vector Path Gears, and 60FPS CRT Oscilloscope. |
| **`HeliosView_StudioHybridDemo`** | `examples/05_studio_hybrid/` | Flagship hybrid desktop studio: embeds a native top toolbar, a left DirectDraw Canvas UI viewport (`UIHost`), and a right WebView2 browser (`WebViewHost`) inside one window. |
| **`HeliosView_CApiDemo`** | `examples/06_c_api/` | Pure C99 ABI tour: windowing, events, tray icon, context menus, and native dialogs built without a C++ compiler. |
| **`HeliosView_CanvasTest`** | `tests/canvas_test.cpp` | Headless unit test verifying Blend2D vector drawing, paths, matrices, and image rasterization. |
| **`HeliosView_PresenterTest`** | `tests/presenter_test.cpp` | Unit test validating `PixelView` and `BufferPresenter` pixel blitting pipeline. |

Run any demo directly:

```sh
./build/bin/HeliosView_StudioHybridDemo.exe
```

---

## Releases — SDK Package

HeliosView publishes self-contained SDK packages for Windows x64 (`HeliosView-<version>-win64-SDK.zip`):

```
bin/        HeliosView.dll + WebView2Loader.dll + precompiled showcase demos
lib/        HeliosView.lib + libboost_json.lib + CMake package configs
include/    C API (HeliosView/), C++ Core (HeliosViewCore/), stdexec, Boost
examples/   Standalone demo source code
```

To consume the prebuilt SDK in an external project:

```cmake
find_package(HeliosView REQUIRED) # Pass -DCMAKE_PREFIX_PATH=<path-to-sdk>
target_link_libraries(my_desktop_app PRIVATE HeliosView::Core)
```

---

## Tutorial & Architecture Guide

### 1. App & Message Loop

`helios::App` manages the process UI thread, the event message pump, and asynchronous task scheduling:

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <print>

int main() {
    helios::App app;

    // Post an asynchronous task to run on the UI thread
    app.postTask([] {
        std::println("Hello from the UI thread!");
    });

    // Run the OS message loop (terminates when all windows close or app.quit() is called)
    return app.exec();
}
```

`App` also acts as a P2300 scheduler:
```cpp
auto ui_sender = std::execution::schedule(app.get_scheduler())
               | std::execution::then([] { std::println("Scheduled on UI loop"); });
```

---

### 2. Signals & Slots

HeliosView features a lightweight, high-performance C++23 signal/slot mechanism supporting synchronous callables, class member functions, and asynchronous senders:

```cpp
helios::Signal<int, int> onWindowResized;

// Lambda slot
auto slotId = onWindowResized.connect([](int w, int h) {
    std::println("Window resized: {}x{}", w, h);
});

// Member function slot
onWindowResized.connect(&MyController::handleResize, this);

// Fire signal
onWindowResized(1280, 720);

// Disconnect
onWindowResized.disconnect(slotId);
```

---

### 3. Native Window Shell

`helios::Window` provides full control over top-level desktop windows with modern Windows 11 capabilities:

```cpp
#include <HeliosViewCore/HeliosView.h>

int main() {
    helios::App app;

    // Create a frameless window with custom size and title
    helios::Window window(1024, 640, "HeliosView Shell", helios::WindowStyle::Frameless);
    
    // Windows 11 Acrylic or Mica backdrop styling
    window.setBackdrop(helios::BackdropStyle::Acrylic);
    window.setDarkMode(true);

    // Register a titlebar drag area (top 40px)
    window.addDragRegion(0, 0, 1024, 40);

    // Hook window lifecycle signals
    window.resized.connect([](int w, int h) { /* handle resize */ });
    window.closeRequested.connect(&helios::Window::close, &window);

    window.show();
    return app.exec();
}
```

Capabilities:
- **Styles**: `WindowStyle::Normal`, `Borderless`, `Frameless`.
- **DWM Backdrops**: `BackdropStyle::None`, `Mica`, `Acrylic`, `Tabbed`.
- **DPI Scaling**: Per-monitor DPI awareness v2 (`enableDpiAwareness()`, `window.dpi()`).
- **Taskbar Progress**: `setProgress(state, value)` (Normal, Indeterminate, Error, Paused).

---

### 4. Modern WebView2 & RPC Bridge

`helios::Window` natively embeds Chromium WebView2 and provides type-deduced C++ ↔ JavaScript RPC via **`bindJson`** (with `helios::WebViewWindow` available as a backwards-compatible alias):

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <boost/describe.hpp>

// 1. Define typed DTO
struct CalculateRequest {
    int a;
    int b;
    std::string operation;
};
BOOST_DESCRIBE_STRUCT(CalculateRequest, (), (a, b, operation))

int main() {
    helios::App app;
    auto window = std::make_shared<helios::Window>(1000, 700, "WebView Bridge");
    window->show();
    window->createWebView();

    // 2. Bind C++ coroutine RPC handler (parameter types automatically deduced!)
    window->bindJson("calculate", [](CalculateRequest req) -> std::execution::task<int> {
        if (req.operation == "add") co_return req.a + req.b;
        if (req.operation == "mul") co_return req.a * req.b;
        co_return 0;
    });

    // 3. Drive frontend HTML / JS
    window->navigateHtml(R"html(
        <!DOCTYPE html>
        <html>
        <body>
            <button onclick="run()">Compute</button>
            <script>
                async function run() {
                    const res = await window.helios.call('calculate', {
                        a: 21, b: 2, operation: 'mul'
                    });
                    alert('Result: ' + res); // 42
                }
            </script>
        </body>
        </html>
    )html");

    return app.exec();
}
```

Features:
- **Parameter Type Deduction**: Automatically deserializes incoming JSON payloads into Boost.Describe structures.
- **Bi-directional Broadcast**: `window->broadcast("event_name", payload)` and `window->subscribeJson(...)`.
- **Virtual Local Asset Server**: `mapLocalFolder("assets", "D:/app/dist")` allows loading web assets over `https://assets/...` with no local HTTP port conflicts.

---

### 5. 2D Vector Canvas & DirectDraw

HeliosView integrates **Blend2D** directly into the core, offering high-speed software 2D vector drawing with subpixel quality:

```cpp
#include <HeliosViewCore/Canvas.h>

// Create a 800x600 32-bit RGBA off-screen canvas
helios::Canvas canvas(800, 600);
helios::Painter painter(canvas);

// Clear background
painter.clear(helios::Rgba32(24, 26, 32));

// Anti-aliased vector shapes
painter.fillRoundRect(50, 50, 200, 100, 16, 16, helios::Rgba32(64, 128, 255));
painter.strokeCircle(400, 300, 80, helios::Rgba32(255, 180, 0), 4.0);

// Complex vector paths
helios::Path path;
path.moveTo(300, 100);
path.cubicTo(350, 50, 450, 50, 500, 100);
path.lineTo(400, 200);
path.close();
painter.fillPath(path, helios::Rgba32(46, 204, 113));

// Save directly to PNG/JPEG or blit to screen
canvas.writeToFile("render.png");
```

---

### 6. Retained UI Components & Layout

`HeliosViewCore/UI/Widget.h` provides an extensible retained-mode UI component system rendered directly via `Canvas`:

```cpp
#include <HeliosViewCore/UI/Widget.h>

// 1. Create layout containers
auto root = std::make_shared<helios::ui::VStack>(20 /*padding*/, 12 /*spacing*/);
root->setBounds(0, 0, 400, 600);

// 2. Add built-in interactive controls
auto title = std::make_shared<helios::ui::Label>("Settings Panel", 20.0f, helios::Rgba32(240, 240, 245), true);
root->addChild(title);

auto slider = std::make_shared<helios::ui::Slider>(0.0f, 100.0f, 45.0f, 260, 24);
slider->onValueChanged = [](float val) {
    std::println("Slider changed: {:.1f}", val);
};
root->addChild(slider);

auto toggle = std::make_shared<helios::ui::Switch>(true);
toggle->onToggled = [](bool checked) {
    std::println("Switch toggled: {}", checked);
};
root->addChild(toggle);

auto button = std::make_shared<helios::ui::Button>("Apply Settings", 140, 36, helios::ui::ButtonStyle::Primary);
button->onClick = [] {
    std::println("Action triggered!");
};
root->addChild(button);
```

Available built-in components:
- `Button` (Primary & Normal styles, hover/pressed states)
- `Slider` (Continuous floating-point value dragging)
- `Switch` (Smooth toggle switch)
- `Checkbox` (Selectable checkmark with text label)
- `ProgressBar` (Normalized progress indicator)
- `SegmentedControl` (Tab-style multiple choice selection)
- `Card` (Rounded container with borders and backgrounds)
- `Label` (Anti-aliased typography)
- `VStack` & `HStack` (Vertical and horizontal auto-layout containers)
- `CustomWidget` (Delegate custom painting and mouse interaction)

---

### 7. Viewport Hosts & Hybrid Studio

With HeliosView's **Child Viewport Host Architecture**, developers can host native DirectDraw canvas components and Chromium WebView2 instances simultaneously in the same window:

```cpp
#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>

int main() {
    helios::App app;
    helios::Window window(1280, 720, "Hybrid Studio", helios::WindowStyle::Normal);

    // Left Viewport: Native Retained 2D Canvas Host (Width: 320)
    auto uiHost = window.createUIHost(0, 0, 320, 720);
    auto sidePanel = std::make_shared<helios::ui::VStack>(16, 12);
    sidePanel->addChild(std::make_shared<helios::ui::Button>("Tools", 200, 36));
    uiHost->setRootWidget(sidePanel);

    // Right Viewport: Modern Chromium WebView Host (Width: 960)
    auto webHost = window.createWebViewHost(320, 0, 960, 720);
    webHost->navigate("https://github.com");

    // Dynamic split resizing
    window.resized.connect([&](int w, int h) {
        uiHost->setBounds(0, 0, 320, h);
        webHost->setBounds(320, 0, w - 320, h);
    });

    window.show();
    return app.exec();
}
```

---

### 8. Async & HTTP Client (Keep-Alive Pooling)

HeliosView incorporates a multi-threaded worker pool (`helios::Async`) and an HTTP client (`helios::http::Client`) supporting HTTP/1.1 Keep-Alive connection pooling and SSL/TLS:

```cpp
#include <HeliosViewCore/Async.h>
#include <HeliosViewCore/Http.h>

helios::Async async(4); // 4 worker threads
helios::http::Client client(async);

// Asynchronous GET request within a stdexec coroutine
auto task = [] (helios::http::Client& cli) -> std::execution::task<void> {
    auto res = co_await cli.get("https://api.github.com/zen");
    std::println("HTTP Status: {}, Body: {}", res.status, res.body);
};
```

Features:
- Thread pool integrated with `stdexec` P2300 schedulers.
- Persistent TCP connection reuse per origin.
- DNS resolution caching.
- Safe automatic retry on idle socket disconnection.

---

### 9. System Integrations (Tray, Menus, Dialogs)

```cpp
// 1. Notification Area (Tray)
helios::Tray tray("My App");
helios::Menu trayMenu;
trayMenu.addItem("Open")->triggered.connect([&] { window.showNormal(); });
trayMenu.addItem("Exit")->triggered.connect([&] { app.quit(); });
tray.setMenu(trayMenu);

// 2. Native File Dialogs
std::string selectedFolder;
if (helios::selectFolder(window.nativeHandle(), "Choose Folder", selectedFolder)) {
    std::println("Folder: {}", selectedFolder);
}

// 3. System Toasts
helios::notificationShow("Backup Complete", "All files synchronized.");
```

---

### 10. The Pure C99 ABI

Everything in HeliosView is accessible via standard C functions:

```c
#include <HeliosView/heliosview.h>

int main(void) {
    // Initialize window
    heliosview_window_t* win = heliosview_window_create(800, 600, "C Window");
    heliosview_window_show(win);

    // Create system tray
    heliosview_tray_t* tray = heliosview_tray_create(win, "Tray Icon", NULL, NULL);
    heliosview_tray_notify(tray, "HeliosView", "Running from C99", HELIOSVIEW_TRAY_NOTIFY_INFO, 3000);

    // Start event loop
    heliosview_run(NULL, NULL);

    heliosview_tray_destroy(tray);
    heliosview_window_destroy(win);
    return 0;
}
```

---

## Repository Layout

```
HeliosView/
├── include/
│   ├── HeliosView/                   # Pure C99 ABI Headers (Stable dynamic interface)
│   │   ├── heliosview.h              # Master C umbrella header
│   │   ├── heliosview_canvas.h       # 2D Canvas & Blend2D C functions
│   │   ├── heliosview_host.h         # Child viewport host APIs (UIHost, WebViewHost)
│   │   ├── heliosview_ui.h           # Retained UI widget ABI & dispatch
│   │   └── ...                       # Window, Tray, Menu, Dialogs, System, Toasts
│   └── HeliosViewCore/               # Header-only C++23 Framework
│       ├── HeliosView.h              # Master C++ umbrella header
│       ├── App.h                     # App instance & UI thread scheduler
│       ├── Window.h                  # Top-level window wrapper
│       ├── WebViewWindow.h           # WebView2 window & Viewport Hosts (UIHost, WebViewHost)
│       ├── Canvas.h                  # C++ Canvas, Painter, Path, Matrix RAII
│       ├── BufferPresenter.h         # Pixel buffer presentation & blitting
│       ├── UI/
│       │   └── Widget.h              # Retained UI widgets (Button, Slider, Switch, etc.)
│       ├── WebViewJson.h             # Type-deduced bindJson & RPC serialization
│       ├── Async.h                   # Boost.Asio thread pool & execution integration
│       └── Http.h                    # Beast-powered HTTP Keep-Alive client
├── src/
│   ├── heliosview.cpp                # Core C ABI implementation & dispatch
│   ├── heliosview_canvas_blend2d.cpp # Blend2D JIT canvas backend
│   └── win32/                        # Platform-specific Win32 / WebView2 / DWM backend
├── examples/                         # Structured Showcase Demos
│   ├── 01_console_core/              # Core async, stdexec, and HTTP
│   ├── 02_window_shell/              # Win32 shell, frameless, Mica/Acrylic, dark mode
│   ├── 03_webview_bridge/            # WebView2 glassmorphism UI & RPC bridge
│   ├── 04_canvas_ui/                 # 2D DirectDraw Canvas & Widget gallery
│   ├── 05_studio_hybrid/             # Flagship dual-viewport hybrid desktop studio
│   └── 06_c_api/                     # Pure C99 ABI tour
├── tests/                            # Unit Test Suite
│   ├── canvas_test.cpp               # Headless 2D canvas & vector test
│   └── presenter_test.cpp            # PixelView & BufferPresenter test
└── third_party/                      # Vendored Dependencies
    ├── blend2d/                      # Blend2D 2D Vector Rasterizer
    ├── asmjit/                       # AsmJit x86/ARM JIT backend
    ├── stdexec/                      # P2300 Senders/Receivers reference implementation
    └── boost/                        # Boost (Asio, Beast, Describe, JSON)
```

---

## Roadmap

- [x] High-performance Blend2D 2D vector canvas engine.
- [x] Child Viewport Host architecture (`UIHost` + `WebViewHost`).
- [x] Retained UI component hierarchy (`Button`, `Slider`, `Switch`, `Card`, `VStack`, `HStack`).
- [x] Modular showcase suite (`01_console_core` through `06_c_api`).
- [ ] Hardware-accelerated GPU backends (Direct2D / Vulkan / WebGPU).
- [ ] Cross-platform backend implementations (macOS Cocoa + WKWebView, Linux GTK4 + WebKitGTK).
- [ ] Accessible UI node tree (UI Automation / Screen Reader integration).

---

## License

HeliosView is licensed under the Apache License 2.0. Third-party submodules retain their respective permissive open-source licenses (Blend2D: Zlib, asmjit: Zlib, stdexec: Apache 2.0, Boost: BSL-1.0).
