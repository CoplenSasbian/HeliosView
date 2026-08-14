// HeliosView.Core example: signals/slots + the threading contract.
//
// Threading contract: all window/WebView/... APIs must be called on the
// message-loop thread -- the thread running App::exec. To return to it from a
// worker thread, use App::postTask(fn) (the sanctioned cross-thread mechanism).
// This demo runs a background std::thread that posts a task back to the UI
// thread when it finishes.
#include <HeliosViewCore/HeliosView.h>

#include <chrono>
#include <print>
#include <thread>

int main()
{
    std::println("HeliosView {}", helios::version());

    auto app = std::make_shared<helios::App>();

    helios::Window window(800, 600, "HeliosView Demo");
    window.show();

    // Sync slot: window closed
    window.closed.connect([] {
        std::println("[slot] window closed");
    });

    // Sync slot: key handling
    window.keyPressed.connect([&window](helios::KeyCode key) {
        std::println("[slot] key pressed: {}", static_cast<int>(key));
        if (key == helios::KeyCode::Escape)
            window.close();
    });

    // Background worker: sleeps off the UI thread, then posts back via
    // postTask (runs on the UI thread while the loop is idle).
    std::thread worker([app] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        app->postTask([] {
            std::println("[ui] background task finished -> back on the UI thread");
        });
    });
    worker.detach();

    std::println("[main] entering UI loop (press keys / Esc)...");
    const int rc = app->exec();
    std::println("[main] loop exited with {}", rc);
    return rc;
}
