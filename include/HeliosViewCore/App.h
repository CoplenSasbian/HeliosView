#pragma once

/**
 * HeliosView.Core —— App：消息循环 + 事件队列（Qt: QCoreApplication 简化版）。
 *
 * 依赖：Types.h。窗口事件分发通过 detail::g_window_handlers 间接进行，
 * 不依赖 Window 的完整定义（由 Window.h 注册/注销）。
 */

#include <HeliosViewCore/Types.h>

#include <cstdint>
#include <functional>
#include <map>

namespace helios {

namespace detail {

// 窗口事件分发器：window id → 窗口 event() 的虚调用封装（std::function）。
// 由 Window 构造/移动时注册、销毁时注销；App::exec 据此把事件分发给对应窗口。
inline std::map<int32_t, std::function<bool(const Event&)>> g_window_handlers;

} // namespace detail

class App {
public:
    App() { s_instance = this; }
    virtual ~App() { if (s_instance == this) s_instance = nullptr; }
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // 当前 App 实例（Qt: QApplication::instance）
    static App* instance() { return s_instance; }

    // 进入消息循环（Qt: exec）。返回 0 正常退出。
    // 最后一个窗口被销毁后自动退出循环（Qt: quitOnLastWindowClosed）。
    int exec() { return heliosview_run(&App::loopCallback, this); }

    // 请求退出消息循环（Qt: quit）
    void quit() { heliosview_quit(); }

    // 事件队列（SDL 风格）：从队列取事件，事件到达前 waitEvent 阻塞
    bool pollEvent(Event& out)
    {
        heliosview_event_t c{};
        if (heliosview_poll(&c) == 1) {
            out = Event::fromC(c);
            return true;
        }
        return false;
    }
    bool waitEvent(Event& out)
    {
        heliosview_event_t c{};
        if (heliosview_wait(&c) == 1) {
            out = Event::fromC(c);
            return true;
        }
        return false;
    }

    // 任意线程投递事件（SDL: SDL_PushEvent / Qt: QCoreApplication::postEvent）
    void postEvent(const Event& e)
    {
        heliosview_event_t c = e.toC();
        heliosview_post_event(&c);
    }

    // 注册原生消息 → 事件转换委托（C 函数指针，语义见 heliosview.h）
    void setNativeHandler(heliosview_native_handler_fn handler)
    {
        heliosview_set_native_handler(handler);
    }

    // App 级事件：窗口未处理时回调（Qt: QCoreApplication::notify）。
    // 默认忽略；返回 true 表示已处理。
    virtual bool event(const Event&) { return false; }

private:
    static int loopCallback(void* userdata)
    {
        auto* self = static_cast<App*>(userdata);

        // 泵取全部排队事件，分发给目标窗口，未处理的上交 App::event()
        Event ev;
        while (self->pollEvent(ev)) {
            if (ev.type == EventType::Quit) {
                self->quit();
                break;
            }
            auto it = detail::g_window_handlers.find(ev.windowId);
            if (it != detail::g_window_handlers.end() && it->second(ev))
                continue;
            self->event(ev);
        }

        // 窗口全部关闭 → 退出循环
        if (detail::g_window_handlers.empty()) {
            self->quit();
            return 1;
        }
        return 0;
    }

    static inline App* s_instance = nullptr;
};

} // namespace helios
