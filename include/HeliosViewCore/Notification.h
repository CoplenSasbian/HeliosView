#pragma once

/**
 * HeliosView.Core -- Notification: OS toast notifications.
 *
 * Unlike every other HeliosView API, the notification functions are
 * thread-agnostic: they may be called from any thread (e.g. a worker thread
 * reporting that a background task finished). Their callbacks run on an
 * unspecified thread, so marshal back to the loop thread (App::postTask /
 * heliosview_wake_loop) before touching windows or menus.
 *
 *   helios::notificationInit();                        // once, at startup
 *   helios::notificationRequestPermission();           // macOS shows a prompt
 *   helios::notificationShow("Done", "Download finished");   // any thread
 *
 * Registration is platform-specific but the call is the same: on Windows an
 * AppUserModelID plus the Start Menu shortcut carrying it (unpackaged apps need
 * both), on macOS the bundle identifier, on Linux the desktop id. The id comes
 * from notificationInit's argument, else App::setAppId, else the executable
 * name. macOS also needs the user's permission — check
 * notificationPermission() before reporting success.
 */

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/Types.h>

#include <functional>
#include <string>

namespace helios {

// Initialize the notification backend (registration + the platform's notifier).
// appId may be nullptr to use App::appId() or the executable name. Call once at
// startup; returns true on success. Thread-safe.
inline bool notificationInit(const char* appId = nullptr)
{
    return heliosview_notification_init(appId) == 0;
}

// Ask for notification permission (macOS: shows the system prompt the first
// time). The callback may run later, on an unspecified thread. Returns true when
// the request was accepted; notificationPermission() then reports the state.
inline bool notificationRequestPermission(std::function<void(NotificationPermission)> callback = {})
{
    struct Trampoline {
        static void invoke(heliosview_notification_permission_t permission, void* userdata)
        {
            auto* fn = static_cast<std::function<void(NotificationPermission)>*>(userdata);
            (*fn)(static_cast<NotificationPermission>(permission));
            delete fn;
        }
    };
    if (!callback)
        return heliosview_notification_request_permission(nullptr, nullptr) == 0;
    auto* fn = new std::function<void(NotificationPermission)>(std::move(callback));
    if (heliosview_notification_request_permission(&Trampoline::invoke, fn) != 0) {
        delete fn;
        return false;
    }
    return true;
}

// The last known permission state. Thread-safe.
inline NotificationPermission notificationPermission()
{
    return static_cast<NotificationPermission>(heliosview_notification_permission_state());
}

// Register (or clear, with an empty std::function) the callback for a click on a
// posted notification. Runs on an unspecified thread.
inline bool notificationSetClickCallback(std::function<void(const char* title, const char* body)> callback = {})
{
    struct Trampoline {
        static void invoke(const char* title, const char* body, void* userdata)
        {
            auto* fn = static_cast<std::function<void(const char*, const char*)>*>(userdata);
            (*fn)(title, body);
        }
    };
    static std::function<void(const char*, const char*)> s_callback; /* one process-wide callback */
    s_callback = std::move(callback);
    return heliosview_notification_set_click_callback(s_callback ? &Trampoline::invoke : nullptr,
                                                      s_callback ? &s_callback : nullptr) == 0;
}

// Show a toast with a title and body. Returns true on success. Thread-safe.
inline bool notificationShow(const char* title, const char* body)
{
    return heliosview_notification_show(title, body) == 0;
}

inline bool notificationShow(const std::string& title, const std::string& body)
{
    return heliosview_notification_show(title.c_str(), body.c_str()) == 0;
}

} // namespace helios
