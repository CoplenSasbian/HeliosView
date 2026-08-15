# HeliosView v1.0.0

The first stable release of HeliosView — a C++ WebView windowing library:
a pure C API (`HeliosView.dll`) plus a header-only C++ wrapper (`HeliosView.Core`).

## Features

**Windows**
- Three styles: NORMAL / BORDERLESS / FRAMELESS (with built-in MDL2 control buttons)
- Position / size / opacity / topmost / fullscreen / min-max size / taskbar
  progress & flash / Mica backdrop / dark mode
- Window ids are the native HWND (`uintptr_t`); events dispatch safely by
  handle — no process-global state

**WebView (WebView2)**
- JS ⇄ native bridge: `window.helios.call` → Promise, nlohmann auto-binding
  (`bindJson` / `subscribeJson`)
- Bidirectional BroadcastChannel, eval / evalAsync, local folder mapping, insets
- Built-in `<helios-window-title-bar>` / `<helios-window-controls>` components
  (native app-region drag; maximize button disables when the window is not resizable)

**System**
- Tray, popup menus, message box, file/folder dialogs, clipboard, open URL,
  toast notifications
- Common-Controls v6 activated from the library itself: themed message boxes
  and dialogs for every consumer — no exe manifest required

**Examples**
- `HeliosViewDemo`: WebView master demo — every feature driven by sliders and
  text inputs
- Window / App / System / WebView / Events / C demos

**Requirements**: Windows 10/11, C++23 (the C API is usable from C99),
CMake ≥ 4.3, no vcpkg.

---

# HeliosView v1.0.0（中文）

HeliosView 首个稳定版本 —— 一个 C++ WebView 窗口库：纯 C API（`HeliosView.dll`）+ 头文件 C++ 包装层（`HeliosView.Core`）。

## 功能

**窗口**
- 三种样式：NORMAL / BORDERLESS / FRAMELESS（含内置 MDL2 控制按钮组件）
- 位置 / 尺寸 / 透明度 / 置顶 / 全屏 / 最小最大尺寸 / 任务栏进度与闪烁 / Mica 背景 / 暗色模式
- 窗口 id 即原生 HWND（`uintptr_t`），事件按句柄安全分发 —— 无进程级全局状态

**WebView（WebView2）**
- JS ⇄ 原生桥：`window.helios.call` → Promise，nlohmann 自动绑定（`bindJson` / `subscribeJson`）
- 双向 BroadcastChannel、eval / evalAsync、本地目录映射、insets
- 内置 `<helios-window-title-bar>` / `<helios-window-controls>` 组件（原生 app-region 拖动；窗口不可调整大小时最大化按钮自动禁用）

**系统能力**
- 托盘、右键菜单、消息框、文件 / 文件夹对话框、剪贴板、打开 URL、Toast 通知
- 公共控件 v6 由库自身激活：消息框 / 对话框现代化主题，无需 exe 清单

**示例**
- `HeliosViewDemo`：WebView 总演示 —— 滑块 + 输入框驱动全部功能
- 另有窗口 / 应用 / 系统 / WebView / 事件 / C 示例

**环境要求**：Windows 10/11，C++23（C API 可被 C99 使用），CMake ≥ 4.3，无 vcpkg。
