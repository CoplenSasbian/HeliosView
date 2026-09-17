// ============================================================================
// HeliosView Example 02: Native Window Shell & OS Integration Demo
// ============================================================================
// Demonstrates native desktop client management without WebView:
//   1. Window Styles: Normal, Frameless (custom titlebar/drag), Borderless, Mica (Win11)
//   2. Window State: Fullscreen, Topmost, Opacity, Taskbar Progress & Badges
//   3. Menus: Application MenuBar & Context Popup Menu (checked/disabled/radio)
//   4. System Tray: Tray icon, tooltip, context menu, and balloon alerts
//   5. Native Dialogs: File/Folder pickers, MessageBox
//   6. OS Notifications: Windows Action Center Toasts
// ============================================================================

#include <HeliosViewCore/HeliosView.h>

#include <format>
#include <iostream>
#include <memory>
#include <string>

namespace {

class ShellWindow : public helios::Window {
public:
    ShellWindow(int width, int height, const char* title)
        : Window(width, height, title, helios::WindowStyle::Normal)
    {
        // 1. Connect input & state signals
        keyPressed.connect(&ShellWindow::onKeyPressed, this);
        mouseButtonPressed.connect(&ShellWindow::onMousePressed, this);
        resized.connect(&ShellWindow::onResized, this);
        closeRequested.connect(&ShellWindow::onCloseRequested, this);

        // 2. Set dark mode if supported
        setDarkMode(true);

        printInstructions();
    }

    // Context popup menu instance
    std::unique_ptr<helios::Menu> contextMenu;
    std::unique_ptr<helios::Tray> systemTray;

private:
    void printInstructions() {
        std::cout << "\n===========================================================\n";
        std::cout << " HeliosView Example 02: Native Window Shell Demo\n";
        std::cout << "===========================================================\n";
        std::cout << "  [TIP] These native window & OS integration capabilities have\n";
        std::cout << "        been integrated with graphical UI drivers in:\n";
        std::cout << "          - Example 03 (HeliosView_WebViewBridgeDemo): Web UI Driven\n";
        std::cout << "          - Example 04 (HeliosView_CanvasUiDemo): 2D Canvas UI Driven\n";
        std::cout << "-----------------------------------------------------------\n";
        std::cout << "  Keyboard Shortcuts for this minimal headless shell:\n";
        std::cout << "    [F1]  Maximize / Restore window\n";
        std::cout << "    [F2]  Minimize window\n";
        std::cout << "    [F3]  Toggle Fullscreen mode\n";
        std::cout << "    [F4]  Toggle Always-On-Top\n";
        std::cout << "    [F5]  Toggle Window Opacity (100% <-> 70%)\n";
        std::cout << "    [F6]  Cycle Taskbar Progress State (Normal -> Warning -> Error -> Clear)\n";
        std::cout << "    [F7]  Show Native File Open Dialog\n";
        std::cout << "    [F8]  Show Native Message Box\n";
        std::cout << "    [F9]  Send OS Toast Notification\n";
        std::cout << "    [F10] Win11 Mica Backdrop Toggle\n";
        std::cout << "    [F11] Flash Taskbar Icon\n";
        std::cout << "    [Esc] Close window cleanly\n";
        std::cout << "    [Right-Click Window] Open Context Menu\n";
        std::cout << "===========================================================\n\n";
    }

    void onKeyPressed(helios::KeyCode key) {
        switch (key) {
        case helios::KeyCode::Escape:
            std::cout << "[Action] Escape pressed -> Closing window.\n";
            close();
            break;

        case helios::KeyCode::F1:
            std::cout << "[Action] Toggle maximize window\n";
            toggleMaximize();
            break;

        case helios::KeyCode::F2:
            std::cout << "[Action] Minimize window\n";
            minimize();
            break;

        case helios::KeyCode::F3:
            m_fullscreen = !m_fullscreen;
            std::cout << std::format("[Action] Fullscreen mode: {}\n", m_fullscreen);
            setFullscreen(m_fullscreen);
            break;

        case helios::KeyCode::F4:
            m_topmost = !m_topmost;
            std::cout << std::format("[Action] Always on top: {}\n", m_topmost);
            setTopmost(m_topmost);
            break;

        case helios::KeyCode::F5:
            m_opacity = (m_opacity > 0.8f) ? 0.70f : 1.0f;
            std::cout << std::format("[Action] Set opacity: {:.0f}%\n", m_opacity * 100.0f);
            setOpacity(m_opacity);
            break;

        case helios::KeyCode::F6:
            cycleTaskbarProgress();
            break;

        case helios::KeyCode::F7: {
            std::cout << "[Action] Opening native file picker...\n";
            auto files = helios::openFiles(nativeHandle(), "Select Any File (HeliosView Dialogs)");
            if (!files.empty()) {
                std::cout << std::format("  Selected: {}\n", files.front());
            } else {
                std::cout << "  User cancelled dialog.\n";
            }
            break;
        }

        case helios::KeyCode::F8:
            std::cout << "[Action] Showing modal message box...\n";
            helios::messageBox(
                nativeHandle(),
                helios::MessageBoxType::Info,
                helios::MessageBoxButtons::Ok,
                "HeliosView Shell",
                "This is a native OS dialog triggered from C++."
            );
            break;

        case helios::KeyCode::F9:
            std::cout << "[Action] Dispatching OS Toast Notification...\n";
            helios::notificationShow("HeliosView System", "Native OS toast notification delivered!");
            break;

        case helios::KeyCode::F10:
            if (setBackdrop(helios::Backdrop::Mica) == 0) {
                std::cout << "[Action] Mica backdrop applied successfully (Win11).\n";
            } else {
                std::cout << "[Action] Mica backdrop not supported on this platform/version.\n";
            }
            break;

        case helios::KeyCode::F11:
            std::cout << "[Action] Flashing taskbar icon...\n";
            flash();
            break;

        default:
            break;
        }
    }

    void onMousePressed(int32_t x, int32_t y, helios::MouseButton button) {
        if (button == helios::MouseButton::Right && contextMenu) {
            std::cout << std::format("[Event] Right-click at ({}, {}) -> Showing context menu\n", x, y);
            contextMenu->show(nativeHandle());
        }
    }

    void onResized(int32_t w, int32_t h) {
        std::cout << std::format("[Event] Window resized to {}x{}\n", w, h);
    }

    void onCloseRequested() {
        std::cout << "[Event] Close button clicked -> Cleaning up and closing.\n";
        close();
    }

    void cycleTaskbarProgress() {
        m_progressStep = (m_progressStep + 1) % 4;
        switch (m_progressStep) {
        case 0:
            std::cout << "[Action] Taskbar progress: None / Cleared\n";
            clearProgress();
            break;
        case 1:
            std::cout << "[Action] Taskbar progress: Normal (40%)\n";
            setProgressState(helios::ProgressState::Normal);
            setProgress(40, 100);
            break;
        case 2:
            std::cout << "[Action] Taskbar progress: Paused/Warning (75%)\n";
            setProgressState(helios::ProgressState::Paused);
            setProgress(75, 100);
            break;
        case 3:
            std::cout << "[Action] Taskbar progress: Error (100%)\n";
            setProgressState(helios::ProgressState::Error);
            setProgress(100, 100);
            break;
        }
    }

    bool m_fullscreen = false;
    bool m_topmost = false;
    float m_opacity = 1.0f;
    int m_progressStep = 0;
};

} // namespace

int main() {
    auto app = std::make_shared<helios::App>();

    helios::notificationInit("HeliosViewShellDemo");

    // 1. Create main window
    auto win = std::make_unique<ShellWindow>(900, 600, "HeliosView Window Shell Demo");

    // 2. Setup Context Menu
    win->contextMenu = std::make_unique<helios::Menu>();
    auto* item1 = win->contextMenu->addItem("Maximize / Restore");
    item1->triggered.connect([w = win.get()] {
        w->toggleMaximize();
    });

    auto* item2 = win->contextMenu->addItem("Toggle Always on Top");
    item2->setCheckable(true);
    item2->triggered.connect([w = win.get(), item2] {
        bool top = !item2->checked();
        item2->setChecked(top);
        w->setTopmost(top);
    });

    win->contextMenu->addSeparator();

    auto* itemQuit = win->contextMenu->addItem("Quit Application");
    itemQuit->triggered.connect([w = win.get()] { w->close(); });

    // 3. Setup System Tray Icon
    win->systemTray = std::make_unique<helios::Tray>("HeliosView Shell Demo");
    win->systemTray->leftClicked.connect([w = win.get()] {
        std::cout << "[Tray] Left click on system tray -> Bringing window to front\n";
        w->showNormal();
        w->focus();
    });

    win->systemTray->notify("HeliosView Running", "Shell demo is active in system tray.", helios::NotifyIcon::Info, 3000);

    // Show window and run loop
    win->show();
    return app->exec();
}
