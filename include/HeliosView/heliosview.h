#ifndef HELIOSVIEW_HELIOSVIEW_H
#define HELIOSVIEW_HELIOSVIEW_H

/**
 * HeliosView C API (the only external interface of HeliosView.dll) -- umbrella header.
 *
 * This header is a pure C interface that guarantees a stable ABI:
 *   - Uses only C-compatible types (POD); no C++ objects/exceptions cross the DLL boundary
 *   - All functions use extern "C" linkage
 *   - Errors are reported via return values/error codes, never exceptions
 *
 * =========================== Layout ===========================
 *
 * The API is split by subsystem, and this file only declares what is shared and
 * includes the rest. Include it for the whole API, or include one part directly when
 * a translation unit only needs that part (a drawing-only file does not have to see
 * the windowing API, a console tool does not have to see the WebView):
 *
 *   heliosview_base.h          error codes, heliosview_rect_t, heliosview_window_t
 *   heliosview_core.h          version, error reporting, allocator, string codecs
 *   heliosview_event.h         event/keycode enums, event queue, native-message hooks
 *   heliosview_loop.h          message loop + loop timers
 *   heliosview_app.h           app identity, activation policy, session end, icons
 *   heliosview_screen.h        screen / monitor geometry
 *   heliosview_window.h        top-level windows (+ taskbar progress, backdrop)
 *   heliosview_tray.h          system tray icon
 *   heliosview_menu.h          actions, popup menus, the application menu bar
 *   heliosview_webview.h       embedded WebView (Windows: WebView2) + JS bridge
 *   heliosview_dialogs.h       folder / file pickers, message box
 *   heliosview_system.h        clipboard, open URL/path, run program, hotkeys, OS info
 *   heliosview_notification.h  OS toast notifications
 *   heliosview_canvas.h        drawing: canvas, painter, paths, images (rendering engines)
 *
 * Every part includes heliosview_base.h itself, so the parts may be included in any
 * order and on their own.
 *
 * =========================== Platform independence ===========================
 *
 * Platform details (Win32 messages, HWND, HDC, GDI+ objects, ...) do not appear in
 * this interface:
 *   - Native messages are passed to the registered conversion delegate as opaque
 *     void* pointers, valid only during the callback
 *   - Windows are opaque handles of type heliosview_window_t*
 *   - A canvas is a block of pixels with a documented layout; the rendering engine
 *     behind it is chosen by an enum, never by naming a platform type
 *   - An event's window_id is the platform's native window handle, as a
 *     pointer-sized integer (uintptr_t; 0 = none) — the HWND on Windows. It is
 *     valid only while the window exists; see heliosview_window_from_id.
 *   - Keycodes and mouse buttons are uniformly mapped to platform-independent enums
 *
 * Strings: every string parameter and result is UTF-8 (const char*). The two-phase
 * codecs below (heliosview_utf8_to_wide / wide_to_utf8) convert between UTF-8 and
 * wchar_t for consumers that must interop with a platform's wide-char APIs.
 *
 * =========================== Event model ===========================
 *
 *   - heliosview_run runs the message loop, converting native messages via a
 *     (registerable) conversion delegate into queued heliosview_event_t events
 *   - heliosview_poll / heliosview_wait dequeue events from the queue
 *   - heliosview_post_event posts events from any thread
 *
 * =========================== Threading model ===========================
 *
 *   All window / WebView / tray / menu / dialog / event-queue APIs must be called
 *   on the message-loop thread -- the thread running heliosview_run (in C++, the
 *   App::exec thread). Calling them from another thread is undefined behavior.
 *
 *   The exceptions (safe from any thread):
 *     - heliosview_post_event / heliosview_wake_loop / heliosview_quit
 *     - heliosview_notification_init / _show (OS toasts are thread-agnostic)
 *     - heliosview_free (and the allocator, set before any other call)
 *     - the canvas functions in heliosview_canvas.h (a canvas is memory, not a
 *       window: any one thread at a time, never two at once)
 *
 *   To return to the message-loop thread from a worker thread, post an event
 *   (heliosview_post_event) or wake the loop (heliosview_wake_loop); the C++
 *   wrapper provides App::postTask for this.
 *
 *   Platform note: on Windows the message-loop thread may be any thread. On
 *   macOS and Linux it must be the process's MAIN thread — AppKit (NSApplication)
 *   and GTK both require UI work there — so a portable program runs its loop on
 *   main() and hands work to other threads, never the other way round.
 *
 * =========================== Coordinates ===========================
 *
 * Every screen coordinate and size in this API uses the same convention as Win32:
 * origin at the TOP-LEFT of the primary display, x right, y down, in
 * virtual-desktop logical units (DPI-independent pixels/points).
 * Backends whose native origin differs (macOS: bottom-left, y up) convert
 * internally; portable code never sees the difference. The scale of one unit is
 * reported by heliosview_window_scale_factor().
 *
 * A canvas (heliosview_canvas.h) uses the same top-left / x-right / y-down
 * convention, in its own pixel space.
 *
 * Wide characters: wchar_t is UTF-16 on Windows and UTF-32 on macOS/Linux, so
 * heliosview_utf8_to_wide / heliosview_wide_to_utf8 convert to and from the
 * PLATFORM's wchar_t — not to UTF-16 specifically. Prefer the UTF-8 API and use
 * these codecs only to interop with a platform's wide-char functions.
 *
 * =========================== Error reporting ===========================
 *
 * Return convention (every function): 0 = success, < 0 = an error code,
 * > 0 = a payload / count (e.g. number of items or characters).
 *
 * Standard error codes (defined in heliosview_base.h):
 *     0   HELIOSVIEW_SUCCESS: operation completed successfully
 *    -1   HELIOSVIEW_ERROR_GENERIC: generic failure — invalid/missing argument,
 *         underlying platform call failed with no specific code, or called on the wrong thread
 *    -2   HELIOSVIEW_ERROR_INVALID_ARGUMENT: invalid argument or name (e.g. not a valid C identifier)
 *    -3   HELIOSVIEW_WEBVIEW_DESTROYED: the WebView instance was already destroyed
 *    -4   HELIOSVIEW_ERROR_UNSUPPORTED: the platform or OS version cannot provide this
 *         feature (e.g. a Mica backdrop on Windows 10, any Windows-only feature on
 *         another platform, a canvas engine this build does not provide). Safe to
 *         ignore: callers degrade to their fallback.
 *
 * Other error codes are platform codes:
 *   - a small negative value (e.g. -5) is the negated OS error status (Win32
 *     GetLastError);
 *   - a large positive value (e.g. 2147467259) is the negated HRESULT as returned
 *     by COM / WebView2 / DWM failures (E_FAIL 0x80004005 surfaces as 2147467259).
 * A negated platform code can collide numerically with the reserved codes above;
 * read heliosview_last_error_string for the actual reason.
 *
 * Async completion callbacks (error != 0) use the same code space.
 *
 * For the descriptive reason behind the most recent failure on this thread, use
 * heliosview_last_error / heliosview_last_error_string — every failing call
 * records a failure-site message.
 * Note: heliosview_wait / heliosview_poll and dialog functions return small
 * tri-state status codes (1/0) documented per function.
 *
 * C++ users should include <HeliosViewCore/HeliosView.h> (the HeliosView.Core wrapper).
 */

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include <HeliosView/heliosview_base.h> /* error codes, heliosview_rect_t, heliosview_window_t */
#include <HeliosView/heliosview_export.h>

/* The API, by subsystem (see "Layout" above). The order is documentation only: the
 * parts are self-contained and include-guard safe. */
#include <HeliosView/heliosview_core.h>
#include <HeliosView/heliosview_event.h>
#include <HeliosView/heliosview_loop.h>
#include <HeliosView/heliosview_app.h>
#include <HeliosView/heliosview_screen.h>
#include <HeliosView/heliosview_window.h>
#include <HeliosView/heliosview_tray.h>
#include <HeliosView/heliosview_menu.h>
#include <HeliosView/heliosview_webview.h>
#include <HeliosView/heliosview_dialogs.h>
#include <HeliosView/heliosview_system.h>
#include <HeliosView/heliosview_notification.h>
#include <HeliosView/heliosview_canvas.h>

#endif /* HELIOSVIEW_HELIOSVIEW_H */
