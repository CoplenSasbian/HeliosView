// HeliosView.Core example: the 1.0 system APIs (dialogs, clipboard, toasts,
// taskbar progress, tray balloon, open-URL / show-in-folder).
#include <HeliosViewCore/HeliosView.h>

#include <memory>
#include <print>
#include <string>

int main()
{
    std::println("HeliosView {}", helios::version());

    auto app = std::make_shared<helios::App>();
    helios::Window window(600, 400, "HeliosView System Demo");
    window.show();

    // OS toasts: thread-agnostic; init once at startup.
    helios::notificationInit("HeliosView.SystemDemo");

    window.keyPressed.connect([&window](helios::KeyCode key) {
        switch (key) {
        case helios::KeyCode::Escape:
            window.close();
            break;
        case helios::KeyCode::F1: {
            std::string path;
            if (helios::selectFolder(window.nativeHandle(), "Pick a folder", path))
                std::println("[demo] folder: {}", path);
            break;
        }
        case helios::KeyCode::F2: {
            const auto files = helios::openFiles(
                window.nativeHandle(), "Pick images",
                "Images (*.png;*.jpg)|*.png;*.jpg|All files (*.*)|*.*", /*multi=*/true);
            for (const auto& f : files)
                std::println("[demo] file: {}", f);
            break;
        }
        case helios::KeyCode::F3: {
            std::string path;
            if (helios::saveFile(window.nativeHandle(), "Save as", "Text (*.txt)|*.txt",
                                 "out.txt", path))
                std::println("[demo] save to: {}", path);
            break;
        }
        case helios::KeyCode::F4:
            helios::messageBox(window.nativeHandle(), helios::MessageBoxType::Info,
                               helios::MessageBoxButtons::Ok, "Info",
                               "Hello from HeliosView");
            break;
        case helios::KeyCode::F5:
            helios::openUrl("https://example.com");
            break;
        case helios::KeyCode::F6:
            helios::showInFolder("C:\\Windows");
            break;
        case helios::KeyCode::F7:
            helios::clipboardSetText("clipboard test");
            break;
        case helios::KeyCode::F8:
            helios::notificationShow("Download", "Finished");
            break;
        case helios::KeyCode::F9:
            window.setProgress(60, 100);
            break;
        case helios::KeyCode::F10:
            window.clearProgress();
            break;
        default:
            break;
        }
    });

    std::println("[demo] F1 folder | F2 files | F3 save | F4 msg box | F5 open URL | "
                 "F6 show in folder | F7 clipboard | F8 toast | F9/F10 taskbar progress");
    return app->exec();
}
