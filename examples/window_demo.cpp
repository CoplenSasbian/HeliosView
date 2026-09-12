// HeliosView.Core example: Window - styles, state, signals, menus and the tray.
//
// This is the "no WebView" window demo: everything a Window (and the app around it)
// offers, driven from the keyboard and the mouse. The WebView demos show the JS
// bridge; this one shows the native windowing layer on its own.
//
// It also shows the two ways to react to events, side by side:
//   - member slots:  subclass Window and connect(&MyWindow::onKeyPressed, this)
//   - lambdas:       window.resized.connect([](int32_t w, int32_t h) { ... })
// Both run on the message-loop thread.
//
// Controls (the window with focus receives the keys):
//   Esc   close the focused window (the demo exits with the last one)
//   F1    maximize                  F2    minimize
//   F3    restore / show normal     F4    toggle resizable
//   F5    toggle fullscreen         F6    toggle topmost
//   F7    opacity 100% <-> 60%      F8    flash the taskbar
//   F9    taskbar progress step     F10   clear taskbar progress
//   F11   Mica backdrop + dark mode (Win11)
//   F12   rename the window
//   right-click inside the main window   popup menu (also attached to the tray)
//   tray left click / double click / right click -> balloon, restore, menu (Quit)
//
// The extra windows demonstrate the built-in styles and the WindowFlag combinations;
// see HeliosViewCore/Window.h for the full list (WindowStyle::Borderless, the other
// flags, addDragRegion for a custom drag strip, and so on).
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>
#include <memory>

namespace {

/* ---------- a Window subclass with member-function slots ---------- */

class DemoWindow : public helios::Window {
public:
    DemoWindow(int width, int height, const char* title,
               helios::WindowStyle style = helios::WindowStyle::Normal)
        : Window(width, height, title, style)
    {
        /* Member slots: the signal calls this object's method on the UI thread.
         * (Signal::connect(&Class::method, object) - the object must outlive the
         * connection, which it does here: the window owns the slots.) */
        keyPressed.connect(&DemoWindow::onKeyPressed, this);
        mouseButtonPressed.connect(&DemoWindow::onMousePressed, this);
        resized.connect(&DemoWindow::onResized, this);
        closeRequested.connect(&DemoWindow::onCloseRequested, this);
    }

    /* The popup menu is shown by onMousePressed; it is owned by main() (the tray
     * keeps a reference to it too, so it must outlive both). */
    helios::Menu* popup = nullptr;

private:
    void onKeyPressed(helios::KeyCode key)
    {
        switch (key) {
        case helios::KeyCode::Escape:
            close(); /* the close button does NOT auto-close: the app decides */
            break;
        case helios::KeyCode::F1:
            maximize();
            break;
        case helios::KeyCode::F2:
            minimize();
            break;
        case helios::KeyCode::F3:
            showNormal(); /* restore from minimized/maximized */
            break;
        case helios::KeyCode::F4:
            m_resizable = !m_resizable;
            setResizable(m_resizable);
            std::printf("[win] resizable = %d\n", m_resizable);
            break;
        case helios::KeyCode::F5:
            setFullscreen(!isFullscreen());
            break;
        case helios::KeyCode::F6:
            m_topmost = !m_topmost;
            setTopmost(m_topmost);
            std::printf("[win] topmost = %d\n", m_topmost);
            break;
        case helios::KeyCode::F7:
            m_opaque = !m_opaque;
            setOpacity(m_opaque ? 1.0f : 0.6f);
            break;
        case helios::KeyCode::F8:
            flash(); /* hint that a background task finished (taskbar / dock) */
            break;
        case helios::KeyCode::F9:
            /* Taskbar progress: an indeterminate state, then 0..100% in 20% steps. */
            if (m_progress == 0) {
                setProgressState(helios::ProgressState::Indeterminate);
                m_progress = 20;
            } else if (m_progress >= 100) {
                clearProgress();
                m_progress = 0;
            } else {
                setProgress(static_cast<uint32_t>(m_progress), 100);
                m_progress += 20;
            }
            std::printf("[win] taskbar progress step -> %d%%\n", m_progress);
            break;
        case helios::KeyCode::F10:
            clearProgress();
            m_progress = 0;
            break;
        case helios::KeyCode::F11:
            /* Mica + dark mode: Windows 11 backdrop; on other platforms/versions
             * setBackdrop returns an error code and the window keeps its look. */
            if (setBackdrop(helios::Backdrop::Mica) == 0)
                setDarkMode(true);
            std::printf("[win] backdrop=Mica dark=1\n");
            break;
        case helios::KeyCode::F12:
            m_titles = (m_titles + 1) % 3;
            setTitle(m_titles == 0   ? "HeliosView Window Demo"
                     : m_titles == 1 ? "Renamed Window"
                                     : "HeliosView - hello");
            break;
        default:
            break;
        }
    }

    void onMousePressed(int32_t x, int32_t y, helios::MouseButton button)
    {
        std::printf("[win] mouse button %d at %d, %d\n", static_cast<int>(button), x, y);
        if (popup && button == helios::MouseButton::Right)
            popup->show(nativeHandle()); /* popup at the cursor, owned by this window */
    }

    void onResized(int32_t w, int32_t h)
    {
        std::printf("[win] resized to %d x %d\n", w, h);
    }

    void onCloseRequested()
    {
        std::printf("[win] close requested\n");
        close();
    }

    int m_progress = 0;
    int m_titles = 0;
    bool m_opaque = true;
    bool m_topmost = false;
    bool m_resizable = true;
};

/* ---------- the standalone popup menu (shared with the tray) ---------- */

std::unique_ptr<helios::Menu> makePopupMenu(helios::Window& window, bool& topmost)
{
    auto menu = std::make_unique<helios::Menu>();

    /* Convenience items: the menu creates and owns the action behind each one. */
    helios::Menu::Item* showItem = menu->addItem("Show / Restore");
    helios::Menu::Item* minimizeItem = menu->addItem("Minimize");
    helios::Menu::Item* maximizeItem = menu->addItem("Maximize");
    menu->addSeparator();

    /* A checkable item: the app owns the state (the menu only draws the checkmark). */
    helios::Menu::Item* topmostItem = menu->addCheckItem("Always on top", topmost);
    topmostItem->triggered.connect([&window, topmostItem, &topmost] {
        topmost = !topmost;
        window.setTopmost(topmost);
        topmostItem->setChecked(topmost);
    });

    menu->addSeparator();
    menu->addItem("Disabled (greyed out)")->setEnabled(false);
    menu->addSeparator();

    /* Platform roles: the OS supplies the label, the shortcut and the action. */
    menu->addRole(helios::MenuRole::Quit)->triggered.connect([] {
        std::printf("[menu] quit\n");
        if (auto* a = helios::App::instance())
            a->quit();
    });

    showItem->triggered.connect([&window] { window.showNormal(); });
    minimizeItem->triggered.connect([&window] { window.minimize(); });
    maximizeItem->triggered.connect([&window] { window.toggleMaximize(); });

    menu->setDefaultAction(*showItem); /* bold: Enter / double-click activates it */
    return menu;
}

} // namespace

int main()
{
    std::printf("HeliosView %s - window / menu / tray demo\n", helios::version().c_str());

    /* DPI awareness is initialized automatically when the first window is created;
     * calling it explicitly is still supported for early initialization. */
    helios::enableDpiAwareness();

    /* Process identity + how the OS presents the process. A tray-only / menu-bar-only
     * app should use ActivationPolicy::Accessory: on macOS that is what keeps the
     * (otherwise useless) Dock icon away. */
    helios::App::setAppId("com.example.heliosview.windowdemo");
    helios::App::setActivationPolicy(helios::ActivationPolicy::Regular);
    std::printf("[app] id=%s policy=%d\n", helios::App::appId(),
                static_cast<int>(helios::App::activationPolicy()));

    /* Order matters: the App first (windows register their events through it), and
     * the windows/tray/menu destroyed before it - they are locals here, so they die
     * before `app` does. */
    helios::App app;

    /* ---------- the main window (subclassed: member-function slots) ---------- */

    DemoWindow window(880, 560, "HeliosView Window Demo");
    window.setMinimumSize(480, 320); /* client-area size constraints */
    window.setMaximumSize(1600, 1200);
    window.show();

    /* Lambda slots: connect without subclassing. All of these are optional - the
     * demo logs them so the event stream is visible on the console. */
    window.firstShown.connect([] { std::printf("[win] first shown\n"); });
    window.moved.connect([](int32_t x, int32_t y) { std::printf("[win] moved to %d, %d\n", x, y); });
    window.sizing.connect([](int32_t w, int32_t h) { std::printf("[win] sizing %d x %d\n", w, h); });
    window.focused.connect([] { std::printf("[win] focus gained\n"); });
    window.blurred.connect([] { std::printf("[win] focus lost\n"); });
    window.minimized.connect([] { std::printf("[win] minimized\n"); });
    window.maximized.connect([] { std::printf("[win] maximized\n"); });
    window.restored.connect([] { std::printf("[win] restored\n"); });
    window.enabledChanged.connect([](bool on) { std::printf("[win] enabled = %d\n", on); });
    std::printf("[win] dpi = %u, title bar height = %d px, scale = %.2f\n", window.dpi(),
                window.titleBarHeight(), window.scaleFactor());

    /* ---------- styles and flags on the extra windows ---------- */

    /* WindowStyle::Frameless: no system title bar at all - the app draws the whole
     * chrome and calls startDrag() (or registers a drag region) to move it. The
     * WebView demos do exactly that with the injected <helios-window-title-bar>. */
    helios::Window frameless(420, 260, "Frameless", helios::WindowStyle::Frameless);
    /* The close button only *asks* (closeRequested); the app decides. Without this,
     * clicking X would do nothing - and the demo would be stuck with the window. */
    frameless.closeRequested.connect([&frameless] { frameless.close(); });
    frameless.show();

    /* WindowFlag::*: the macOS "title bar hidden + content underneath" look; on
     * Windows the same flags mean "no native caption". Styles and flags combine. */
    helios::Window native(420, 260, "Native chrome", helios::WindowStyle::Normal,
                          helios::WindowFlag::TitleBarHidden
                              | helios::WindowFlag::TitleBarTransparent
                              | helios::WindowFlag::FullSizeContent
                              | helios::WindowFlag::Resizable);
    native.closeRequested.connect([&native] { native.close(); });
    native.show();
    std::printf("[native] flags = 0x%X\n", helios::toUint(native.flags()));

    /* A third built-in style, WindowStyle::Borderless (no border, no title bar),
     * is available too - see HeliosViewCore/Window.h. */

    /* ---------- menu bar (macOS: the one global bar; Windows: this window's bar) */

    helios::MenuBar bar;
    bar.addMenu("File")->addRole(helios::MenuRole::Quit);
    helios::Menu* editMenu = bar.addMenu("Edit");
    editMenu->addRole(helios::MenuRole::Undo);
    editMenu->addRole(helios::MenuRole::Cut);
    editMenu->addRole(helios::MenuRole::Copy);
    editMenu->addRole(helios::MenuRole::Paste);
    helios::Menu* windowMenu = bar.addMenu("Window");
    windowMenu->setKind(helios::MenuKind::Window); /* macOS wires NSApp.windowsMenu */
    windowMenu->addRole(helios::MenuRole::Minimize);
    windowMenu->addRole(helios::MenuRole::Zoom);

    /* A custom action with a custom accelerator ("Primary" = Ctrl / Cmd). */
    helios::Action toggleFullscreen("Toggle Fullscreen", "Primary+F");
    toggleFullscreen.triggered.connect([&window] { window.setFullscreen(!window.isFullscreen()); });
    bar.addMenu("View")->addAction(toggleFullscreen);
    bar.setAppMenu();

    /* ---------- popup menu + tray ---------- */

    /* Declaration order = lifetime: the tray holds a reference to the menu, so the
     * menu must outlive it (locals are destroyed in reverse order). */
    bool topmost = false;
    std::unique_ptr<helios::Menu> menu = makePopupMenu(window, topmost);
    std::unique_ptr<helios::Tray> tray = std::make_unique<helios::Tray>("HeliosView Window Demo");
    window.popup = menu.get();

    if (tray->valid()) {
        /* Attach the menu to the tray instead of popping it up from rightClicked:
         * that is the only portable way (on Linux the menu is exported over DBus,
         * on macOS it becomes the NSStatusItem menu). */
        tray->setMenu(*menu);
        tray->leftClicked.connect([] { std::printf("[tray] left click\n"); });
        tray->leftDoubleClicked.connect([&window] {
            std::printf("[tray] double click -> show\n");
            window.showNormal();
            window.focus();
        });
        tray->rightClicked.connect([&window] {
            std::printf("[tray] right click -> menu\n");
            window.showNormal();
        });
        tray->notify("HeliosView", "Tray balloon: the demo is still running", helios::NotifyIcon::Info);
    } else {
        std::printf("[tray] no notification area available; running without a tray icon\n");
    }

    std::printf("[demo] Esc closes the focused window (the demo ends with the last one) | "
                "F1 maximize | F2 minimize | F3 restore | F4 resizable | F5 fullscreen | "
                "F6 topmost | F7 opacity | F8 flash | F9/F10 taskbar progress | F11 Mica | "
                "F12 rename | right-click = popup menu | tray/tray-menu = quit\n");
    return app.exec();
}
