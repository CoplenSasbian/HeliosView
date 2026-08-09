// HeliosView.Core example: signals connected to async functions (async slots).
// The slot returns a std::execution::task coroutine; firing starts it on a
// separate thread (fire-and-forget), and co_await schedule switches between
// the Async pool and the UI thread.
//
// Important:
// 1. The async task runs on a separate thread, so captured objects must be
//    owned by the task (shared ownership via shared_ptr).
// 2. Write coroutine tasks as plain functions taking parameters: under MSVC,
//    captures in coroutine lambdas are corrupted after a cross-thread
//    co_await (compiler/library layout issue); parameters are unaffected.
#include <HeliosViewCore/HeliosView.h>

#include <chrono>
#include <memory>
#include <print>
#include <thread>

// Coroutine slot: key handling (plain function, params passed in)
std::execution::task<void> handleKey(std::shared_ptr<helios::App> app,
                                     helios::Window& window, helios::KeyCode key)
{
    std::println("[slot] key pressed: {}", static_cast<int>(key));
    if (key == helios::KeyCode::Escape)
        window.close();
    co_return;
}

// Coroutine slot: mouse click -> background pool for slow work -> postTask to UI
std::execution::task<void> handleMouse(std::shared_ptr<helios::App> app,
                                       std::shared_ptr<helios::Async> async,
                                       int32_t x, int32_t y, helios::MouseButton button)
{
    std::println("[slot] mouse {} at ({}, {}) -> background...",
                 static_cast<int>(button), x, y);

    co_await std::execution::schedule(async->get_scheduler()); // switch to the background pool
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::println("[slot] background work done (pool thread)");

    // Back to the UI thread (postTask: UI updates without coroutine-resumption races)
    app->postTask([app] {
        std::println("[slot] back on UI thread");
    });
}

int main()
{
    std::println("HeliosView {}", helios::version());

    // shared_ptr: async slot tasks capture by value; App/Async stay alive until all tasks finish
    auto app = std::make_shared<helios::App>();
    auto async = std::make_shared<helios::Async>();

    helios::Window window(800, 600, L"HeliosView Async Slot Demo");
    window.show();

    // Sync slot: window closed
    window.closed.connect([] {
        std::println("[slot] window closed");
    });

    // Async slot: key (thin lambda forwarding to the coroutine function)
    window.keyPressed.connect([app, &window](helios::KeyCode key) {
        return handleKey(app, window, key);
    });

    window.mouseButtonPressed.connect([app, async](int32_t x, int32_t y, helios::MouseButton button) {
        return handleMouse(app, async, x, y, button);
    });

    std::println("[main] entering UI loop (click / press keys / Esc)...");
    return app->exec();
}
