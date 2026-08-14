// HeliosView.Core example: subclassing Window + connecting signals to member
// functions, plus the 1.0 window/system APIs (taskbar progress, backdrop,
// message box, clipboard, tray balloon).
//
// Signals connect via connect(&Class::member, this) and run on the UI thread.
#include <HeliosViewCore/HeliosView.h>

#include <memory>
#include <print>
#include <string>
#include <thread>

// Custom window: subclass core's Window
class MyWindow : public helios::Window {
public:
    MyWindow(int width, int height, const char* title,
             helios::WindowStyle style = helios::WindowStyle::Normal)
        : Window(width, height, title, style)
    {
        keyPressed.connect(&MyWindow::onKeyPressed, this);
        mouseButtonPressed.connect(&MyWindow::onMousePressed, this);
        resized.connect(&MyWindow::onResized, this);
        closed.connect(&MyWindow::onClosed, this);
    }

private:
    std::unique_ptr<helios::Tray> m_tray;
    std::string m_clip;
    int m_titleCount = 0;
    int m_progress = 0;
    bool m_opaque = true;

    // Sync member slot: key press (runs on the UI thread)
    void onKeyPressed(helios::KeyCode key)
    {
        std::println("[app] key {} (thread {})", static_cast<int>(key),
                     std::this_thread::get_id());
        switch (key) {
        case helios::KeyCode::Escape: close(); break;
        case helios::KeyCode::F1: showMinimized(); break;
        case helios::KeyCode::F2: showMaximized(); break;
        case helios::KeyCode::F3: showNormal(); break;
        case helios::KeyCode::F4: center(); break;
        case helios::KeyCode::F5: setOpacity(m_opaque ? 0.5f : 1.0f); m_opaque = !m_opaque; break;
        case helios::KeyCode::F6:
            setTitle(m_titleCount++ == 0 ? "Renamed Window" : "HeliosView App Demo");
            break;
        case helios::KeyCode::F7: move(100, 100); break;
        case helios::KeyCode::F8: resize(640, 480); break;
        case helios::KeyCode::F9: // taskbar progress (cycle 0..100)
            m_progress = (m_progress + 20) % 110;
            if (m_progress >= 100)
                clearProgress();
            else
                setProgress(static_cast<uint32_t>(m_progress), 100);
            break;
        case helios::KeyCode::F10: // Mica backdrop + dark mode (Win11)
            setBackdrop(helios::Backdrop::Mica);
            setDarkMode(true);
            break;
        case helios::KeyCode::F11: { // message box
            const helios::MessageBoxResult r = helios::messageBox(
                nativeHandle(), helios::MessageBoxType::Question,
                helios::MessageBoxButtons::YesNo, "Question", "Continue?");
            std::println("[app] message box -> {}", static_cast<int>(r));
            break;
        }
        case helios::KeyCode::F12: { // clipboard round-trip
            helios::clipboardSetText("Hello from HeliosView");
            if (helios::clipboardGetText(m_clip))
                std::println("[app] clipboard: {}", m_clip);
            break;
        }
        default: break;
        }
    }

    void onMousePressed(int32_t x, int32_t y, helios::MouseButton button)
    {
        std::println("[app] mouse {} at ({}, {})", static_cast<int>(button), x, y);
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

public:
    void createTray()
    {
        m_tray = std::make_unique<helios::Tray>(nativeHandle(), "HeliosView App Demo");
        if (!m_tray->valid()) { /* needs a created (shown) native window */
            m_tray.reset();
            return;
        }
        m_tray->leftClicked.connect([this] { std::println("[app] tray left-clicked"); });
        m_tray->leftDoubleClicked.connect([this] { close(); });
        m_tray->rightClicked.connect([this] { showNormal(); show(); });
        m_tray->notify("Tray", "Hello from the tray balloon");
    }
};

int main()
{
    std::println("HeliosView {}", helios::version());

    auto app = std::make_shared<helios::App>();

    // One window per built-in style
    auto normal = std::make_shared<MyWindow>(420, 320, "Normal Window");
    auto frameless = std::make_shared<MyWindow>(420, 320, "Frameless (bordered, no title bar)",
                                                helios::WindowStyle::Frameless);
    auto borderless = std::make_shared<MyWindow>(420, 320, "Borderless",
                                                 helios::WindowStyle::Borderless);
    normal->show();
    frameless->show();
    borderless->show();
    normal->createTray(); /* tray icon in the notification area (window must be shown first) */

    std::println("[main] entering UI loop on thread {} (F1-F12: window APIs, click / Esc)...",
                 std::this_thread::get_id());
    return app->exec();
}
