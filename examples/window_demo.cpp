// HeliosView.Core window-feature example (signal/slot usage).
// Interact: move the mouse / press keys (Esc closes the window) / click the X
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>

int main()
{
    std::printf("HeliosView %s\n", helios::version().c_str());

    helios::enableDpiAwareness(); // before any window: crisp, per-monitor DPI

    helios::App app;
    helios::Window window(800, 600, "HeliosView Demo");
    window.show();

    // Size constraints: keep the window between 400x300 and 1200x900 (client).
    window.setMinimumSize(400, 300);
    window.setMaximumSize(1200, 900);

    // Frameless window with a custom title bar: register a drag region so the
    // top strip moves the window (like a native title bar), and three window
    // control buttons (minimize / maximize / close) at the top-right corner.
    // The buttons look like whatever you draw in the client area; the library
    // wires them to the real title-bar behavior (click = action, no drag).
    helios::Window frameless(480, 320, "Frameless", helios::WindowStyle::Frameless);
    frameless.addDragRegion(0, 0, 480, 40); // the custom title-bar strip
    frameless.addControlButton(helios::ControlButton::Minimize, 480 - 3 * 46, 0, 46, 40);
    frameless.addControlButton(helios::ControlButton::Maximize, 480 - 2 * 46, 0, 46, 40);
    frameless.addControlButton(helios::ControlButton::Close, 480 - 1 * 46, 0, 46, 40);
    frameless.resized.connect([](int32_t w, int32_t) {
        // keep the drag strip spanning the (new) window width
        std::printf("[frameless] resize %d\n", w);
    });
    frameless.show();

    helios::Menu menu(window.nativeHandle());
    helios::Menu::Item* showItem = menu.addItem("Show / Restore");
    helios::Menu::Item* minimizeItem = menu.addItem("Minimize");
    helios::Menu::Item* maximizeItem = menu.addItem("Maximize");
    helios::Menu::Item* resizableItem = menu.addItem("Toggle Resizable");
    menu.addSeparator();
    helios::Menu::Item* quitItem = menu.addItem("Quit");
    showItem->triggered.connect([&window] {
        std::printf("[win] menu: show/restore\n");
        window.showNormal();
    });
    minimizeItem->triggered.connect([&window] {
        std::printf("[win] menu: minimize\n");
        window.minimize();
    });
    maximizeItem->triggered.connect([&window] {
        std::printf("[win] menu: toggle maximize\n");
        window.toggleMaximize();
    });
    resizableItem->triggered.connect([&window] {
        const bool on = window.state() != helios::ShowState::Maximized; // demo: arbitrary
        std::printf("[win] menu: resizable = %d\n", on);
        window.setResizable(on);
    });
    quitItem->triggered.connect([&app] {
        std::printf("[win] menu: quit\n");
        app.quit();
    });

    // Tray icon (notification area). The native window must exist first, so this
    // runs after show(). Connect signals to respond to clicks on the icon.
    helios::Tray tray(window.nativeHandle(), "HeliosView Demo");
    tray.leftClicked.connect([] { std::printf("[win] tray left-click\n"); });
    tray.rightClicked.connect([&] {
        std::printf("[win] tray right-click -> menu\n");
        menu.show(window.nativeHandle()); // context menu at the cursor
    });
    tray.leftDoubleClicked.connect([&app] {
        std::printf("[win] tray double-click -> quit\n");
        app.quit();
    });

    // Signal/slot: connect lambdas directly, no Window subclassing
    window.resized.connect([](int32_t w, int32_t h) {
        std::printf("[win] resize %d x %d\n", w, h);
    });

    window.keyPressed.connect([&](helios::KeyCode key) {
        std::printf("[win] key down: %d\n", static_cast<int>(key));
        switch (key) {
        case helios::KeyCode::Escape:
            window.close();
            break;
        case helios::KeyCode::F1:
            window.toggleMaximize();
            break;
        case helios::KeyCode::F2:
            window.minimize();
            break;
        case helios::KeyCode::F3:
            window.restore();
            break;
        case helios::KeyCode::F4:
            window.setResizable(window.state() != helios::ShowState::Maximized);
            break;
        case helios::KeyCode::F5:
            window.setFullscreen(!window.isFullscreen());
            break;
        case helios::KeyCode::F6:
            window.flash(); /* background-task-finished hint */
            break;
        case helios::KeyCode::F7:
            window.setEnabled(!window.isEnabled()); /* modal lock */
            break;
        default:
            break;
        }
    });

    window.focused.connect([] { std::printf("[win] focus gained\n"); });
    window.blurred.connect([] { std::printf("[win] focus lost\n"); });
    window.moved.connect([](int32_t x, int32_t y) {
        std::printf("[win] moved to %d, %d\n", x, y);
    });
    window.sizing.connect([](int32_t w, int32_t h) {
        std::printf("[win] sizing %d x %d\n", w, h);
    });
    window.enabledChanged.connect([](bool on) {
        std::printf("[win] enabled = %d\n", on);
    });
    std::printf("[win] dpi = %u\n", window.dpi());

    window.mouseMoved.connect([](int32_t x, int32_t y) {
        std::printf("[win] mouse move: %d, %d\n", x, y);

    });

    window.mouseButtonPressed.connect([&](int32_t x, int32_t y, helios::MouseButton button) {
        std::printf("[win] mouse button %d down at %d, %d\n",
                    static_cast<int>(button), x, y);
        menu.show(window.nativeHandle());

    });

    window.closed.connect([] {
        std::printf("[win] close requested\n");
        // Default: Window::event destroys the window after emitting closed;
        // App::exec exits once the last window is closed
    });

    return app.exec();
}
