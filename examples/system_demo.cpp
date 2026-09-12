// HeliosView.Core example: OS integration - dialogs, clipboard, notifications,
// global hotkeys, standard folders and process launching.
//
// The "system services" demo: everything a desktop app needs from the OS that is
// not a window or a WebView. All of it is a one-line call on the message-loop
// thread (notifications are the exception: they are safe from any thread).
//
// Controls:
//   Esc   quit
//   F1    folder picker        (selectFolder)          F2  open files (openFiles)
//   F3    save file            (saveFile)              F4  message box (messageBox)
//   F5    clipboard round-trip (clipboardSet/GetText)  F6  OS toast (notificationShow)
//   F7    open a URL in the browser                    F8  reveal a folder in Explorer
//   F9    register/unregister a global hotkey          F10 system + screen info
//   F11   launch a detached process (runProgram)
//
// The global hotkey (Ctrl+Alt+H) fires even while the app is in the background.
// A session-end callback (shutdown / logoff) is registered at startup.
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/* Notification click callbacks run on an unspecified thread, so they must not
 * touch windows/menus directly - App::postTask marshals back to the loop thread. */
void onNotificationClicked(const char* title, const char* body)
{
    std::printf("[toast] clicked: '%s' - '%s'\n", title ? title : "", body ? body : "");
}

void onHotkey(uint32_t id, void* userdata)
{
    (void)id;
    (void)userdata;
    /* Fires even while the app has no focus - that is the point of a global hotkey. */
    std::printf("[hotkey] Ctrl+Alt+H fired (works while the app is in the background)\n");
}

/* Return 0 to allow the shutdown, non-zero to veto it. Runs synchronously on the
 * message-loop thread before the session ends, so it can save state here. */
int onSessionEnd(void* userdata)
{
    (void)userdata;
    std::printf("[session] the OS session is ending - saving state (returning 0 = allow)\n");
    return 0;
}

void printSystemInfo()
{
    std::string os;
    if (helios::osVersion(os))
        std::printf("[info] os: %s\n", os.c_str());
    std::printf("[info] backend: %s\n", heliosview_backend_name());

    const std::pair<const char*, heliosview_system_path_kind_t> folders[] = {
        {"home", HELIOSVIEW_SYSTEM_PATH_HOME},
        {"documents", HELIOSVIEW_SYSTEM_PATH_DOCUMENTS},
        {"downloads", HELIOSVIEW_SYSTEM_PATH_DOWNLOADS},
        {"appdata", HELIOSVIEW_SYSTEM_PATH_APPDATA},
        {"cache", HELIOSVIEW_SYSTEM_PATH_CACHE},
        {"temp", HELIOSVIEW_SYSTEM_PATH_TEMP},
    };
    for (const auto& [name, kind] : folders) {
        std::string path;
        if (helios::systemPath(kind, path))
            std::printf("[info] %-10s %s\n", name, path.c_str());
    }

    helios::Rect primary{};
    if (helios::primaryWorkArea(primary))
        std::printf("[info] primary work area: %d,%d %dx%d\n", primary.x, primary.y,
                    primary.width, primary.height);
    int32_t x = 0, y = 0;
    if (helios::cursorPosition(x, y))
        std::printf("[info] cursor at %d,%d\n", x, y);
}

} // namespace

int main()
{
    std::printf("HeliosView %s - system services demo\n", helios::version().c_str());

    /* Process identity: on Windows the AppUserModelID is what makes OS toasts and
     * taskbar grouping work; set it before the first window. */
    helios::App::setAppId("com.example.heliosview.systemdemo");

    helios::App app;

    /* ---------- notifications: init once, then use from anywhere ---------- */
    std::printf("[toast] backend: %s\n",
                helios::notificationInit() ? "ready" : "unavailable (unpackaged app?)");
    helios::notificationRequestPermission([](helios::NotificationPermission permission) {
        std::printf("[toast] permission: %d\n", static_cast<int>(permission));
    });
    helios::notificationSetClickCallback(&onNotificationClicked);

    /* The OS asks the app to shut down (Windows session end): save state here. */
    helios::setSessionEndCallback(&onSessionEnd);

    helios::Window window(640, 420, "HeliosView System Demo");
    window.show();

    /* The hotkey callback receives the userdata pointer, not a lambda. */
    uint32_t hotkey = 0;

    window.keyPressed.connect([&](helios::KeyCode key) {
        switch (key) {
        case helios::KeyCode::Escape:
            window.close();
            break;
        case helios::KeyCode::F1: {
            std::string path;
            if (helios::selectFolder(window.nativeHandle(), "Pick a folder", path))
                std::printf("[dialog] folder: %s\n", path.c_str());
            else
                std::printf("[dialog] cancelled\n");
            break;
        }
        case helios::KeyCode::F2: {
            const auto files = helios::openFiles(
                window.nativeHandle(), "Pick images",
                std::vector<helios::FileFilter>{{"Images", "png;jpg;jpeg"}, {"All files", "*.*"}},
                /*multi=*/true);
            std::printf("[dialog] %zu file(s)\n", files.size());
            for (const auto& f : files)
                std::printf("[dialog]   %s\n", f.c_str());
            break;
        }
        case helios::KeyCode::F3: {
            std::string path;
            if (helios::saveFile(window.nativeHandle(), "Save as",
                                 std::vector<helios::FileFilter>{{"Text files", "txt"}},
                                 "out.txt", path))
                std::printf("[dialog] save to: %s\n", path.c_str());
            break;
        }
        case helios::KeyCode::F4:
            std::printf("[dialog] message box -> %d\n",
                        static_cast<int>(helios::messageBox(
                            window.nativeHandle(), helios::MessageBoxType::Info,
                            helios::MessageBoxButtons::Ok, "HeliosView",
                            "Hello from HeliosView")));
            break;
        case helios::KeyCode::F5: {
            helios::clipboardSetText("HeliosView clipboard round-trip");
            std::string text;
            std::printf("[clipboard] read back: %s\n",
                        helios::clipboardGetText(text) ? text.c_str() : "(no text)");
            break;
        }
        case helios::KeyCode::F6:
            std::printf("[toast] shown: %s\n",
                        helios::notificationShow("HeliosView", "System toast - click me") ? "yes"
                                                                                          : "no");
            break;
        case helios::KeyCode::F7:
            std::printf("[system] openUrl -> %d\n", helios::openUrl("https://example.com"));
            break;
        case helios::KeyCode::F8:
            std::printf("[system] showInFolder -> %d\n", helios::showInFolder("C:\\Windows"));
            break;
        case helios::KeyCode::F9:
            if (hotkey) {
                helios::hotkeyUnregister(hotkey);
                std::printf("[hotkey] unregistered Ctrl+Alt+H\n");
                hotkey = 0;
            } else if (helios::hotkeyRegister("Ctrl+Alt+H", &onHotkey, &window, hotkey)) {
                std::printf("[hotkey] registered Ctrl+Alt+H (id %u) - works while unfocused\n",
                            hotkey);
            } else {
                std::printf("[hotkey] registration failed (already taken?)\n");
            }
            break;
        case helios::KeyCode::F10:
            printSystemInfo();
            break;
        case helios::KeyCode::F11:
            std::printf("[system] runProgram(notepad) -> %d\n",
                        helios::runProgram("notepad.exe", "HeliosView demo"));
            break;
        default:
            break;
        }
    });

    /* The close button does NOT auto-close; the app decides what to do. */
    window.closeRequested.connect([&window] {
        std::printf("[demo] close requested\n");
        window.close();
    });

    std::printf("[demo] Esc quit | F1 folder | F2 files | F3 save | F4 message box | "
                "F5 clipboard | F6 toast | F7 open URL | F8 show in folder | "
                "F9 global hotkey | F10 system info | F11 run program\n");
    const int rc = app.exec();

    if (hotkey) /* F9 may have toggled it while the loop ran */
        helios::hotkeyUnregister(hotkey);
    return rc;
}
