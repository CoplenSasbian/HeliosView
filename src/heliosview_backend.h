#pragma once

/**
 * HeliosView backend contract.
 *
 * The public C API (include/HeliosView/heliosview.h) is platform independent; a
 * backend implements it for one OS. Today there is one real backend
 * (src/win32/) and one stub (src/portable/heliosview_portable_stub.cpp) that
 * reports every feature as unsupported, so the library still builds, links and
 * runs its conformance tests on platforms that have no backend yet.
 *
 * =========================== Adding a backend ===========================
 *
 * 1. Create src/<os>/ and implement the public API. The linker is the
 *    checklist: every HELIOSVIEW_API function needs exactly one definition.
 *    For anything the OS cannot provide, return HELIOSVIEW_ERROR_UNSUPPORTED
 *    (-4) and record a message with hv_fail() so heliosview_last_error_string()
 *    explains why; never report success for a feature you did not apply.
 * 2. Implement the two backend entry points declared below.
 * 3. Register every window with hv_register_window() once its native handle
 *    exists and hv_unregister_window() before destroying it, so the core can
 *    provide heliosview_window_count() / heliosview_window_from_id().
 * 4. Add the sources to src/CMakeLists.txt and the OS system libraries to
 *    cmake/PlatformLibs.cmake. Nothing else changes.
 *
 * ==================== Core services a backend may use ====================
 *
 *   hv::queue_push(event)   enqueue an event on the message-loop thread (fills
 *                           timestamp_ms when 0 and wakes the loop)
 *   hv::now_ms()            monotonic milliseconds since library initialization
 *   hv::hv_alloc<T>(...), hv::hv_dealloc(p)   allocate through the caller's
 *                           configured allocator (heliosview_set_allocator)
 *   hv_fail(code, msg)      record (code, message) as this thread's last error
 *                           and return `code` — one-line failure sites
 *   hv_fail_win32 / hv_fail_hresult (win32)   the same, with the platform's own
 *                           message appended
 *   hv_register_window(id, w) / hv_unregister_window(id) / hv_window_count()
 *   hv::g_platform_wake     set to a function that wakes the message loop
 *                           (win32: SetEvent) so heliosview_wake_loop works
 *   hv::g_app_id / hv::g_activation_policy   the process identity and activation
 *                           policy the core stores for heliosview_app_init /
 *                           heliosview_set_activation_policy; a backend applies
 *                           them when it creates its application object
 *                           (macOS: NSApplicationActivationPolicy from the
 *                           policy; Linux: the desktop/application id)
 *
 * ==================== Threading rules ===========================
 *
 * - Windows are created, used and destroyed on the message-loop thread; the
 *   event queue and the window registry are thread-local by design.
 * - heliosview_post_event / heliosview_quit / heliosview_wake_loop may be
 *   called from any thread.
 * - Native filters are registered and dispatched on the message-loop thread
 *   (see heliosview.h).
 * - On macOS and Linux the message-loop thread must be the process's MAIN
 *   thread (AppKit/GTK require UI work there). heliosview_run must therefore
 *   run the main thread's loop; a backend should fail loudly (hv_fail) rather
 *   than silently misbehave when UI is touched from another thread.
 *
 * ==================== Notes for a new backend ===========================
 *
 * - Window chrome: heliosview_window_flag_t refines the preset style. macOS
 *   maps TITLEBAR_HIDDEN/TITLEBAR_TRANSPARENT/FULL_SIZE_CONTENT onto the
 *   NSWindow styleMask / titlebarAppearsTransparent / fullSizeContentView, which
 *   is the idiomatic look there — do not make users fall back to a borderless
 *   window with hand-drawn buttons.
 * - Tray menus: heliosview_tray_set_menu attaches a menu whose lifetime the tray
 *   shares with the caller (heliosview_menu_destroy only drops the caller's
 *   reference). Linux must export it over DBus (com.canonical.dbusmenu);
 *   macOS assigns it to the NSStatusItem. heliosview_menu_show is not available
 *   for tray menus on those platforms — the shell opens the menu.
 * - Menus are reference-counted: a menu displayed by a parent menu, the
 *   application menu bar or a tray is kept alive by that holder. A backend must
 *   implement the same retain/release discipline (see the win32 menu backend).
 * - Menu kinds: heliosview_menu_create makes a POPUP (Win32: CreatePopupMenu,
 *   GTK: GtkMenu, macOS: any NSMenu) — shown with heliosview_menu_show,
 *   attached to a tray, added as a submenu; heliosview_menu_create_bar makes
 *   the MENU BAR (Win32: CreateMenu, GTK: GtkMenuBar, macOS: the NSMenu
 *   installed as NSApp.mainMenu). The kind is a real per-platform difference,
 *   and a backend must REJECT the cross-kind uses its platform cannot honor (a
 *   popup as the application menu bar; a bar as a submenu or a shown popup),
 *   mirroring the win32 validations in heliosview_menu_set_app_menu /
 *   heliosview_menu_add_submenu.
 * - Coordinates: the portable contract is top-left origin, y down, virtual
 *   desktop units. Convert to the native origin inside the backend.
 * - Notifications: heliosview_notification_request_permission must be honored
 *   (macOS shows a prompt; Windows/Linux report the current setting), and the
 *   click callback must be routed back to the application.
 */
#include "heliosview_internal.h"

/* ================= Backend entry points =================
 * Exactly one definition per backend (the core calls both). */

/* Short backend name: "win32", "macos", "linux", "portable". Reported by the
 * public heliosview_backend_name() and used by tests to print what ran. */
const char* hv_backend_name();

/* Is `window_id` still one of this backend's live native windows? The core asks
 * this in heliosview_window_from_id() before handing out a registered pointer,
 * so a window destroyed behind the library's back can never yield a stale
 * pointer. Backends that cannot validate cheaply may return true (the registry
 * is still authoritative); the stub returns false. */
bool hv_backend_window_alive(uintptr_t window_id);
