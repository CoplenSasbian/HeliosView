#ifndef HELIOSVIEW_HELIOSVIEW_H
#define HELIOSVIEW_HELIOSVIEW_H

/**
 * HeliosView C API (the only external interface of HeliosView.dll).
 *
 * This header is a pure C interface that guarantees a stable ABI:
 *   - Uses only C-compatible types (POD); no C++ objects/exceptions cross the DLL boundary
 *   - All functions use extern "C" linkage
 *   - Errors are reported via return values/error codes, never exceptions
 *
 * Platform details (Win32 messages, HWND, etc.) do not appear in this interface:
 *   - Native messages are passed to the registered conversion delegate as opaque
 *     void* pointers, valid only during the callback
 *   - Windows are opaque handles of type heliosview_window_t*
 *   - Keycodes and mouse buttons are uniformly mapped to platform-independent enums
 *
 * Strings: every string parameter and result is UTF-8 (const char*). The two-phase
 * codecs below (heliosview_utf8_to_wide / wide_to_utf8) convert between UTF-8 and
 * wchar_t for consumers that must interop with a platform's wide-char APIs.
 *
 * Event model:
 *   - heliosview_run runs the message loop, converting native messages via a
 *     (registerable) conversion delegate into queued heliosview_event_t events
 *   - heliosview_poll / heliosview_wait dequeue events from the queue
 *   - heliosview_post_event posts events from any thread
 *
 * Threading model:
 *   All window / WebView / tray / menu / dialog / event-queue APIs must be called
 *   on the message-loop thread -- the thread running heliosview_run (in C++, the
 *   App::exec thread). Calling them from another thread is undefined behavior.
 *
 *   The exceptions (safe from any thread):
 *     - heliosview_post_event / heliosview_wake_loop / heliosview_quit
 *     - heliosview_webview_resolve / _reject / _broadcast (marshalled internally)
 *     - heliosview_notification_init / _show (OS toasts are thread-agnostic)
 *     - heliosview_free (and the allocator, set before any other call)
 *
 *   To return to the message-loop thread from a worker thread, post an event
 *   (heliosview_post_event) or wake the loop (heliosview_wake_loop); the C++
 *   wrapper provides App::postTask for this.
 *
 * C++ users should include <HeliosViewCore/HeliosView.h> (the HeliosView.Core wrapper).
 */

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Version ================= */

HELIOSVIEW_API const char* heliosview_version(void);

/* ================= Memory allocation =================
 *
 * The library allocates its internal objects (windows, webviews, WebView2
 * callback stubs, dialog results, ...) through a configurable allocator, so a
 * C app can supply its own memory management (e.g. a pool or arena) instead of
 * the process heap. Defaults to the standard allocator (malloc / free).
 *
 * Set it once, before any other library call. The allocator is read by
 * subsequent allocations; changing it while objects are alive is undefined
 * (memory must be freed with the same allocator that allocated it).
 */

typedef void* (*heliosview_alloc_fn)(size_t size, void* context);
typedef void  (*heliosview_free_fn)(void* ptr, void* context);

typedef struct heliosview_allocator {
    heliosview_alloc_fn alloc;  /* allocate `size` bytes, aligned for any object; NULL = malloc */
    heliosview_free_fn  free_;  /* free a pointer returned by `alloc`; NULL = free */
    void* context;              /* opaque, passed unchanged to alloc/free */
} heliosview_allocator_t;

/* Set the default allocator (NULL restores malloc/free). Not thread-safe while allocations are live. */
HELIOSVIEW_API void heliosview_set_allocator(const heliosview_allocator_t* allocator);

/* Free memory the library allocated (paths from the dialog APIs, clipboard text,
 * ...). Always pair a library-returned pointer with this, never the platform's
 * free(): the library may allocate through its configured allocator, and freeing
 * across CRT boundaries on Windows is undefined. NULL is ignored. Thread-safe. */
HELIOSVIEW_API void heliosview_free(void* ptr);

/* ================= String conversion (UTF-8 <-> UTF-16) =================
 *
 * Two-phase codecs for consumers that need to convert the library's UTF-8
 * strings to/from wchar_t (on Windows wchar_t is UTF-16; on platforms where
 * wchar_t is 32-bit the conversion is UTF-8 <-> wchar_t's native encoding).
 * The library itself stores UTF-8 and converts internally at platform boundaries.
 *
 * Call each function twice to convert without an intermediate buffer:
 *   size_t n = heliosview_utf8_to_wide(utf8, utf8_len, nullptr);  // required wchar_t count (incl. NUL), 0 = failure
 *   wchar_t buf[n];
 *   heliosview_utf8_to_wide(utf8, utf8_len, buf);                 // fills buf, NUL-terminated
 *
 * Input lengths are explicit; pass (size_t)-1 to read a NUL-terminated input.
 * Return value: with a NULL output, the required element count INCLUDING the
 * terminating NUL (0 = failure); with a non-NULL output, the element count
 * written EXCLUDING the NUL (the buffer must hold at least the previously
 * returned count). Invalid input sequences are replaced with U+FFFD.
 */

/* Convert UTF-8 bytes to wchar_t (see the two-phase contract above). */
HELIOSVIEW_API size_t heliosview_utf8_to_wide(const char* utf8, size_t utf8_len,
                                              wchar_t* out_wide);

/* Convert wchar_t to UTF-8 bytes (see the two-phase contract above). */
HELIOSVIEW_API size_t heliosview_wide_to_utf8(const wchar_t* wide, size_t wide_len,
                                              char* out_utf8);

/* ================= Events ================= */

typedef enum heliosview_event_type {
    HELIOSVIEW_EVENT_QUIT = 1,          /* Quit request (posted via heliosview_post_event) */
    HELIOSVIEW_EVENT_WINDOW_CLOSE,      /* Window close request (user clicked X) */
    HELIOSVIEW_EVENT_WINDOW_RESIZE,
    HELIOSVIEW_EVENT_WINDOW_FOCUS,      /* Window gained focus (activated) */
    HELIOSVIEW_EVENT_WINDOW_BLUR,       /* Window lost focus (deactivated) */
    HELIOSVIEW_EVENT_KEY_DOWN,
    HELIOSVIEW_EVENT_KEY_UP,
    HELIOSVIEW_EVENT_MOUSE_MOVE,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_UP,
    HELIOSVIEW_EVENT_TRAY_LEFT_CLICK,        /* tray icon left click */
    HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK, /* tray icon left double click */
    HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK,       /* tray icon right click (context menu) */
    HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK,      /* tray icon middle click */
    HELIOSVIEW_EVENT_MENU_SELECT             /* a menu item was chosen (menu_item = item id) */
} heliosview_event_type_t;

/* Platform-independent keycodes (native keycodes are mapped in the C layer) */
typedef enum heliosview_keycode {
    HELIOSVIEW_KEY_UNKNOWN = 0,
    HELIOSVIEW_KEY_ESCAPE,
    HELIOSVIEW_KEY_RETURN,
    HELIOSVIEW_KEY_SPACE,
    HELIOSVIEW_KEY_LEFT,
    HELIOSVIEW_KEY_RIGHT,
    HELIOSVIEW_KEY_UP,
    HELIOSVIEW_KEY_DOWN,
    HELIOSVIEW_KEY_0,
    HELIOSVIEW_KEY_1,
    HELIOSVIEW_KEY_2,
    HELIOSVIEW_KEY_3,
    HELIOSVIEW_KEY_4,
    HELIOSVIEW_KEY_5,
    HELIOSVIEW_KEY_6,
    HELIOSVIEW_KEY_7,
    HELIOSVIEW_KEY_8,
    HELIOSVIEW_KEY_9,
    HELIOSVIEW_KEY_A,
    HELIOSVIEW_KEY_B,
    HELIOSVIEW_KEY_C,
    HELIOSVIEW_KEY_D,
    HELIOSVIEW_KEY_E,
    HELIOSVIEW_KEY_F,
    HELIOSVIEW_KEY_G,
    HELIOSVIEW_KEY_H,
    HELIOSVIEW_KEY_I,
    HELIOSVIEW_KEY_J,
    HELIOSVIEW_KEY_K,
    HELIOSVIEW_KEY_L,
    HELIOSVIEW_KEY_M,
    HELIOSVIEW_KEY_N,
    HELIOSVIEW_KEY_O,
    HELIOSVIEW_KEY_P,
    HELIOSVIEW_KEY_Q,
    HELIOSVIEW_KEY_R,
    HELIOSVIEW_KEY_S,
    HELIOSVIEW_KEY_T,
    HELIOSVIEW_KEY_U,
    HELIOSVIEW_KEY_V,
    HELIOSVIEW_KEY_W,
    HELIOSVIEW_KEY_X,
    HELIOSVIEW_KEY_Y,
    HELIOSVIEW_KEY_Z,
    HELIOSVIEW_KEY_F1,
    HELIOSVIEW_KEY_F2,
    HELIOSVIEW_KEY_F3,
    HELIOSVIEW_KEY_F4,
    HELIOSVIEW_KEY_F5,
    HELIOSVIEW_KEY_F6,
    HELIOSVIEW_KEY_F7,
    HELIOSVIEW_KEY_F8,
    HELIOSVIEW_KEY_F9,
    HELIOSVIEW_KEY_F10,
    HELIOSVIEW_KEY_F11,
    HELIOSVIEW_KEY_F12
} heliosview_keycode_t;

typedef enum heliosview_mouse_button {
    HELIOSVIEW_MOUSE_LEFT = 1,
    HELIOSVIEW_MOUSE_RIGHT,
    HELIOSVIEW_MOUSE_MIDDLE
} heliosview_mouse_button_t;

/* Event: flat POD, safe to pass across the DLL boundary. The struct layout is
 * fixed for the 1.x series: new event data is added only at a major version. */
typedef struct heliosview_event {
    heliosview_event_type_t type;
    int32_t window_id;                       /* window id of the event's origin (0 = not window-related) */
    int64_t timestamp_ms;                    /* milliseconds since library initialization */
    int32_t x;                               /* mouse X (window client area) */
    int32_t y;                               /* mouse Y */
    int32_t width;                           /* window width (WINDOW_RESIZE) */
    int32_t height;                          /* window height (WINDOW_RESIZE) */
    heliosview_keycode_t key;                /* keycode (KEY_DOWN / KEY_UP) */
    heliosview_mouse_button_t mouse_button;  /* button (MOUSE_BUTTON_*) */
    uint32_t menu_item;                      /* menu item id (MENU_SELECT) */
    void* userdata;                          /* owning tray/menu userdata (TRAY_* / MENU_SELECT) */
} heliosview_event_t;

/* ================= Event queue =================
 *
 * Thread-local: the queue lives on the message-loop thread. Events are posted by
 * the native-message conversion (WndProc, same thread) and by heliosview_post_event
 * (message-loop thread) and consumed by heliosview_poll/wait on the same thread.
 * Cross-thread event or window access is not supported. */

/* Non-blocking fetch: 1 = event written to out, 0 = queue empty. Message-loop thread. */
HELIOSVIEW_API int heliosview_poll(heliosview_event_t* out_event);

/* Blocking fetch (polling, 1 ms granularity): 1 = event, -1 = quit request
 * (heliosview_quit), 0 = other error. Blocks the calling thread; must be called on
 * the message-loop thread (the queue is thread-local). */
HELIOSVIEW_API int heliosview_wait(heliosview_event_t* out_event);

/* Post an event to the calling thread's queue (message-loop thread). timestamp_ms is filled automatically when 0. */
HELIOSVIEW_API void heliosview_post_event(const heliosview_event_t* event);

/* Request to quit the message loop (heliosview_run returns) */
HELIOSVIEW_API void heliosview_quit(void);

/* Wake the message loop (posts no event): lets other threads notify the loop to
 * process pending work (e.g. scheduled tasks). No-op when no loop is running. */
HELIOSVIEW_API void heliosview_wake_loop(void);

/* ================= Native message -> event conversion ================= */

/* Conversion delegate: native_msg is a platform native message pointer, valid
 * only during the callback; on Windows it is const MSG* (callback runs on the
 * message-dispatch thread). Return value:
 *   1  -> converted to an event written to out_event (queued)
 *   0  -> consumed, not queued
 *  -1  -> not handled; the next converter is tried
 * The library's built-in conversion always runs first; registered converters are
 * then tried in registration order, and the first that returns 1 or 0 wins. If no
 * converter returns 1 or 0, the message falls through to the platform (DefWindowProc). */
typedef int (*heliosview_native_handler_fn)(void* native_msg, heliosview_event_t* out_event);

/* Register a converter (see above). Returns an id used by
 * heliosview_remove_native_handler (0 = failure, e.g. null handler). */
HELIOSVIEW_API uint32_t heliosview_add_native_handler(heliosview_native_handler_fn handler);

/* Remove a converter previously registered with heliosview_add_native_handler.
 * 0 = success, negative = not registered. */
HELIOSVIEW_API int heliosview_remove_native_handler(uint32_t id);

/* ================= Message loop ================= */

/* Called once after each pump drains the queue; return non-zero to exit the loop */
typedef int (*heliosview_loop_callback)(void* userdata);

/* Pump all pending native messages once and queue them as events (non-blocking) */
HELIOSVIEW_API void heliosview_pump_events(void);

/* Message loop: pump native messages, convert and queue, then call
 * frame_callback (application frame logic).
 * 0 = normal exit (heliosview_quit / WM_QUIT / callback returned non-zero) */
HELIOSVIEW_API int heliosview_run(heliosview_loop_callback frame_callback, void* userdata);

/* ================= Windows ================= */

typedef struct heliosview_window heliosview_window_t;

/* Predefined window styles */
typedef enum heliosview_window_style {
    HELIOSVIEW_WINDOW_NORMAL = 0, /* Standard window: title bar + border + system menu */
    HELIOSVIEW_WINDOW_BORDERLESS, /* Borderless (fully custom drawing) */
    HELIOSVIEW_WINDOW_FRAMELESS,  /* Bordered, no title bar (custom title-bar style, e.g. VS Code/Chrome) */
} heliosview_window_style_t;

/* Create a window with a preset style and user data (parameters are only
 * registered; the native window is created on show()). userdata is owned by the
 * caller (the C++ wrapper stores an object pointer) and retrieved via
 * heliosview_window_userdata. Returns NULL on failure. */
HELIOSVIEW_API heliosview_window_t* heliosview_window_create_ex(int width, int height,
                                                                const char* title, /* UTF-8 */
                                                                heliosview_window_style_t style,
                                                                void* userdata);

/* Create a standard window (no user data) */
HELIOSVIEW_API heliosview_window_t* heliosview_window_create(int width, int height, const char* title);

/* Window user data (object pointer used for event dispatch) */
HELIOSVIEW_API void* heliosview_window_userdata(const heliosview_window_t* window);
HELIOSVIEW_API void heliosview_window_set_userdata(heliosview_window_t* window, void* userdata);

/* Look up a window by id (for event dispatch; call only on the message-loop thread) */
HELIOSVIEW_API heliosview_window_t* heliosview_window_from_id(int32_t window_id);

/* Number of live windows (used to detect when the last window closes) */
HELIOSVIEW_API int heliosview_window_count(void);

HELIOSVIEW_API void heliosview_window_destroy(heliosview_window_t* window);

/* Create and show the native window: 0 = success, negative = error code */
HELIOSVIEW_API int heliosview_window_show(heliosview_window_t* window);

/* Hide the native window (keeps it alive; show()/show_state bring it back). 0 = success. */
HELIOSVIEW_API int heliosview_window_hide(heliosview_window_t* window);

typedef enum heliosview_show_state {
    HELIOSVIEW_SHOW_NORMAL = 0,     /* Normal (restore minimized/maximized) */
    HELIOSVIEW_SHOW_MINIMIZED,
    HELIOSVIEW_SHOW_MAXIMIZED,
} heliosview_show_state_t;

/* Show the window in the given state: 0 = success, negative = error code */
HELIOSVIEW_API int heliosview_window_show_state(heliosview_window_t* window,
                                                heliosview_show_state_t state);

HELIOSVIEW_API heliosview_show_state_t heliosview_window_state(const heliosview_window_t* window);

/* Close the window (sends a close request through the event pipeline: a
 * WINDOW_CLOSE event; the app decides whether to destroy). 0 = success,
 * negative = error code */
HELIOSVIEW_API int heliosview_window_close(heliosview_window_t* window);

/* Give the window focus (foreground activation + keyboard focus): 0 = success, negative = error code */
HELIOSVIEW_API int heliosview_window_focus(heliosview_window_t* window);

/* Whether the window is visible: 1 = visible, 0 = not visible / not created */
HELIOSVIEW_API int heliosview_window_is_visible(const heliosview_window_t* window);

/* Keep the window always on top (on != 0) or restore normal z-order (on == 0). 0 = success */
HELIOSVIEW_API int heliosview_window_set_topmost(heliosview_window_t* window, int on);

/* Set the window position (screen coordinates, top-left corner). 0 = success */
HELIOSVIEW_API int heliosview_window_set_position(heliosview_window_t* window, int32_t x, int32_t y);

/* Query the window position (screen coordinates). 0 = success */
HELIOSVIEW_API int heliosview_window_position(const heliosview_window_t* window,
                                              int32_t* out_x, int32_t* out_y);

/* Set the window size (client area). 0 = success */
HELIOSVIEW_API int heliosview_window_set_size(heliosview_window_t* window,
                                              int32_t width, int32_t height);

/* Query the window size (client area). 0 = success */
HELIOSVIEW_API int heliosview_window_size(const heliosview_window_t* window,
                                          int32_t* out_width, int32_t* out_height);

/* Set the window title (UTF-8). 0 = success */
HELIOSVIEW_API int heliosview_window_set_title(heliosview_window_t* window, const char* title);

/* Center the window on screen (current monitor's work area). 0 = success */
HELIOSVIEW_API int heliosview_window_center(heliosview_window_t* window);

/* Set the window opacity (0.0 fully transparent to 1.0 opaque). 0 = success */
HELIOSVIEW_API int heliosview_window_set_opacity(heliosview_window_t* window, float opacity);

/* Replace the window's icon (loaded from an .ico/.cur file path, UTF-8);
 * NULL restores the default application icon. 0 = success. */
HELIOSVIEW_API int heliosview_window_set_icon(heliosview_window_t* window, const char* icon_path);

/* Minimize the window (equivalent to show_state with SHOW_MINIMIZED). 0 = success. */
HELIOSVIEW_API int heliosview_window_minimize(heliosview_window_t* window);

/* Maximize the window. 0 = success. */
HELIOSVIEW_API int heliosview_window_maximize(heliosview_window_t* window);

/* Restore a minimized or maximized window to normal. 0 = success. */
HELIOSVIEW_API int heliosview_window_restore(heliosview_window_t* window);

/* Toggle between the normal and maximized show states. 0 = success. */
HELIOSVIEW_API int heliosview_window_toggle_maximize(heliosview_window_t* window);

/* Enable (resizable != 0) or disable (resizable == 0) resizing and the maximize
 * box. Only affects windows created with a resizable style (NORMAL / FRAMELESS);
 * NORMAL windows can still be minimized. 0 = success. */
HELIOSVIEW_API int heliosview_window_set_resizable(heliosview_window_t* window, int resizable);

/* Register a client-area drag region: a mouse-down + drag inside any registered
 * region moves the window (like a title bar; WM_NCHITTEST -> HTCAPTION). This is
 * how frameless/borderless windows get an OS move gesture. Regions accumulate;
 * all are cleared when the window is destroyed. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_add_drag_region(heliosview_window_t* window,
                                                     int32_t x, int32_t y,
                                                     int32_t width, int32_t height);

/* Remove all registered drag regions. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_clear_drag_regions(heliosview_window_t* window);

/* The window's DPI (per-monitor; GetDpiForWindow). 0 = failure / not created. */
HELIOSVIEW_API uint32_t heliosview_window_dpi(const heliosview_window_t* window);

/* Make the process per-monitor DPI aware (v2). Call once, before any window is
 * created. Returns 0 on success, negative if already set or unsupported. */
HELIOSVIEW_API int heliosview_set_dpi_awareness(void);

/* Window id (source of window_id in events) */
HELIOSVIEW_API int32_t heliosview_window_id(const heliosview_window_t* window);

/* ================= Screen / monitor geometry =================
 *
 * Work-area queries help position windows correctly on the current monitor
 * (multi-monitor + DPI aware). A "work area" is the monitor's usable area
 * (excluding taskbar/anchored bars), in physical screen coordinates.
 * The primary monitor is the one at the origin (index 0). */

typedef struct heliosview_rect {
    int32_t x;      /* left (screen coordinates) */
    int32_t y;      /* top */
    int32_t width;  /* positive */
    int32_t height; /* positive */
} heliosview_rect_t;

/* Work area of the monitor that contains the given screen point (falls back to
 * the primary monitor if the point is off-screen). 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_screen_work_area(int32_t x, int32_t y,
                                               heliosview_rect_t* out_rect);

/* Work area of the monitor the window is on (nearest if it spans several).
 * 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_work_area(const heliosview_window_t* window,
                                               heliosview_rect_t* out_rect);

/* Work area of the primary monitor. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_primary_work_area(heliosview_rect_t* out_rect);

/* The cursor's screen position. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_cursor_position(int32_t* out_x, int32_t* out_y);

/* ================= Taskbar progress =================
 *
 * A taskbar progress indicator attached to a window (Win32: ITaskbarList3).
 * Set a determinate value with heliosview_window_set_progress, change its
 * visual state (indeterminate / paused / error) with
 * heliosview_window_set_progress_state, and remove it with
 * heliosview_window_clear_progress. Message-loop thread. */

typedef enum heliosview_progress_state {
    HELIOSVIEW_PROGRESS_NONE = 0,        /* no progress indicator (== clear) */
    HELIOSVIEW_PROGRESS_NORMAL,          /* determinate, value/max */
    HELIOSVIEW_PROGRESS_INDETERMINATE,   /* animated, no value */
    HELIOSVIEW_PROGRESS_ERROR,           /* determinate, red */
    HELIOSVIEW_PROGRESS_PAUSED,          /* determinate, yellow */
} heliosview_progress_state_t;

/* Show a determinate progress (value of max; clamped). 0 = success, negative = error */
HELIOSVIEW_API int heliosview_window_set_progress(heliosview_window_t* window,
                                                  uint32_t value, uint32_t max);

/* Set only the progress visual state. 0 = success */
HELIOSVIEW_API int heliosview_window_set_progress_state(heliosview_window_t* window,
                                                        heliosview_progress_state_t state);

/* Remove the progress indicator. 0 = success */
HELIOSVIEW_API int heliosview_window_clear_progress(heliosview_window_t* window);

/* ================= Window backdrop & dark mode (Win11 DWM) =================
 *
 * Applies a system backdrop to the window (Mica / Acrylic) and toggles the
 * immersive dark-mode title bar. Available on Win11; on unsupported systems the
 * functions return a negative error code (the window is left unchanged). */

typedef enum heliosview_backdrop {
    HELIOSVIEW_BACKDROP_NONE = 0,  /* default (opaque) background */
    HELIOSVIEW_BACKDROP_MICA,      /* Mica material */
    HELIOSVIEW_BACKDROP_ACRYLIC,   /* Acrylic material */
} heliosview_backdrop_t;

/* Apply a system backdrop. 0 = success, negative = unsupported/failure */
HELIOSVIEW_API int heliosview_window_set_backdrop(heliosview_window_t* window,
                                                  heliosview_backdrop_t backdrop);

/* Toggle the immersive dark-mode title bar (on != 0 = dark). 0 = success */
HELIOSVIEW_API int heliosview_window_set_dark_mode(heliosview_window_t* window, int on);

/* ================= Window routing registry =================
 *
 * The window keeps a type-erased registry: a native routing id -> caller userdata
 * (a void*, e.g. a C++ object pointer). A registered id can be used as a native
 * message id (a menu item id / WM_COMMAND id, or a tray callback message) and the
 * default native-message conversion resolves the resulting event back to the
 * userdata (the event's `userdata` field). Tray icons and menu items are built on
 * this, and it is exposed so callers can register their own routing ids too.
 * Registered ids must be removed with heliosview_window_remove_item.
 */

/* Allocate a routing id on `window`, associate it with `userdata`, and store it
 * in the window's registry. Returns the id (never 0; 0 = failure, e.g. null window
 * or the id space is exhausted). */
HELIOSVIEW_API uint32_t heliosview_window_add_item(heliosview_window_t* window, void* userdata);

/* Remove a routing id previously returned by heliosview_window_add_item.
 * 0 = success, negative = invalid window or the id is not registered. */
HELIOSVIEW_API int heliosview_window_remove_item(heliosview_window_t* window, uint32_t id);

/* ================= Tray icon (system tray notification icon) =================
 *
 * A tray icon is attached to a window and shows an icon in the OS notification
 * area. Mouse events on the icon (single/double click, right/middle click) are
 * converted into heliosview events with the associated window's window_id
 * (see HELIOSVIEW_EVENT_TRAY_*), so they flow through the normal event queue.
 *
 * Note: the target window must already be created (shown) before creating a tray
 * on it, because the tray posts its callback messages to the window's HWND.
 *
 * The icon is loaded from a .ico/.cur/etc. file path (UTF-8); pass NULL to use
 * the default application icon. Destroy the tray before destroying its window
 * (destroying the window destroys any trays attached to it).
 */

typedef struct heliosview_tray heliosview_tray_t;

/* Create and show a tray icon attached to `window` with the given tooltip
 * (UTF-8) and icon file path (NULL = default icon). `userdata` is caller data
 * (e.g. a C++ Tray object) copied verbatim into the TRAY_* events this tray
 * produces. Returns NULL on failure. */
HELIOSVIEW_API heliosview_tray_t* heliosview_tray_create(heliosview_window_t* window,
                                                         const char* tooltip,
                                                         const char* icon_path,
                                                         void* userdata);

/* Update the tray tooltip. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_tray_set_tooltip(heliosview_tray_t* tray, const char* tooltip);

/* Replace the tray icon, loaded from an icon file path (NULL = default icon).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_tray_set_icon(heliosview_tray_t* tray, const char* icon_path);

/* Remove the tray icon and free the tray handle. Also called automatically when
 * the owning window is destroyed. */
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

/* ================= Menu (popup / context menu) =================
 *
 * A popup menu that can be attached to a window and shown (typically at the
 * current cursor position, e.g. for a tray-icon right-click context menu).
 * Each item is assigned a unique id; choosing an item posts a
 * HELIOSVIEW_EVENT_MENU_SELECT event (with menu_item = the item id and
 * window_id = the owner window), which flows through the normal event queue.
 *
 * Items are added with heliosview_menu_add_item; the caller receives the item's
 * id via out_id (used to match the event). Submenus are added by handle: the
 * parent menu takes ownership of them (destroying the parent destroys its
 * submenus). heliosview_menu_show tracks the menu at the current cursor
 * position, attached to `window` (which must already be created).
 */

typedef struct heliosview_menu heliosview_menu_t;

/* Create an empty popup menu attached to `window`. The window owns the menu:
 * its items are registered on the window so MENU_SELECT events can be routed
 * back to this menu (via `userdata`, copied verbatim into the event). The menu
 * and all its submenus must share the same owner window. Returns NULL on failure. */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_create(heliosview_window_t* window,
                                                         void* userdata);

/* Destroy the menu and all its submenus. */
HELIOSVIEW_API void heliosview_menu_destroy(heliosview_menu_t* menu);

/* Add a text item; its unique id is written to out_id (NULL = ignore).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_item(heliosview_menu_t* menu, const char* text,
                                            uint32_t* out_id);

/* Add a separator. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_separator(heliosview_menu_t* menu);

/* Add `submenu` as a submenu under `text`. The parent takes ownership of the
 * submenu. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_submenu(heliosview_menu_t* menu, const char* text,
                                               heliosview_menu_t* submenu);

/* Show the menu at the current cursor position, owned by `window` (its HWND
 * receives the WM_COMMAND that yields the MENU_SELECT event). 0 = success,
 * negative = error code. */
HELIOSVIEW_API int heliosview_menu_show(heliosview_menu_t* menu, heliosview_window_t* window);

/* ================= WebView (Win32: WebView2) =================
 *
 * A WebView handle independent of the window: it attaches to a parent window at
 * creation; afterwards operations involve only the webview itself, not
 * heliosview_window_t. Initialization is asynchronous; navigation requests made
 * before it completes are queued automatically (the last one wins).
 * Note: destroy the webview before the window; the window must not be destroyed
 * before initialization completes (usually a few milliseconds).
 * Other platforms: not yet implemented (return an error code). Event support
 * will come in a later version.
 */

typedef struct heliosview_webview heliosview_webview_t;

/* Create a WebView in the parent window's client area (async initialization). Returns NULL on failure. */
HELIOSVIEW_API heliosview_webview_t* heliosview_webview_create(heliosview_window_t* parent);

/* Destroy the WebView (must be called before destroying the parent window) */
HELIOSVIEW_API void heliosview_webview_destroy(heliosview_webview_t* webview);

/* Navigate to a URL (queued if initialization is not complete) */
HELIOSVIEW_API int heliosview_webview_navigate(heliosview_webview_t* webview, const char* url);

HELIOSVIEW_API int heliosview_webview_navigate_html(heliosview_webview_t* webview, const char* html);

/* ================= WebView native bindings (JS <-> native bridge) =================
 *
 * Each WebView runs a small shim (injected automatically) that exposes:
 *   - window.helios.call(name, ...args) -> Promise   invoke a bound native function
 *   - window.BroadcastChannel(name)                  receive native broadcasts
 *
 * Native <-> JS messages are JSON strings with a "__hv":1 envelope:
 *   JS -> native : { "__hv":1, "kind":"call", "id":N, "name":"...", "args":[...] }
 *   native -> JS : { "__hv":1, "kind":"resolve",  "id":N, "result":<json> }
 *                  { "__hv":1, "kind":"reject",   "id":N, "error":<json> }
 *                  { "__hv":1, "kind":"broadcast", "name":"...", "data":<json> }
 *
 * Threading: bind / eval / eval_async are UI-thread calls (eval_* are queued while
 * the WebView initializes). resolve / reject / broadcast may be called from any
 * thread; they marshal to the UI thread internally.
 * Lifetime: destroy the WebView only when no asynchronous calls are in flight
 * (a bind handler still running, or an eval_async not yet completed).
 */

/* Destructor for a binding's userdata; called when the binding is replaced or the
 * WebView is destroyed. May be NULL. */
typedef void (*heliosview_webview_userdata_dtor)(void* userdata);

/* Callback for a bound native function. args_json is the JSON array of the JS
 * call's arguments ("" when none). Reply via heliosview_webview_resolve/reject
 * with the same call_id. Runs on the UI thread. */
typedef void (*heliosview_webview_bind_cb)(heliosview_webview_t* webview,
                                           uint64_t call_id, const char* name,
                                           const char* args_json, void* userdata);

/* Callback for heliosview_webview_eval_async. result_json is the JSON encoding of
 * the script's completion value. error is 0 on success, else a negated platform
 * error code. Runs on the UI thread. */
typedef void (*heliosview_webview_eval_cb)(int error, const char* result_json, void* userdata);

/* Callback for a broadcast subscription: fires when the page posts a message to
 * its BroadcastChannel(name) instance(s). data_json is the posted value, which
 * may be any JSON type ("" when the message had no data). Runs on the UI thread. */
typedef void (*heliosview_webview_subscribe_cb)(heliosview_webview_t* webview,
                                                const char* name, const char* data_json,
                                                void* userdata);

/* Callback for navigation events: fires when a navigation completes (page fully
 * loaded) or fails. error is 0 on success, else a negated platform error code
 * (on WebView2: -HRESULT, e.g. -0x7ff5fb70 / COREWEBVIEW2_E_NAVIGATION_CANCELLED).
 * Runs on the UI thread. Only one callback may be registered; setting a new one
 * replaces the previous (running its dtor). */
typedef void (*heliosview_webview_navigation_cb)(heliosview_webview_t* webview,
                                                 int error, void* userdata);

/* Register a native function under `name`, callable from JS via
 * window.helios.call(name, ...). Rebinding a name replaces the previous binding
 * and calls its dtor (if any). dtor(userdata) also runs when the WebView is
 * destroyed. UI-thread call. */
HELIOSVIEW_API int heliosview_webview_bind(heliosview_webview_t* webview, const char* name,
                                           heliosview_webview_bind_cb callback, void* userdata,
                                           heliosview_webview_userdata_dtor dtor);

/* Resolve a pending JS Promise: result_json is any valid JSON value. Thread-safe. */
HELIOSVIEW_API int heliosview_webview_resolve(heliosview_webview_t* webview,
                                              uint64_t call_id, const char* result_json);

/* Reject a pending JS Promise: error_json is any valid JSON value. Thread-safe. */
HELIOSVIEW_API int heliosview_webview_reject(heliosview_webview_t* webview,
                                             uint64_t call_id, const char* error_json);

/* Run a JavaScript string (fire-and-forget). UI-thread call; queued while the
 * WebView is still initializing. */
HELIOSVIEW_API int heliosview_webview_eval(heliosview_webview_t* webview, const char* script);

/* Run a JavaScript string and get its JSON completion value. UI-thread call; queued
 * while the WebView is still initializing. The callback fires exactly once. */
HELIOSVIEW_API int heliosview_webview_eval_async(heliosview_webview_t* webview, const char* script,
                                                 heliosview_webview_eval_cb callback, void* userdata);

/* Broadcast a JSON value to the JS page's BroadcastChannel(name) instances; the
 * page receives it as a standard 'message' event. Thread-safe. */
HELIOSVIEW_API int heliosview_webview_broadcast(heliosview_webview_t* webview,
                                                const char* name, const char* data_json);

/* Subscribe to broadcasts the page posts via its BroadcastChannel(name) instances:
 * callback(name, data_json, userdata) fires on the UI thread for every postMessage
 * to a channel of that name. Subscribing to a name replaces the previous
 * subscription (calling its dtor). dtor(userdata) also runs when the WebView is
 * destroyed. UI-thread call. */
HELIOSVIEW_API int heliosview_webview_subscribe(heliosview_webview_t* webview, const char* name,
                                                heliosview_webview_subscribe_cb callback,
                                                void* userdata,
                                                heliosview_webview_userdata_dtor dtor);

/* Remove the subscription for `name` (calling its dtor). UI-thread call. */
HELIOSVIEW_API int heliosview_webview_unsubscribe(heliosview_webview_t* webview, const char* name);

/* ================= WebView events & local resources ================= */

/* Callback for navigation-start events: fires on the UI thread when a new
 * navigation begins (the initial load, links, programmatic navigate, browser
 * back/forward, and redirects). uri is the target URI (UTF-8, valid for the
 * duration of the call). is_redirected / is_user_initiated follow WebView2's
 * NavigationStarting semantics (1/0). The callback's return value cancels the
 * navigation when non-zero (0 = let it proceed). */
typedef int (*heliosview_webview_navigation_starting_cb)(heliosview_webview_t* webview,
                                                         const char* uri,
                                                         int is_redirected,
                                                         int is_user_initiated,
                                                         void* userdata);

/* Callback for source-changed (URL-changed) events: fires on the UI thread when
 * the WebView's Source (current URL) property changes. uri is the new source URI
 * (UTF-8, valid for the duration of the call); is_new_document is 1 when the
 * source change is due to a new document load, 0 for an in-document change. */
typedef void (*heliosview_webview_source_changed_cb)(heliosview_webview_t* webview,
                                                     const char* uri,
                                                     int is_new_document,
                                                     void* userdata);

/* Callback for document-title events: fires on the UI thread when the page's
 * title changes. title is the new document title (UTF-8, valid for the duration
 * of the call). */
typedef void (*heliosview_webview_title_changed_cb)(heliosview_webview_t* webview,
                                                    const char* title,
                                                    void* userdata);

/* Register a navigation-completed callback (replacing any previous one and
 * running its dtor). The callback fires on the UI thread when a navigation
 * completes or fails; it is not called for navigations that never finish
 * (e.g. aborted). UI-thread call (thread-safe: other threads are marshalled). */
HELIOSVIEW_API int heliosview_webview_set_navigation_callback(heliosview_webview_t* webview,
                                                              heliosview_webview_navigation_cb callback,
                                                              void* userdata,
                                                              heliosview_webview_userdata_dtor dtor);

/* Register a navigation-starting callback (replacing any previous one and
 * running its dtor). Fires on the UI thread just before a navigation begins;
 * returning non-zero cancels it (e.g. to block cross-origin or external links).
 * UI-thread call (thread-safe: other threads are marshalled). */
HELIOSVIEW_API int heliosview_webview_set_navigation_starting_callback(
    heliosview_webview_t* webview,
    heliosview_webview_navigation_starting_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Register a source-changed (URL-changed) callback (replacing any previous one
 * and running its dtor). Fires on the UI thread whenever the WebView's current
 * URL changes. UI-thread call (thread-safe: other threads are marshalled). */
HELIOSVIEW_API int heliosview_webview_set_source_changed_callback(
    heliosview_webview_t* webview,
    heliosview_webview_source_changed_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Register a document-title-changed callback (replacing any previous one and
 * running its dtor). Fires on the UI thread when the page title changes.
 * UI-thread call (thread-safe: other threads are marshalled). */
HELIOSVIEW_API int heliosview_webview_set_title_changed_callback(
    heliosview_webview_t* webview,
    heliosview_webview_title_changed_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Map a local folder to a virtual host name so the page can load files from it
 * via https://<host>/<relative-path>. Used to serve images or other local assets
 * that are not part of the packaged frontend (game banners, avatars, ...).
 * WebView2 restricts mappings to the "trusted origin" host suffix .local; call
 * before navigating, or the page must be reloaded for new mappings to take effect.
 * Returns 0 = success, negative = failure. */
HELIOSVIEW_API int heliosview_webview_map_local_folder(heliosview_webview_t* webview,
                                                       const char* host_name,
                                                       const char* folder_path);

/* ================= Native dialogs =================
 *
 * All dialogs are modal and must be called on the message-loop thread. They take
 * an optional parent window (NULL = unparented). A selected path is returned as
 * a UTF-8 string allocated by the library; free it with heliosview_free. Return
 * values: 1 = a result was produced, 0 = cancelled, negative = error. */

/* Folder picker. On success (1) out_path receives the selected folder (heliosview_free). */
HELIOSVIEW_API int heliosview_select_folder(heliosview_window_t* window,
                                            const char* title,
                                            char** out_path);

/* Open-file dialog. `filter` uses the "Name1 (*.ext)|*.ext|Name2|..." format
 * (NULL = "All files (*.*)|*.*"); `multi` != 0 enables multi-selection. On
 * success (n > 0) out_paths receives a NULL-terminated array of n UTF-8 paths
 * (each string and the array itself are freed with heliosview_free, or in one
 * call with heliosview_free_paths). 0 = cancelled, negative = error. */
HELIOSVIEW_API int heliosview_open_files(heliosview_window_t* window, const char* title,
                                         const char* filter, int multi, char*** out_paths);

/* Free a path array returned by heliosview_open_files (each string and the array). NULL is ignored. */
HELIOSVIEW_API void heliosview_free_paths(char** paths);

/* Save-file dialog. On success (1) out_path receives the chosen path (heliosview_free). */
HELIOSVIEW_API int heliosview_save_file(heliosview_window_t* window, const char* title,
                                        const char* filter, const char* default_name,
                                        char** out_path);

/* ================= Message box ================= */

typedef enum heliosview_message_type {
    HELIOSVIEW_MESSAGE_INFO = 1,
    HELIOSVIEW_MESSAGE_WARNING,
    HELIOSVIEW_MESSAGE_ERROR,
    HELIOSVIEW_MESSAGE_QUESTION,
} heliosview_message_type_t;

typedef enum heliosview_message_buttons {
    HELIOSVIEW_MESSAGE_OK = 1,
    HELIOSVIEW_MESSAGE_OK_CANCEL,
    HELIOSVIEW_MESSAGE_YES_NO,
    HELIOSVIEW_MESSAGE_YES_NO_CANCEL,
    HELIOSVIEW_MESSAGE_RETRY_CANCEL,
} heliosview_message_buttons_t;

typedef enum heliosview_message_result {
    HELIOSVIEW_MESSAGE_RESULT_NONE = 0,
    HELIOSVIEW_MESSAGE_RESULT_OK,
    HELIOSVIEW_MESSAGE_RESULT_CANCEL,
    HELIOSVIEW_MESSAGE_RESULT_YES,
    HELIOSVIEW_MESSAGE_RESULT_NO,
    HELIOSVIEW_MESSAGE_RESULT_RETRY,
} heliosview_message_result_t;

/* Show a modal message box. Returns the button the user pressed
 * (HELIOSVIEW_MESSAGE_RESULT_NONE = failure). Message-loop thread. */
HELIOSVIEW_API int heliosview_message_box(heliosview_window_t* window,
                                          heliosview_message_type_t type,
                                          heliosview_message_buttons_t buttons,
                                          const char* title, const char* message);

/* ================= System helpers ================= */

/* Open a URL in the default browser. 0 = success, negative = error. Message-loop thread. */
HELIOSVIEW_API int heliosview_open_url(const char* url);

/* Reveal a file/folder in Explorer, selecting it. 0 = success. Message-loop thread. */
HELIOSVIEW_API int heliosview_show_in_folder(const char* path);

/* Copy UTF-8 text to the clipboard. 0 = success. Message-loop thread. */
HELIOSVIEW_API int heliosview_clipboard_set_text(const char* text);

/* Read UTF-8 clipboard text: 1 = text written to out (heliosview_free), 0 = no
 * text, negative = error. Message-loop thread. */
HELIOSVIEW_API int heliosview_clipboard_get_text(char** out);

/* ================= Notifications (OS toast) =================
 *
 * Modern OS notifications (Win32: Windows toast). Unlike every other API in this
 * header, these functions are thread-agnostic: they may be called from any
 * thread (e.g. a worker reporting that a background task finished).
 *
 * Setup: heliosview_notification_init registers an AppUserModelID and installs a
 * Start Menu shortcut carrying it (unpackaged Win32 apps need both for toasts).
 * app_user_model_id may be NULL, in which case it is derived from the exe file
 * name. Call it once, typically at startup. It is safe to call again later.
 */

/* Initialize the notification backend. Returns 0 on success, negative on failure
 * (e.g. no Windows). Thread-safe (first call initializes). */
HELIOSVIEW_API int heliosview_notification_init(const char* app_user_model_id);

/* Show a toast with a title and body. Returns 0 on success, negative on failure
 * (e.g. not initialized, or toasts unavailable). Thread-safe. */
HELIOSVIEW_API int heliosview_notification_show(const char* title, const char* body);

#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_H */
