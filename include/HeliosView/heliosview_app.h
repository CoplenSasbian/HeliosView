#ifndef HELIOSVIEW_HELIOSVIEW_APP_H
#define HELIOSVIEW_HELIOSVIEW_APP_H

/**
 * HeliosView C API -- application identity, activation policy, session end, icons
 *
 * Process-level concerns: the application id and activation policy, the OS
 * session-end notification (save on shutdown / logoff), and the icon flags used by
 * windows and trays.
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

/* ================= Application (identity + activation policy) =================
 *
 * Process-wide settings that a backend needs BEFORE the first window/tray is
 * created. Both are optional: a program that never calls them behaves as
 * heliosview_set_activation_policy(HELIOSVIEW_ACTIVATION_REGULAR) with an empty
 * app id. Call them first thing in main(), before creating any window, menu or
 * tray; calling them later is allowed but a policy change may not be applied
 * retroactively by every backend.
 */

/* How the process presents itself to the OS:
 *   REGULAR    a normal GUI application (default): Dock icon on macOS, taskbar
 *              presence on Windows/Linux.
 *   ACCESSORY  no Dock/taskbar icon of its own — the normal choice for a
 *              tray-only or menu-bar-only application. macOS requires this or
 *              the app shows a Dock icon with no way to reopen a window.
 *   PROHIBITED never becomes the active/front application (background agent). */
typedef enum heliosview_activation_policy {
    HELIOSVIEW_ACTIVATION_REGULAR = 0,
    HELIOSVIEW_ACTIVATION_ACCESSORY,
    HELIOSVIEW_ACTIVATION_PROHIBITED,
} heliosview_activation_policy_t;

/* Set the process's application id (UTF-8): Windows AppUserModelID, macOS
 * bundle identifier, Linux desktop/application id. NULL or "" clears it. The id
 * is stored by the core and applied by the backend where it matters (toast
 * notifications, taskbar grouping, desktop integration); it also becomes the
 * default used by heliosview_notification_init(NULL). Returns 0 on success. */
HELIOSVIEW_API int heliosview_app_init(const char* app_id);

/* The application id set by heliosview_app_init ("" when none). The pointer is
 * owned by the library and stays valid until the next heliosview_app_init. */
HELIOSVIEW_API const char* heliosview_app_id(void);

/* Set the activation policy. Returns 0 on success, HELIOSVIEW_ERROR_UNSUPPORTED
 * when the platform has no equivalent concept (the value is still stored and
 * reported by heliosview_activation_policy). */
HELIOSVIEW_API int heliosview_set_activation_policy(heliosview_activation_policy_t policy);

/* The current activation policy (REGULAR when never set). */
HELIOSVIEW_API heliosview_activation_policy_t heliosview_activation_policy(void);

/* ================= Icons =================
 *
 * An icon is identified by a file path (UTF-8) and loaded by the backend from
 * whichever format the platform understands:
 *     Windows  .ico, .cur (also .png/.bmp/.jpg via the imaging path)
 *     macOS    .icns, .png, .pdf (vector)
 *     Linux    .png, .svg (and .xpm)
 * Portable code ships one file per platform next to the executable and picks it
 * by extension; a path the backend cannot load makes the call fail (negative
 * return) and the previous icon is kept. NULL/"" restores the platform default.
 */
typedef enum heliosview_icon_flag {
    HELIOSVIEW_ICON_FLAG_NONE = 0,
    /* macOS: treat the image as a template (monochrome mask) so the system
     * recolors it for light/dark menu bars and highlight states. Required for a
     * correct status-bar icon; ignored elsewhere. The same effect can be
     * achieved without this flag by naming the file "<name>Template.png". */
    HELIOSVIEW_ICON_FLAG_TEMPLATE = 1u << 0,
} heliosview_icon_flag_t;


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_APP_H */
