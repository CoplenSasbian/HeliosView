// HeliosView.Core window-feature example (signal/slot usage).
// Interact: move the mouse / press keys (Esc closes the window) / click the X
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>

int main()
{
    std::printf("HeliosView %s\n", helios::version().c_str());

    helios::App app;
    helios::Window window(800, 600, "HeliosView Demo");
    window.show();

    // Signal/slot: connect lambdas directly, no Window subclassing
    window.resized.connect([](int32_t w, int32_t h) {
        std::printf("[win] resize %d x %d\n", w, h);
    });

    window.keyPressed.connect([&window](helios::KeyCode key) {
        std::printf("[win] key down: %d\n", static_cast<int>(key));
        if (key == helios::KeyCode::Escape)
            window.close();
    });

    window.mouseMoved.connect([](int32_t x, int32_t y) {
        std::printf("[win] mouse move: %d, %d\n", x, y);
    });

    window.mouseButtonPressed.connect([](int32_t x, int32_t y, helios::MouseButton button) {
        std::printf("[win] mouse button %d down at %d, %d\n",
                    static_cast<int>(button), x, y);
    });

    window.closed.connect([] {
        std::printf("[win] close requested\n");
        // Default: Window::event destroys the window after emitting closed;
        // App::exec exits once the last window is closed
    });

    return app.exec();
}
