#pragma once

/**
 * HeliosView.Core -- Notification: OS toast notifications.
 *
 * Unlike every other HeliosView API, the notification functions are
 * thread-agnostic: they may be called from any thread (e.g. a worker thread
 * reporting that a background task finished).
 *
 *   helios::notificationInit();                    // once, at startup
 *   helios::notificationShow("Done", "Download finished");  // any thread
 *
 * On win32, unpackaged apps must register an AppUserModelID + a Start Menu
 * shortcut; notificationInit() does both (the id defaults to the exe name).
 */

#include <HeliosView/heliosview.h>

#include <string>

namespace helios {

// Initialize the notification backend (AppUserModelID + Start Menu shortcut).
// appUserModelId may be nullptr to derive one from the exe name. Call once at
// startup; returns true on success. Thread-safe.
inline bool notificationInit(const char* appUserModelId = nullptr)
{
    return heliosview_notification_init(appUserModelId) == 0;
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
