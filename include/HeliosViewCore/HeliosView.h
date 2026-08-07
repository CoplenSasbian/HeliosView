#pragma once

/**
 * HeliosView.Core —— header-only C++ 封装层（统一入口）。
 *
 * ABI 稳定性由底层的 HeliosView.dll C 接口保证；
 * 本头文件提供类似 Qt 的类型安全封装。各功能在独立头文件中，
 * 可只 include 需要的部分，或直接 include 本文件（推荐）：
 *
 *   - Signal.h   信号槽（std::function + C++23 std::flat_set）
 *   - Types.h    事件类型与事件结构（与 C 接口一一对应）
 *   - App.h      消息循环 + 事件队列（Qt: QCoreApplication）
 *   - Window.h   顶层窗口 + 信号（Qt: QWidget）
 *
 * 用法（Qt 风格，信号槽）：
 *   helios::App app;
 *   helios::Window win(800, 600, "title");
 *   win.keyPressed.connect(...);
 *   win.show();
 *   return app.exec();   // 消息循环，最后一个窗口关闭后退出
 *
 * 所有函数均为 inline，无需链接额外的库（HeliosView.dll 除外，
 * 它由 CMake 目标 HeliosView::Core 自动传递）。
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/Types.h>
#include <HeliosViewCore/Window.h>

#include <string>

namespace helios {

/* ---------- 其他 ---------- */

// 返回库版本号，例如 "0.1.0"
inline std::string version()
{
    return heliosview_version();
}

} // namespace helios
