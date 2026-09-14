#ifndef HELIOSVIEW_HELIOSVIEW_NOTIFICATION_H
#define HELIOSVIEW_HELIOSVIEW_NOTIFICATION_H

/**
 * HeliosView C API -- OS toast notifications
 *
 * Native toast notifications: permission handling, the click callback, and showing
 * a toast. Unlike the windowing API these calls are thread-safe.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Notifications (OS toast) =================
 *
 * Modern OS notifications (Win32: Windows toast; macOS: UserNotifications;
 * Linux: org.freedesktop.Notifications). Unlike every other API in this header,
 * these functions are thread-agnostic: they may be called from any thread (e.g. a
 * worker reporting that a background task finished). Their callbacks, however,
 * run on an unspecified thread (see below) — never assume the message loop.
 *
 * Setup: heliosview_notification_init registers the application id (see
 * heliosview_app_init; NULL = the id set there, else one derived from the
 * executable name) and performs the platform's registration — on Windows an
 * AppUserModelID plus the Start Menu shortcut carrying it (unpackaged Win32 apps
 * need both for toasts). Call it once, typically at startup; it is safe to call
 * again later.
 *
 * Permission: macOS shows a system prompt the first time an application asks to
 * post notifications and silently drops toasts until the user allows them, so a
 * portable program calls heliosview_notification_request_permission once at
 * startup and checks the state before reporting success. Windows and Linux have
 * no prompt: the request reports the current OS setting immediately.
 */

/* Notification permission state. */
typedef enum heliosview_notification_permission {
    HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN = 0, /* not initialized / not yet asked */
    HELIOSVIEW_NOTIFICATION_PERMISSION_GRANTED,     /* toasts will be shown */
    HELIOSVIEW_NOTIFICATION_PERMISSION_DENIED,      /* the user or the OS turned them off */
} heliosview_notification_permission_t;

/* Permission result callback: `permission` is the resulting state, `userdata`
 * the value passed to the request. Runs on an unspecified thread (macOS answers
 * on a background queue) — do not touch windows/menus from it; use
 * heliosview_post_event / heliosview_wake_loop to get back to the loop thread. */
typedef void (*heliosview_notification_permission_cb)(heliosview_notification_permission_t permission,
                                                      void* userdata);

/* Ask for notification permission. On macOS this shows the system prompt (once;
 * afterwards the stored answer is reported) and the callback may run later; on
 * Windows/Linux it reports the current OS setting and may call back before
 * returning. `callback` may be NULL to only trigger the prompt. Returns 0 when
 * the request was accepted, negative when the backend cannot ask (call
 * heliosview_notification_init first). Thread-safe.
 *
 * Windows note: the first run of a freshly registered application id reports
 * UNKNOWN for the whole run (the OS picks the registration up asynchronously);
 * UNKNOWN is not a denial — poll the state or ask again on a later run. */
HELIOSVIEW_API int heliosview_notification_request_permission(
    heliosview_notification_permission_cb callback, void* userdata);

/* The last known permission state (UNKNOWN before init or before the first
 * request). Thread-safe. */
HELIOSVIEW_API heliosview_notification_permission_t heliosview_notification_permission_state(void);

/* Callback for a user click on a posted notification (title/body as posted,
 * `userdata` as passed below). Runs on an unspecified thread — marshal back to
 * the loop thread before touching windows. NULL clears the callback. */
typedef void (*heliosview_notification_click_cb)(const char* title, const char* body, void* userdata);

/* Initialize the notification backend. Returns 0 on success, negative on failure
 * (e.g. no notification service on this platform). Thread-safe (first call
 * initializes). */
HELIOSVIEW_API int heliosview_notification_init(const char* app_id);

/* Register (or clear, with NULL) the click callback for notifications posted by
 * heliosview_notification_show. 0 = success, negative = error (e.g. unsupported).
 * Thread-safe. */
HELIOSVIEW_API int heliosview_notification_set_click_callback(heliosview_notification_click_cb callback,
                                                              void* userdata);

/* Show a toast with a title and body. Returns 0 on success, negative on failure
 * (e.g. not initialized, permission denied, or toasts unavailable). Thread-safe. */
HELIOSVIEW_API int heliosview_notification_show(const char* title, const char* body);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_NOTIFICATION_H */
