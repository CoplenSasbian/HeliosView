// HeliosView.Core example: subclassing Window + connecting signals to member
// functions.
// The custom window class inherits core's Window; signals connect via
// connect(&Class::member, this):
//   - sync member functions: called directly on the emitting (UI) thread
//   - async member functions: return task; started on a separate thread when
//     emitted, can co_await to switch threads
#include <HeliosViewCore/HeliosView.h>

#include <chrono>
#include <memory>
#include <print>
#include <thread>

// Custom window: subclass core's Window
class MyWindow : public helios::Window {
public:
    MyWindow(int width, int height, const char* title,
             std::shared_ptr<helios::App> app, std::shared_ptr<helios::Async> async,
             helios::WindowStyle style = helios::WindowStyle::Normal)
        : Window(width, height, title, style)
        , m_app(std::move(app))
        , m_async(std::move(async))
    {
        // Signals -> member functions (mix of sync and async)
        keyPressed.connect(&MyWindow::onKeyPressed, this);
        mouseButtonPressed.connect(&MyWindow::onMousePressed, this);
        resized.connect(&MyWindow::onResized, this);
        closed.connect(&MyWindow::onClosed, this);
    }

private:
    std::shared_ptr<helios::App> m_app;
    std::shared_ptr<helios::Async> m_async;

    // Sync member slot: key press (runs on the UI thread)
    void onKeyPressed(helios::KeyCode key)
    {
        std::println("[app] key pressed: {} (thread {})", static_cast<int>(key),
                     std::this_thread::get_id());
        switch (key) {
        case helios::KeyCode::Escape:
            close();
            break;
        case helios::KeyCode::F1: showMinimized(); break;
        case helios::KeyCode::F2: showMaximized(); break;
        case helios::KeyCode::F3: showNormal(); break;
        case helios::KeyCode::F4: center(); break;
        case helios::KeyCode::F5: setOpacity(m_opaque ? 0.5f : 1.0f); m_opaque = !m_opaque; break;
        case helios::KeyCode::F6: setTitle(m_titleCount++ == 0 ? "Renamed Window" : "HeliosView App Demo"); break;
        case helios::KeyCode::F7: move(100, 100); break;
        case helios::KeyCode::F8: resize(640, 480); break;
        default: break;
        }
    }

    bool m_opaque = true;
    int m_titleCount = 0;

    // Async member slot: click -> slow work on the pool -> postTask back to UI
    std::execution::task<void> onMousePressed(int32_t x, int32_t y,
                                              helios::MouseButton button)
    {
        std::println("[app] mouse {} at ({}, {}) -> background... (thread {})",
                     static_cast<int>(button), x, y, std::this_thread::get_id());

        co_await std::execution::schedule(m_async->get_scheduler()); // switch to the background pool
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::println("[app] background work done (pool thread {} now)",
                     std::this_thread::get_id());

        m_app->postTask([this] { // back to the UI thread
            std::println("[app] back on UI thread (thread {})",
                         std::this_thread::get_id());
        });
    }

    void onResized(int32_t w, int32_t h)
    {
        std::println("[app] resized {} x {} (thread {})", w, h,
                     std::this_thread::get_id());
    }

    void onClosed()
    {
        std::println("[app] closed (thread {})", std::this_thread::get_id());
    }
};

int main()
{
    std::println("HeliosView {}", helios::version());

    auto app = std::make_shared<helios::App>();
    auto async = std::make_shared<helios::Async>();

    // One window per built-in style (held via shared_ptr: an async member
    // slot's task may outlive window close)
    auto normal = std::make_shared<MyWindow>(420, 320, "Normal Window", app, async);
    auto frameless = std::make_shared<MyWindow>(420, 320, "Frameless (bordered, no title bar)",
                                                app, async, helios::WindowStyle::Frameless);
    auto borderless = std::make_shared<MyWindow>(420, 320, "Borderless",
                                                 app, async, helios::WindowStyle::Borderless);
    normal->show();
    frameless->show();
    borderless->show();

    std::println("[main] entering UI loop on thread {} (click / press keys / Esc)...",
                 std::this_thread::get_id());
    return app->exec();
}
