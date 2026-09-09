// HeliosView.Core window-feature example (signal/slot usage).
// Interact: move the mouse / press keys (Esc closes the window) / click the X (connected to close())
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>

int main()
{
    std::printf("HeliosView %s\n", helios::version().c_str());

    helios::enableDpiAwareness(); // before any window: crisp, per-monitor DPI

    helios::App app;
    // Process identity + how the OS presents the process. A tray-only / menu-bar
    // -only app should use ActivationPolicy::Accessory: on macOS that is what
    // keeps the (otherwise useless) Dock icon away.
    helios::App::setAppId("com.example.heliosview.demo");
    helios::App::setActivationPolicy(helios::ActivationPolicy::Regular);
    std::printf("[app] id=%s policy=%d\n", helios::App::appId(),
                static_cast<int>(helios::App::activationPolicy()));

    helios::Window window(800, 600, "HeliosView Demo");
    window.show();

    // Size constraints: keep the window between 400x300 and 1200x900 (client).
    window.setMinimumSize(400, 300);
    window.setMaximumSize(1200, 900);

    // Frameless: a fully frameless window (no system title bar); the app draws
    // all chrome (title bar + buttons), and the top strip drags the window.
    helios::Window frameless(480, 320, "Frameless",
                             helios::WindowStyle::Frameless);
    frameless.resized.connect([](int32_t w, int32_t) {
        // keep the drag strip spanning the (new) window width
        std::printf("[frameless] resize %d\n", w);
    });
    frameless.show();

    // Style flags: the idiomatic macOS look — a real title bar with its title
    // hidden and the content underneath, traffic lights floating over the page.
    // On Windows the same flags mean "no native caption" (the frameless layout).
    helios::Window native(520, 360, "Native chrome", helios::WindowStyle::Normal,
                          helios::WindowFlag::TitleBarHidden
                              | helios::WindowFlag::TitleBarTransparent
                              | helios::WindowFlag::FullSizeContent
                              | helios::WindowFlag::Resizable);
    native.show();
    std::printf("[native] flags=0x%X scale=%.2f\n", helios::toUint(native.flags()),
                native.scaleFactor());

    // Application menu bar. macOS: the one global bar (the first menu becomes
    // the App menu). Windows: the menu bar of every HeliosView window. Roles
    // supply the platform's labels/shortcuts and the actions the app cannot do
    // itself (Ctrl+C/X/V go to the focused control).
    helios::MenuBar bar;
    bar.addMenu("File")->addRole(helios::MenuRole::Quit);
    helios::Menu* editMenu = bar.addMenu("Edit");
    editMenu->addRole(helios::MenuRole::Undo);
    editMenu->addRole(helios::MenuRole::Cut);
    editMenu->addRole(helios::MenuRole::Copy);
    editMenu->addRole(helios::MenuRole::Paste);
    helios::Menu* windowMenu = bar.addMenu("Window");
    windowMenu->setKind(helios::MenuKind::Window);   // macOS wires NSApp.windowsMenu
    windowMenu->addRole(helios::MenuRole::Minimize);
    windowMenu->addRole(helios::MenuRole::Zoom);
    helios::Action toggleFull("Toggle Fullscreen", "Primary+F");   // custom accelerator
    toggleFull.triggered.connect([&window] { window.setFullscreen(!window.isFullscreen()); });
    bar.addMenu("View")->addAction(toggleFull);
    bar.setAppMenu();

    helios::Menu menu;   // standalone popup: no window needed until show()
    helios::Menu::Item* showItem = menu.addItem("Show / Restore");
    helios::Menu::Item* minimizeItem = menu.addItem("Minimize");
    helios::Menu::Item* maximizeItem = menu.addItem("Maximize");
    helios::Menu::Item* resizableItem = menu.addItem("Toggle Resizable");
    menu.addSeparator();
    helios::Menu::Item* quitItem = menu.addRole(helios::MenuRole::Quit);  // platform label + shortcut
    menu.addSeparator();
    helios::Menu::Item* disabledItem = menu.addItem("Disabled (grey)");
    disabledItem->setEnabled(false); // grayed out, not selectable
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
    menu.setDefaultAction(*showItem); // bold default item (Enter / double-click)

    // Tray icon (notification area). Standalone: no window required, so it can be
    // created before (or without) show(). Connect signals to respond to clicks.
    helios::Tray tray("HeliosView Demo");
    // Attach the context menu instead of popping it up from rightClicked: that is
    // the only portable way (Linux exports the menu over DBus; macOS opens an
    // NSStatusItem menu) — with a menu attached the right-click event may not be
    // delivered at all. The tray keeps a reference to the menu.
    tray.setMenu(menu);
    tray.leftClicked.connect([] { std::printf("[win] tray left-click\n"); });
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

    window.closeRequested.connect([&window] {
        std::printf("[win] close requested -> closing\n");
        window.close();  // close button does NOT auto-close; must call close() here
    });

    return app.exec();
}
