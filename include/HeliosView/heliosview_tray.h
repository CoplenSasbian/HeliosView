#ifndef HELIOSVIEW_HELIOSVIEW_TRAY_H
#define HELIOSVIEW_HELIOSVIEW_TRAY_H

/**
 * HeliosView C API -- system tray icon
 *
 * The notification-area (tray) icon: tooltip, icon, attached menu and balloon
 * notifications.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>
#include <HeliosView/heliosview_menu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Tray icon (system tray notification icon) =================
 *
 * Shows an icon in the OS notification area (system tray / status bar / menu extras). Mouse events on the icon (single/double click, right/middle click) are delivered
 * as HELIOSVIEW_EVENT_TRAY_* events through the event queue.
 *
 * The tray icon is completely standalone and does not require an application window
 * to exist, enabling background-only applications that run purely in the tray.
 *
 * The icon is loaded from an icon file path (UTF-8, see Icons); pass NULL to use
 * the default application icon. Destroy the tray with heliosview_tray_destroy.
 *
 * Menus: attach a menu with heliosview_tray_set_menu so the shell can open it —
 * this is the ONLY way a tray menu can work on Linux (StatusNotifierItem exports
 * the menu over DBus; the application cannot pop one up itself) and the idiomatic
 * way on macOS (an NSStatusItem menu). With a menu attached, opening it is the
 * shell's job on those platforms: TRAY_RIGHT_CLICK may not be delivered at all,
 * and on macOS any click opens the menu, so TRAY_LEFT_CLICK may not be delivered
 * either. On Windows the library opens the menu on right-click and then does not
 * emit TRAY_RIGHT_CLICK (left/middle clicks still emit their events). Code that
 * needs the click itself should leave the menu unattached and call
 * heliosview_menu_show from the event handler — which only works on Windows and
 * macOS.
 */

typedef struct heliosview_tray heliosview_tray_t;

/* The tray can own a context menu (heliosview_tray_set_menu); heliosview_menu_t is
 * declared in heliosview_base.h. */

/* Create and show a standalone tray icon with the given tooltip (UTF-8) and
 * icon file path (NULL = default icon). `userdata` is caller data (e.g. a C++
 * Tray object) copied verbatim into the TRAY_* events this tray produces.
 * The tray does not require an application window to exist. Returns NULL on failure. */
HELIOSVIEW_API heliosview_tray_t* heliosview_tray_create(const char* tooltip,
                                                         const char* icon_path,
                                                         void* userdata);

/* Update the tray tooltip. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_tray_set_tooltip(heliosview_tray_t* tray, const char* tooltip);

/* Replace the tray icon, loaded from an icon file path (NULL = default icon).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_tray_set_icon(heliosview_tray_t* tray, const char* icon_path);

/* Same, with heliosview_icon_flag_t bits (0 = none) — use
 * HELIOSVIEW_ICON_FLAG_TEMPLATE for a macOS status-bar icon. */
HELIOSVIEW_API int heliosview_tray_set_icon_ex(heliosview_tray_t* tray, const char* icon_path,
                                               uint32_t flags);

/* Attach (or detach, with NULL) the tray's context menu — a POPUP menu
 * (heliosview_menu_create), not a menu bar. The tray keeps a
 * reference to the menu until it is replaced or the tray is destroyed, so the
 * menu must not be freed while it is attached (heliosview_menu_destroy only
 * drops the caller's reference; the attached menu stays alive). Menu actions
 * fire as usual. See the platform notes above. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_tray_set_menu(heliosview_tray_t* tray, heliosview_menu_t* menu);

/* Remove the tray icon and free the tray handle. The icon is NOT
 * removed automatically when the owning window is destroyed — always destroy
 * the tray before its window. */
HELIOSVIEW_API void heliosview_tray_destroy(heliosview_tray_t* tray);

/* ================= Tray balloon notification =================
 *
 * A classic balloon popup next to the tray icon. Unlike the toast API (which
 * requires an AppUserModelID + Start Menu shortcut, see Notification below),
 * a balloon always works, needs no setup, and is tied to this tray. Message-loop
 * thread. */

typedef enum heliosview_tray_notify_icon {
    HELIOSVIEW_TRAY_NOTIFY_NONE = 0,
    HELIOSVIEW_TRAY_NOTIFY_INFO,
    HELIOSVIEW_TRAY_NOTIFY_WARNING,
    HELIOSVIEW_TRAY_NOTIFY_ERROR,
} heliosview_tray_notify_icon_t;

/* Show a balloon (title/message are UTF-8; timeout_ms in milliseconds, 0 = default). 0 = success */
HELIOSVIEW_API int heliosview_tray_notify(heliosview_tray_t* tray, const char* title,
                                          const char* message,
                                          heliosview_tray_notify_icon_t icon_type,
                                          uint32_t timeout_ms);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_TRAY_H */
