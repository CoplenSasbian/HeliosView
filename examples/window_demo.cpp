// HeliosView.Core window-feature example (signal/slot usage).
// Interact: move the mouse / press keys (Esc closes the window) / click the X
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>

int main()
{
    std::printf("HeliosView %s\n", helios::version().c_str());

    helios::App app;
    helios::Window window(800, 600, L"HeliosView Demo");
    window.show();

    helios::Menu menu(window.nativeHandle());
    helios::Menu::Item* showItem = menu.addItem(L"Show / Restore");
    helios::Menu::Item* minimizeItem = menu.addItem(L"Minimize");
    menu.addSeparator();
    helios::Menu::Item* quitItem = menu.addItem(L"Quit");
    showItem->triggered.connect([&window] {
        std::printf("[win] menu: show/restore\n");
        window.showNormal();
    });
    minimizeItem->triggered.connect([&window] {
        std::printf("[win] menu: minimize\n");
        window.showMinimized();
    });
    quitItem->triggered.connect([&app] {
        std::printf("[win] menu: quit\n");
        app.quit();
    });

    // Tray icon (notification area). The native window must exist first, so this
    // runs after show(). Connect signals to respond to clicks on the icon.
    helios::Tray tray(window.nativeHandle(), L"HeliosView Demo");
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
        if (key == helios::KeyCode::Escape)
            window.close();
    });

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
