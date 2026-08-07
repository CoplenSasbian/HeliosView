#pragma once

/**
 * HeliosView.Core —— Window：顶层窗口（Qt: QWidget 简化版）。
 *
 * 依赖：Signal.h、Types.h、App.h（窗口注册表）。
 * 事件经 event() 分发为信号；信号槽用 window.keyPressed.connect(...) 连接。
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/Types.h>

#include <cstdint>

namespace helios {

class Window {
public:
    // 创建窗口（原生窗口在 show() 时创建）
    Window(int width, int height, const char* title)
        : m_window(heliosview_window_create(width, height, title))
    {
        if (m_window)
            registerHandler();
    }

    virtual ~Window() { close(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept : m_window(other.m_window)
    {
        other.m_window = nullptr;
        if (m_window)
            registerHandler();
    }

    Window& operator=(Window&& other) noexcept
    {
        if (this != &other) {
            close();
            m_window = other.m_window;
            other.m_window = nullptr;
            if (m_window)
                registerHandler();
        }
        return *this;
    }

    // 创建并显示原生窗口
    void show() { heliosview_window_show(m_window); }

    // 关闭并销毁窗口；若这是最后一个窗口，消息循环随之退出
    void close()
    {
        if (!m_window)
            return;
        detail::g_window_handlers.erase(id());
        heliosview_window_destroy(m_window);
        m_window = nullptr;
    }

    // 窗口 id（事件中的 windowId）
    int32_t id() const { return m_window ? heliosview_window_id(m_window) : 0; }

    /* ===== 信号（Qt 风格：window.keyPressed.connect(...)） ===== */

    Signal<> closed;                                       // 窗口关闭请求（用户点 X），发射后默认销毁窗口
    Signal<int32_t, int32_t> resized;                      // 尺寸变化 (w, h)
    Signal<KeyCode> keyPressed;                            // 按键按下（已过滤自动重复）
    Signal<KeyCode> keyReleased;                           // 按键抬起
    Signal<int32_t, int32_t> mouseMoved;                   // 鼠标移动 (x, y)
    Signal<int32_t, int32_t, MouseButton> mouseButtonPressed;  // 按下 (x, y, button)
    Signal<int32_t, int32_t, MouseButton> mouseButtonReleased; // 抬起 (x, y, button)

    // 窗口事件处理（Qt: QWidget::event）。默认实现把事件转换为信号发射。
    // 返回 true 表示已处理；未处理的会交给 App::event()。
    // 如需阻止关闭（veto），重写本函数拦截 WindowClose 并返回 true。
    virtual bool event(const Event& e)
    {
        switch (e.type) {
        case EventType::WindowResize:
            resized(e.width, e.height);
            return true;
        case EventType::KeyDown:
            keyPressed(e.key);
            return true;
        case EventType::KeyUp:
            keyReleased(e.key);
            return true;
        case EventType::MouseMove:
            mouseMoved(e.x, e.y);
            return true;
        case EventType::MouseButtonDown:
            mouseButtonPressed(e.x, e.y, e.mouseButton);
            return true;
        case EventType::MouseButtonUp:
            mouseButtonReleased(e.x, e.y, e.mouseButton);
            return true;
        case EventType::WindowClose:
            closed();
            close(); /* 默认：关闭请求 → 销毁窗口（App 在最后一个窗口关闭后退出） */
            return true;
        default:
            return false; /* Quit 等不属于窗口事件 */
        }
    }

private:
    // 注册事件分发器：窗口 id → 本对象 event() 的虚调用
    void registerHandler()
    {
        detail::g_window_handlers[id()] = [this](const Event& e) { return event(e); };
    }

    heliosview_window_t* m_window = nullptr;
};

} // namespace helios
