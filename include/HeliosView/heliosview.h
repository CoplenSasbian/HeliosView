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
 *   - An event's window_id is the platform's native window handle, as a
 *     pointer-sized integer (uintptr_t; 0 = none) — the HWND on Windows. It is
 *     valid only while the window exists; see heliosview_window_from_id.
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
 *     - heliosview_notification_init / _show (OS toasts are thread-agnostic)
 *     - heliosview_free (and the allocator, set before any other call)
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
 * Coordinates: every screen coordinate and size in this API uses the same
 * convention as Win32 — origin at the TOP-LEFT of the primary display, x right,
 * y down, in virtual-desktop logical units (DPI-independent pixels/points).
 * Backends whose native origin differs (macOS: bottom-left, y up) convert
 * internally; portable code never sees the difference. The scale of one unit is
 * reported by heliosview_window_scale_factor().
 *
 * Wide characters: wchar_t is UTF-16 on Windows and UTF-32 on macOS/Linux, so
 * heliosview_utf8_to_wide / heliosview_wide_to_utf8 convert to and from the
 * PLATFORM's wchar_t — not to UTF-16 specifically. Prefer the UTF-8 API and use
 * these codecs only to interop with a platform's wide-char functions.
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

/* Backend identity: "win32", "macos", "linux", or "portable" (this platform has
 * no backend yet — every feature then reports HELIOSVIEW_ERROR_UNSUPPORTED).
 * Static string, never NULL. Diagnostic / test output. */
HELIOSVIEW_API const char* heliosview_backend_name(void);

/* ================= Error reporting =================
 *
 * Return convention (every function): 0 = success, < 0 = an error code,
 * > 0 = a payload / count (e.g. number of items or characters).
 *
 * Standard error codes:
 *     0   HELIOSVIEW_SUCCESS: operation completed successfully
 *    -1   HELIOSVIEW_ERROR_GENERIC: generic failure — invalid/missing argument,
 *         underlying platform call failed with no specific code, or called on the wrong thread
 *    -2   HELIOSVIEW_ERROR_INVALID_ARGUMENT: invalid argument or name (e.g. not a valid C identifier)
 *    -3   HELIOSVIEW_WEBVIEW_DESTROYED: the WebView instance was already destroyed
 *    -4   HELIOSVIEW_ERROR_UNSUPPORTED: the platform or OS version cannot provide this
 *         feature (e.g. a Mica backdrop on Windows 10, any Windows-only feature on
 *         another platform). Safe to ignore: callers degrade to their fallback.
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
 */

#define HELIOSVIEW_SUCCESS                 0
#define HELIOSVIEW_ERROR_GENERIC          (-1)
#define HELIOSVIEW_ERROR_INVALID_ARGUMENT (-2)
#define HELIOSVIEW_WEBVIEW_DESTROYED      (-3)
#define HELIOSVIEW_ERROR_UNSUPPORTED      (-4)


/* The error code recorded by the most recent failing library call on this
 * thread (0 = no error recorded). Meaningful only immediately after a call
 * returned < 0 (or a NULL handle). */
HELIOSVIEW_API int heliosview_last_error(void);

/* The failure-site message recorded for heliosview_last_error: write it into
 * buf (always NUL-terminated, truncated to fit `size`; empty string when no
 * error was recorded). It answers "why did the last call fail" (the context at
 * the failure point, e.g. which operation / argument). The error code itself
 * is available via heliosview_last_error; decoding it to a platform message is
 * left to the caller (e.g. FormatMessage on Windows).
 * 0 = success, negative = invalid arguments (buf == NULL or size == 0). */
HELIOSVIEW_API int heliosview_last_error_string(char* buf, size_t size);

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
    HELIOSVIEW_EVENT_WINDOW_RESIZE,     /* Window resized (width/height); NOT emitted when minimized (that is WINDOW_MINIMIZED) */
    HELIOSVIEW_EVENT_WINDOW_MOVED,      /* Window moved (x/y = new top-left position) */
    HELIOSVIEW_EVENT_WINDOW_MOVING,     /* Drag in progress (x/y = current position) */
    HELIOSVIEW_EVENT_WINDOW_SIZING,     /* Resize drag in progress (width/height) */
    HELIOSVIEW_EVENT_WINDOW_FOCUS,      /* Window gained focus (activated) */
    HELIOSVIEW_EVENT_WINDOW_BLUR,       /* Window lost focus (deactivated) */
    HELIOSVIEW_EVENT_WINDOW_ENABLED,    /* Window was enabled */
    HELIOSVIEW_EVENT_WINDOW_DISABLED,   /* Window was disabled (modal lock) */
    HELIOSVIEW_EVENT_KEY_DOWN,
    HELIOSVIEW_EVENT_KEY_UP,
    HELIOSVIEW_EVENT_MOUSE_MOVE,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_UP,
    HELIOSVIEW_EVENT_TRAY_LEFT_CLICK,        /* tray icon left click */
    HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK, /* tray icon left double click */
    HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK,       /* tray icon right click (context menu) */
    HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK,      /* tray icon middle click */
    HELIOSVIEW_EVENT_MENU_SELECT,            /* a menu item was chosen (menu_item = item id) */
    HELIOSVIEW_EVENT_WINDOW_FIRST_SHOWN,      /* window first actually shown (window_id = native handle) — the C++ wrapper maps it to Window::firstShown */
    HELIOSVIEW_EVENT_WINDOW_MINIMIZED,         /* window minimized. No RESIZE is emitted: the OS reports the icon size, which is NOT a real size. */
    HELIOSVIEW_EVENT_WINDOW_MAXIMIZED,         /* window maximized. A RESIZE with the new client size follows. */
    HELIOSVIEW_EVENT_WINDOW_RESTORED,          /* window restored to normal from minimized/maximized. A RESIZE with the real size follows; plain resizes and fullscreen toggles never produce this. */
    HELIOSVIEW_EVENT_WINDOW_SHOWN,             /* window became visible (show/hide only; the first show stays WINDOW_FIRST_SHOWN) */
    HELIOSVIEW_EVENT_WINDOW_HIDDEN,            /* window became hidden. Minimize is NOT a hide: a minimized window keeps WS_VISIBLE, so it reports WINDOW_MINIMIZED instead. */
    HELIOSVIEW_EVENT_TEXT_INPUT,                /* text was entered (keyboard or IME commit): UTF-8 in text[0..text_len); see heliosview_event_t */
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
    HELIOSVIEW_KEY_F12,
    /* editing / navigation */
    HELIOSVIEW_KEY_TAB,
    HELIOSVIEW_KEY_BACKSPACE,
    HELIOSVIEW_KEY_DELETE,
    HELIOSVIEW_KEY_INSERT,
    HELIOSVIEW_KEY_HOME,
    HELIOSVIEW_KEY_END,
    HELIOSVIEW_KEY_PAGE_UP,
    HELIOSVIEW_KEY_PAGE_DOWN,
    /* modifier keys (also reported as bits in heliosview_event_t::modifiers) */
    HELIOSVIEW_KEY_LEFT_SHIFT,
    HELIOSVIEW_KEY_RIGHT_SHIFT,
    HELIOSVIEW_KEY_LEFT_CTRL,
    HELIOSVIEW_KEY_RIGHT_CTRL,
    HELIOSVIEW_KEY_LEFT_ALT,
    HELIOSVIEW_KEY_RIGHT_ALT,
    HELIOSVIEW_KEY_LEFT_META,   /* Windows / Command key */
    HELIOSVIEW_KEY_RIGHT_META,
    /* punctuation (US layout names; the character itself arrives via TEXT_INPUT) */
    HELIOSVIEW_KEY_MINUS,
    HELIOSVIEW_KEY_EQUAL,
    HELIOSVIEW_KEY_LEFT_BRACKET,
    HELIOSVIEW_KEY_RIGHT_BRACKET,
    HELIOSVIEW_KEY_BACKSLASH,
    HELIOSVIEW_KEY_SEMICOLON,
    HELIOSVIEW_KEY_APOSTROPHE,
    HELIOSVIEW_KEY_GRAVE,
    HELIOSVIEW_KEY_COMMA,
    HELIOSVIEW_KEY_PERIOD,
    HELIOSVIEW_KEY_SLASH,
    /* locks / system */
    HELIOSVIEW_KEY_CAPS_LOCK,
    HELIOSVIEW_KEY_NUM_LOCK,
    HELIOSVIEW_KEY_SCROLL_LOCK,
    HELIOSVIEW_KEY_PRINT_SCREEN,
    HELIOSVIEW_KEY_PAUSE,
    HELIOSVIEW_KEY_MENU,        /* context-menu key */
    /* numeric keypad */
    HELIOSVIEW_KEY_NUMPAD_0,
    HELIOSVIEW_KEY_NUMPAD_1,
    HELIOSVIEW_KEY_NUMPAD_2,
    HELIOSVIEW_KEY_NUMPAD_3,
    HELIOSVIEW_KEY_NUMPAD_4,
    HELIOSVIEW_KEY_NUMPAD_5,
    HELIOSVIEW_KEY_NUMPAD_6,
    HELIOSVIEW_KEY_NUMPAD_7,
    HELIOSVIEW_KEY_NUMPAD_8,
    HELIOSVIEW_KEY_NUMPAD_9,
    HELIOSVIEW_KEY_NUMPAD_DECIMAL,
    HELIOSVIEW_KEY_NUMPAD_DIVIDE,
    HELIOSVIEW_KEY_NUMPAD_MULTIPLY,
    HELIOSVIEW_KEY_NUMPAD_SUBTRACT,
    HELIOSVIEW_KEY_NUMPAD_ADD,
    HELIOSVIEW_KEY_NUMPAD_ENTER
} heliosview_keycode_t;

typedef enum heliosview_mouse_button {
    HELIOSVIEW_MOUSE_LEFT = 1,
    HELIOSVIEW_MOUSE_RIGHT,
    HELIOSVIEW_MOUSE_MIDDLE,
    HELIOSVIEW_MOUSE_X1,     /* first extra button (browser "back") */
    HELIOSVIEW_MOUSE_X2      /* second extra button (browser "forward") */
} heliosview_mouse_button_t;

/* Modifier keys held while an input event was produced (bit flags; OR of the
 * bits below). The generic bits are set for either side; the LEFT_/RIGHT_ bits
 * tell which physical key is down. */
typedef enum heliosview_modifier {
    HELIOSVIEW_MOD_NONE       = 0,
    HELIOSVIEW_MOD_SHIFT      = 1u << 0,
    HELIOSVIEW_MOD_CTRL       = 1u << 1,
    HELIOSVIEW_MOD_ALT        = 1u << 2,
    HELIOSVIEW_MOD_META       = 1u << 3,  /* Windows / Command key */
    HELIOSVIEW_MOD_LEFT_SHIFT = 1u << 4,
    HELIOSVIEW_MOD_RIGHT_SHIFT = 1u << 5,
    HELIOSVIEW_MOD_LEFT_CTRL  = 1u << 6,
    HELIOSVIEW_MOD_RIGHT_CTRL = 1u << 7,
    HELIOSVIEW_MOD_LEFT_ALT   = 1u << 8,
    HELIOSVIEW_MOD_RIGHT_ALT  = 1u << 9,
    HELIOSVIEW_MOD_LEFT_META  = 1u << 10,
    HELIOSVIEW_MOD_RIGHT_META = 1u << 11,
    HELIOSVIEW_MOD_CAPS_LOCK  = 1u << 12,
    HELIOSVIEW_MOD_NUM_LOCK   = 1u << 13,
} heliosview_modifier_t;

/* Extra per-event state (bit flags in heliosview_event_t::flags). */
typedef enum heliosview_event_flag {
    HELIOSVIEW_EVENT_FLAG_NONE       = 0,
    HELIOSVIEW_EVENT_FLAG_KEY_REPEAT = 1u << 0, /* KEY_DOWN: OS auto-repeat, not the initial press */
} heliosview_event_flag_t;

/* Event: flat POD, safe to pass across the DLL boundary, no ownership. Fields are
 * meaningful only for the event types noted next to them; unused fields are 0.
 *
 * The struct layout is fixed for the 1.x series: new event data is added only at
 * a major version (new event TYPES may be added at any time).
 *
 * TEXT_INPUT: `text` holds UTF-8 (IME commits included), `text_len` its byte
 * length; the buffer is always NUL-terminated and text_len < sizeof(text).
 * Input longer than the buffer is split into consecutive TEXT_INPUT events at
 * UTF-8 codepoint boundaries, so no bytes are ever dropped — append them in
 * order to reconstruct the full string. */
typedef struct heliosview_event {
    heliosview_event_type_t type;
    uintptr_t window_id;                     /* native window handle of the event's origin (0 = not window-related; HWND on Windows) */
    int64_t timestamp_ms;                    /* milliseconds since library initialization */
    int32_t x;                               /* mouse X (window client area) */
    int32_t y;                               /* mouse Y */
    int32_t width;                           /* window width (WINDOW_RESIZE) */
    int32_t height;                          /* window height (WINDOW_RESIZE) */
    heliosview_keycode_t key;                /* keycode (KEY_DOWN / KEY_UP) */
    heliosview_mouse_button_t mouse_button;  /* button (MOUSE_BUTTON_*) */
    uint32_t menu_item;                      /* menu item id (MENU_SELECT) */
    void* userdata;                          /* owning tray/menu userdata (TRAY_* / MENU_SELECT) */
    uint32_t modifiers;                      /* HELIOSVIEW_MOD_* bits held (KEY_*, MOUSE_*, TEXT_INPUT) */
    uint32_t flags;                          /* HELIOSVIEW_EVENT_FLAG_* bits (KEY_DOWN) */
    uint32_t text_len;                       /* TEXT_INPUT: UTF-8 length in text[] (0 = none) */
    char text[48];                           /* TEXT_INPUT: UTF-8 text, NUL-terminated */
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

/* ================= Native message interceptor / pipeline filter =================
 *
 * Intercepts or augments native window messages (middleware / onion model).
 * Filters run BEFORE the library's built-in conversion, so a filter sees every
 * message first (the legacy heliosview_native_handler_fn delegates run after it).
 *
 * native_context:
 *   - window_id: native window handle / ID (0 = global or unparented message)
 *   - native_msg: platform-specific native message pointer (const MSG* on Windows,
 *                 NSEvent* on macOS, GdkEvent* on Linux). Valid ONLY for the
 *                 duration of the callback: inspect it, never store, replace or
 *                 free it (the library hands the original to the default
 *                 procedure and the OS).
 *   - result: platform result code the window procedure returns to the system
 *   - is_handled: 1 = the message is considered handled (no default processing),
 *                 0 = unhandled
 *
 * Filters are chained: each filter can perform pre-processing, conditionally invoke
 * next(ctx, next_ud) to forward to downstream filters / default handling, and perform
 * post-processing (e.g. adjusting result or observing return values).
 * Not invoking next() short-circuits the pipeline and prevents downstream processing.
 * The value returned to the OS is always ctx->result, so a filter that wants to
 * force a specific return value (including 0) sets ctx->result itself; the terminal
 * stage sets it to 0 when the built-in conversion handled the message, otherwise to
 * the platform's default-procedure result.
 *
 * Threading: register and remove filters on the message-loop thread — dispatch
 * happens there and the registry is not locked. Registering from another thread
 * is a data race, not a no-op.
 *
 * Filter ids and legacy handler ids come from separate id spaces: an id from
 * heliosview_add_native_handler is not valid for heliosview_remove_native_filter.
 */

typedef struct heliosview_native_context {
    uintptr_t window_id;     /* native window handle / ID of the message origin (0 = none / global) */
    void* native_msg;        /* platform-specific native message (Windows: const MSG*) */
    intptr_t result;         /* platform return value / LRESULT when handled */
    int is_handled;          /* 1 = handled (inhibits system default procedure), 0 = unhandled */
} heliosview_native_context_t;

typedef void (*heliosview_next_filter_fn)(heliosview_native_context_t* ctx, void* next_ud);

typedef void (*heliosview_native_filter_fn)(
    heliosview_native_context_t* ctx,
    heliosview_next_filter_fn next,
    void* next_ud,
    void* userdata);

/* Register a native filter into the interceptor pipeline.
 * Filters are invoked in registration order (outer to inner).
 * Returns a filter ID (0 on failure) used to remove it. */
HELIOSVIEW_API uint32_t heliosview_add_native_filter(heliosview_native_filter_fn filter, void* userdata);

/* Remove a filter previously registered with heliosview_add_native_filter.
 * 0 = success, negative = not found. */
HELIOSVIEW_API int heliosview_remove_native_filter(uint32_t id);

/* Legacy conversion delegate: wraps a simple converter function into the pipeline. */
typedef int (*heliosview_native_handler_fn)(void* native_msg, uintptr_t window_id);

/**
 * @deprecated Legacy conversion delegate. Use heliosview_add_native_filter instead for
 * the pipeline middleware filter model.
 */
HELIOSVIEW_API uint32_t heliosview_add_native_handler(heliosview_native_handler_fn handler);

/**
 * @deprecated Use heliosview_remove_native_filter instead.
 */
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

/* ================= Windows ================= */

typedef struct heliosview_window heliosview_window_t;

/* Predefined window styles */
typedef enum heliosview_window_style {
    HELIOSVIEW_WINDOW_NORMAL = 0, /* Standard window: title bar + border + system menu */
    HELIOSVIEW_WINDOW_BORDERLESS, /* Borderless (fully custom drawing; not resizable) */
    HELIOSVIEW_WINDOW_FRAMELESS,  /* Fully frameless: no title bar / caption buttons; resizable via the edges; the app draws all chrome (e.g. the injected <helios-window-controls> web component for the buttons) */
} heliosview_window_style_t;

/* Style flags, combined with | and passed to heliosview_window_create_ex2.
 * The preset style picks the baseline (frame, resize behavior, caption); these
 * flags refine it. A flag a platform cannot honor is ignored, never an error —
 * use them for the idiomatic look of each OS:
 *
 *   Windows: TITLEBAR_HIDDEN/TITLEBAR_TRANSPARENT/FULL_SIZE_CONTENT all mean
 *            "no native caption, the client area fills the window" (the app
 *            draws its own chrome, e.g. the injected <helios-window-controls>).
 *   macOS:   TITLEBAR_HIDDEN keeps a real NSWindow title bar but hides its
 *            title; TITLEBAR_TRANSPARENT lets the content show through it;
 *            FULL_SIZE_CONTENT extends the content view under the title bar —
 *            together these give the standard macOS look with the traffic
 *            lights floating over the page. Prefer them over FRAMELESS on macOS.
 *   Linux:   mapped to the closest WM hint; may be ignored by some window
 *            managers (Wayland in particular).
 *
 * CLOSABLE/MINIMIZABLE/RESIZABLE restrict the corresponding affordance and are
 * independent of the preset style; HELIOSVIEW_WINDOW_FLAG_TOOLWINDOW keeps the
 * window out of the taskbar / Dock window list (a utility or palette window). */
typedef enum heliosview_window_flag {
    HELIOSVIEW_WINDOW_FLAG_NONE = 0,
    HELIOSVIEW_WINDOW_FLAG_TITLEBAR_HIDDEN = 1u << 0,
    HELIOSVIEW_WINDOW_FLAG_TITLEBAR_TRANSPARENT = 1u << 1,
    HELIOSVIEW_WINDOW_FLAG_FULL_SIZE_CONTENT = 1u << 2,
    HELIOSVIEW_WINDOW_FLAG_CLOSABLE = 1u << 3,
    HELIOSVIEW_WINDOW_FLAG_MINIMIZABLE = 1u << 4,
    HELIOSVIEW_WINDOW_FLAG_RESIZABLE = 1u << 5,
    HELIOSVIEW_WINDOW_FLAG_TOOLWINDOW = 1u << 6,
} heliosview_window_flag_t;

/* Create a window with a preset style and user data. The native window is
 * created immediately (not shown); show() makes it visible. userdata is owned
 * by the caller (the C++ wrapper stores an object pointer) and retrieved via
 * heliosview_window_userdata. Returns NULL on failure. Message-loop thread. */
HELIOSVIEW_API heliosview_window_t* heliosview_window_create_ex(int width, int height,
                                                                const char* title, /* UTF-8 */
                                                                heliosview_window_style_t style,
                                                                void* userdata);

/* Create a standard window (no user data) */
HELIOSVIEW_API heliosview_window_t* heliosview_window_create(int width, int height, const char* title);

/* Same as heliosview_window_create_ex, plus a combination of
 * heliosview_window_flag_t bits (0 = none). Returns NULL on failure.
 * Message-loop thread. */
HELIOSVIEW_API heliosview_window_t* heliosview_window_create_ex2(int width, int height,
                                                                 const char* title, /* UTF-8 */
                                                                 heliosview_window_style_t style,
                                                                 uint32_t flags,
                                                                 void* userdata);

/* The flags the window was created with (0 when created without flags). */
HELIOSVIEW_API uint32_t heliosview_window_flags(const heliosview_window_t* window);

/* The window's display scale: 1.0 at 96 DPI / non-Retina, 2.0 on a Retina or
 * 200% display. Multiply logical coordinates/sizes by this to get device pixels
 * (e.g. to size a bitmap). Returns 1.0 when the window is not created. */
HELIOSVIEW_API float heliosview_window_scale_factor(const heliosview_window_t* window);

/* Window user data (object pointer used for event dispatch) */
HELIOSVIEW_API void* heliosview_window_userdata(const heliosview_window_t* window);
HELIOSVIEW_API void heliosview_window_set_userdata(heliosview_window_t* window, void* userdata);

/* Look up a window by its native handle (the window_id an event carries; the
 * HWND on Windows). The handle is validated against the OS before the window is
 * read back, so a destroyed window safely resolves to NULL and a stale queued
 * event becomes a no-op. Call only on the message-loop thread. */
HELIOSVIEW_API heliosview_window_t* heliosview_window_from_id(uintptr_t window_id);

/* Number of live windows (used to detect when the last window closes) */
HELIOSVIEW_API int heliosview_window_count(void);

HELIOSVIEW_API void heliosview_window_destroy(heliosview_window_t* window);

/* Show the native window (created by heliosview_window_create / _create_ex;
 * the first show fires a WINDOW_FIRST_SHOWN event — the window is created and
 * visible; the C++ wrapper maps it to Window::firstShown). 0 = success, -1 = window not created. */
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

/* Replace the window's icon (loaded from an icon file path, UTF-8 — see Icons);
 * NULL restores the default application icon. [Windows only] — macOS and Linux
 * have no per-window icon (the application icon comes from the bundle or the
 * .desktop file); they return HELIOSVIEW_ERROR_UNSUPPORTED. 0 = success. */
HELIOSVIEW_API int heliosview_window_set_icon(heliosview_window_t* window, const char* icon_path);

/* Same, with heliosview_icon_flag_t bits (0 = none). */
HELIOSVIEW_API int heliosview_window_set_icon_ex(heliosview_window_t* window, const char* icon_path,
                                                 uint32_t flags);

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

/* Whether the window can currently be resized / maximized (the complement of
 * set_resizable). 1 = resizable, 0 = not resizable / not created. */
HELIOSVIEW_API int heliosview_window_is_resizable(const heliosview_window_t* window);

/* Register a client-area drag region: a mouse-down + drag inside any registered
 * region moves the window (like a title bar; WM_NCHITTEST -> HTCAPTION). This is
 * how frameless/borderless windows get an OS move gesture. Regions accumulate;
 * all are cleared when the window is destroyed. 0 = success, negative = error.
 *
 * WebView caveat: drag regions are implemented through the host window's
 * WM_NCHITTEST. A full-bleed WebView is a child window covering the entire
 * client area, so hit-testing for points over it is answered by the WebView's
 * own window procedure and never reaches the host — registered drag regions are
 * therefore ineffective while the WebView covers them. With a WebViewWindow,
 * register the drag area on the page instead: the injected
 * <helios-window-title-bar> component drags through WebView2's native
 * app-region:drag support (enabled by the library via
 * ICoreWebView2Settings9::IsNonClientRegionSupportEnabled), or bind
 * heliosview_window_start_drag to a page callback for a fully custom area. */
HELIOSVIEW_API int heliosview_window_add_drag_region(heliosview_window_t* window,
                                                     int32_t x, int32_t y,
                                                     int32_t width, int32_t height);

/* Remove all registered drag regions. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_clear_drag_regions(heliosview_window_t* window);

/* Start a window drag (move loop). Frameless windows whose chrome is covered by
 * a full-bleed WebView never receive WM_NCHITTEST (the WebView child eats the
 * input), so the page calls this on mousedown over its own title bar to move the
 * window like a native title bar. Message-loop thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_start_drag(heliosview_window_t* window);

/* The window's DPI (per-monitor; GetDpiForWindow). 0 = failure / not created.
 * [Windows only] — macOS/Linux have no per-monitor DPI value in this sense; use
 * heliosview_window_scale_factor() for portable code (dpi / 96). */
HELIOSVIEW_API uint32_t heliosview_window_dpi(const heliosview_window_t* window);

/* Height (client pixels, DPI-scaled) of the title-bar strip a FRAMELESS window
 * reserves at the top (the drag area / where the page puts its title-bar
 * buttons). Returns 0 for styles without such a strip (NORMAL / BORDERLESS) or
 * when the window is not created. Useful to keep page content clear of the
 * strip (e.g. keep the top-right free for the buttons). */
HELIOSVIEW_API int32_t heliosview_window_title_bar_height(const heliosview_window_t* window);

/* Enforce a minimum client size (prevents the window from being resized below
 * it, e.g. so the UI is not crushed). Pass 0 for either dimension to leave it
 * unconstrained. Works for NORMAL / FRAMELESS; borderless is fully custom.
 * 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_set_min_size(heliosview_window_t* window,
                                                  int32_t min_width, int32_t min_height);

/* Enforce a maximum client size. Pass 0 for either dimension to leave it
 * unconstrained (0,0 = no maximum). 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_set_max_size(heliosview_window_t* window,
                                                  int32_t max_width, int32_t max_height);

/* Flash the taskbar button a few times (background-task finished hint).
 * 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_flash(heliosview_window_t* window);

/* Flash the taskbar button until the window is focused (e.g. an urgent
 * notification). 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_flash_until_focus(heliosview_window_t* window);

/* Enter (on != 0) or leave (on == 0) fullscreen: the window covers the whole
 * monitor (no frame, no taskbar), and the previous geometry is restored on exit.
 * 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_set_fullscreen(heliosview_window_t* window, int on);

/* Whether the window is currently fullscreen. 1/0. */
HELIOSVIEW_API int heliosview_window_is_fullscreen(const heliosview_window_t* window);

/* Enable (enabled != 0) or disable (enabled == 0) the window. A disabled window
 * does not receive keyboard/mouse input and its children are locked — used for
 * modal states. Disabling fires a WINDOW_DISABLED event, enabling a
 * WINDOW_ENABLED one. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_window_set_enabled(heliosview_window_t* window, int enabled);

/* Whether the window is enabled. 1 = enabled, 0 = disabled / not created. */
HELIOSVIEW_API int heliosview_window_is_enabled(const heliosview_window_t* window);

/* ================= Session end (shutdown / logoff) =================
 *
 * OS session-end (WM_QUERYENDSESSION: system shutdown, restart, or logoff).
 * The registered callback runs synchronously on the message-loop thread before
 * the session ends, giving the app a chance to save state; return non-zero to
 * veto the shutdown (zero = allow). At most one callback: setting a new one
 * replaces the previous.
 * macOS: delivered for applicationShouldTerminate (quit / logout); the veto
 * return value is honored as NSApplicationTerminateReply::Cancel. */
typedef int (*heliosview_session_end_cb)(void* userdata);

/* Register the session-end callback (NULL = unregister). Returns 0. */
HELIOSVIEW_API int heliosview_set_session_end_callback(heliosview_session_end_cb callback,
                                                       void* userdata);

/* Make the process per-monitor DPI aware (v2). Call once, before any window is
 * created. Returns 0 on success, negative if already set or unsupported.
 * [Windows only] — macOS handles scaling per display automatically (Retina) and
 * Linux follows the toolkit/compositor, so they return
 * HELIOSVIEW_ERROR_UNSUPPORTED and portable code can ignore the result. */
HELIOSVIEW_API int heliosview_set_dpi_awareness(void);

/* Native window handle (HWND on Windows) — the window_id carried in events;
 * valid from creation (heliosview_window_create / _create_ex). */
HELIOSVIEW_API uintptr_t heliosview_window_id(const heliosview_window_t* window);

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
 * A taskbar/dock progress indicator attached to a window.
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
 * immersive dark-mode title bar. Mica/Acrylic require Windows 11 22H2 (build
 * 22621); the dark-mode title bar requires Windows 10 1809 (build 17763). Where
 * the OS cannot provide it the function returns HELIOSVIEW_ERROR_UNSUPPORTED
 * (-4) and leaves the window unchanged, so callers can ignore -4 and degrade.
 * On other platforms these map to the platform's own material/theme concept or
 * return -4. */

typedef enum heliosview_backdrop {
    HELIOSVIEW_BACKDROP_NONE = 0,  /* default (opaque) background */
    HELIOSVIEW_BACKDROP_MICA,      /* Mica material */
    HELIOSVIEW_BACKDROP_ACRYLIC,   /* Acrylic material */
} heliosview_backdrop_t;

/* Apply a system backdrop. 0 = success, HELIOSVIEW_ERROR_UNSUPPORTED (-4) when
 * the OS cannot, other negative = failure. */
HELIOSVIEW_API int heliosview_window_set_backdrop(heliosview_window_t* window,
                                                  heliosview_backdrop_t backdrop);

/* Toggle the immersive dark-mode title bar (on != 0 = dark). 0 = success,
 * HELIOSVIEW_ERROR_UNSUPPORTED (-4) when the OS cannot, other negative = failure. */
HELIOSVIEW_API int heliosview_window_set_dark_mode(heliosview_window_t* window, int on);

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

/* Forward declaration: the tray can own a context menu (heliosview_tray_set_menu). */
typedef struct heliosview_menu heliosview_menu_t;

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

/* Attach (or detach, with NULL) the tray's context menu. The tray keeps a
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

/* ================= Action (shareable menu command) =================
 *
 * An action is a command with identity and state: its label, whether it is
 * enabled, whether it is checkable/checked, its shortcut and standard role. It
 * carries no window and no menu — a menu only *displays* actions.
 *
 * The same action may be added to any number of menus (an Edit menu, a context
 * menu, ...): one triggered event, one enabled/checked state, one label.
 *
 * Portability contract (every backend must satisfy it):
 *   - Menus re-read action state when they pop up, so changing an action
 *     updates every menu that shows it. Native item state is a cache, never the
 *     source of truth.
 *   - Shortcut scope differs by platform: on macOS a shortcut is a menu key
 *     equivalent, so it only takes effect for actions reachable from the
 *     application menu bar; on Windows every live action with a shortcut is an
 *     application-wide accelerator. Portable applications should put
 *     shortcut-bearing actions in the application menu bar (see
 *     heliosview_menu_set_app_menu) so both platforms behave identically.
 *   - heliosview_menu_set_default_action (bold item, Enter activates) is a
 *     Windows convention; other platforms may ignore it.
 *
 * Lifetime: reference-counted by the menus that display it. heliosview_action_destroy
 * releases the caller's reference; the object itself lives until the last menu
 * using it is destroyed, so a menu can never reference a freed action.
 *
 * Message-loop thread (like menus and windows).
 */

typedef struct heliosview_action heliosview_action_t;

/* ---------- Standard roles ----------
 *
 * A role marks an action as a standard command. The library then supplies the
 * platform's conventional label and shortcut, and — where the application cannot
 * do the work itself — performs the action:
 *
 *   role                    macOS (system-provided)          Windows (library-provided)
 *   ABOUT                   About <App>                     event only
 *   PREFERENCES             "Settings…" ⌘,                 event only
 *   QUIT                    "Quit <App>" ⌘Q                event only (C++ App quits when
 *                                                          no handler is connected)
 *   HIDE / HIDE_OTHERS      ⌘H / ⌥⌘H                       event only
 *   SHOW_ALL                "Show All"                     event only
 *   SERVICES                system Services menu           event only
 *   UNDO / REDO             ⌘Z / ⇧⌘Z → responder chain     Ctrl+Z / Ctrl+Y → focused control
 *   CUT/COPY/PASTE          ⌘X/⌘C/⌘V → responder chain     Ctrl+X/C/V → focused control
 *   SELECT_ALL              ⌘A → responder chain           Ctrl+A → focused control
 *   DELETE                  forward delete                 Del → focused control
 *   MINIMIZE                ⌘M → performMiniaturize:       minimizes the active window
 *   ZOOM                    performZoom:                   maximizes/restores it
 *   CLOSE_WINDOW            ⌘W → performClose:             closes the active window
 *   TOGGLE_FULLSCREEN       ⌃⌘F → toggleFullScreen:        F11 → toggles it
 *   BRING_ALL_TO_FRONT      "Bring All to Front"           event only
 *
 * "Event only" roles still post MENU_SELECT, so the application can implement
 * them (and observe/confirm others); the library only performs the actions the
 * application cannot perform itself. A role's default label/shortcut can always
 * be overridden with heliosview_action_set_text / _set_shortcut.
 */
typedef enum heliosview_menu_role {
    HELIOSVIEW_MENU_ROLE_NONE = 0,   /* plain action */
    /* application */
    HELIOSVIEW_MENU_ROLE_ABOUT,
    HELIOSVIEW_MENU_ROLE_PREFERENCES,
    HELIOSVIEW_MENU_ROLE_QUIT,
    HELIOSVIEW_MENU_ROLE_HIDE,
    HELIOSVIEW_MENU_ROLE_HIDE_OTHERS,
    HELIOSVIEW_MENU_ROLE_SHOW_ALL,
    HELIOSVIEW_MENU_ROLE_SERVICES,
    /* edit */
    HELIOSVIEW_MENU_ROLE_UNDO,
    HELIOSVIEW_MENU_ROLE_REDO,
    HELIOSVIEW_MENU_ROLE_CUT,
    HELIOSVIEW_MENU_ROLE_COPY,
    HELIOSVIEW_MENU_ROLE_PASTE,
    HELIOSVIEW_MENU_ROLE_SELECT_ALL,
    HELIOSVIEW_MENU_ROLE_DELETE,
    /* window */
    HELIOSVIEW_MENU_ROLE_MINIMIZE,
    HELIOSVIEW_MENU_ROLE_ZOOM,
    HELIOSVIEW_MENU_ROLE_CLOSE_WINDOW,
    HELIOSVIEW_MENU_ROLE_TOGGLE_FULLSCREEN,
    HELIOSVIEW_MENU_ROLE_BRING_ALL_TO_FRONT,
} heliosview_menu_role_t;

/* Create an action with the given label (UTF-8; NULL = empty). `userdata` is
 * caller data (the C++ wrapper stores the Action object pointer) copied into
 * the MENU_SELECT events this action produces. Returns NULL on failure. */
HELIOSVIEW_API heliosview_action_t* heliosview_action_create(const char* text, void* userdata);

/* Create a standard role action (see the table above). `text` overrides the
 * platform's default label (NULL = default). Returns NULL on failure. */
HELIOSVIEW_API heliosview_action_t* heliosview_action_create_role(heliosview_menu_role_t role,
                                                                  const char* text,
                                                                  void* userdata);

/* The action's role (HELIOSVIEW_MENU_ROLE_NONE for a plain action). */
HELIOSVIEW_API heliosview_menu_role_t heliosview_action_role(const heliosview_action_t* action);

/* Release the caller's reference. The action is freed once no menu references it. */
HELIOSVIEW_API void heliosview_action_destroy(heliosview_action_t* action);

/* Update the label (UTF-8). Menus showing this action display it on their next popup. */
HELIOSVIEW_API int heliosview_action_set_text(heliosview_action_t* action, const char* text);

/* Set the keyboard shortcut, written portably as modifier(s) joined by '+' and a
 * key: "Primary+S", "Ctrl+Shift+Z", "Alt+F4", "F11", "Cmd+O".
 *   Primary  = Command on macOS, Control elsewhere (write this in portable code)
 *   Cmd      = Command on macOS, Control elsewhere (alias of Primary)
 *   Ctrl, Alt/Option, Shift, Meta/Win  = that key on every platform
 *   key      = a single character (A-Z, 0-9, punctuation) or a name: F1..F24,
 *              Escape, Return/Enter, Space, Tab, Backspace, Delete, Insert, Home,
 *              End, PageUp, PageDown, Left, Right, Up, Down, Comma, Period,
 *              Slash, Semicolon, Apostrophe, Grave, Minus, Equal, Backslash
 * The string is stored as given (menus display the platform's own form) and is
 * rejected (-1 + heliosview_last_error) when it cannot be parsed. NULL clears it. */
HELIOSVIEW_API int heliosview_action_set_shortcut(heliosview_action_t* action, const char* shortcut);

/* The action's shortcut string (NULL when unset; library-owned, valid until the
 * shortcut is changed or the action is destroyed). */
HELIOSVIEW_API const char* heliosview_action_shortcut(const heliosview_action_t* action);

/* Enable (enabled != 0) or disable the action: disabled items are grayed out and
 * not selectable in every menu that shows it. 0 = success. */
HELIOSVIEW_API int heliosview_action_set_enabled(heliosview_action_t* action, int enabled);

/* Make the action checkable (checkable != 0): menus then draw a checkmark while
 * it is checked. heliosview_action_set_radio_style switches the mark to a radio
 * bullet. Un-checkable actions ignore heliosview_action_set_checked. */
HELIOSVIEW_API int heliosview_action_set_checkable(heliosview_action_t* action, int checkable);
HELIOSVIEW_API int heliosview_action_set_radio_style(heliosview_action_t* action, int radio);

/* Set the checked state (only meaningful for a checkable action). */
HELIOSVIEW_API int heliosview_action_set_checked(heliosview_action_t* action, int checked);

/* Read state: 1 = enabled / checked / checkable / radio style, 0 = not (or NULL action). */
HELIOSVIEW_API int heliosview_action_is_enabled(const heliosview_action_t* action);
HELIOSVIEW_API int heliosview_action_is_checked(const heliosview_action_t* action);
HELIOSVIEW_API int heliosview_action_is_checkable(const heliosview_action_t* action);
HELIOSVIEW_API int heliosview_action_is_radio_style(const heliosview_action_t* action);

/* The action's label (UTF-8, library-owned; valid until the action is changed or
 * destroyed; never NULL — empty string when unset). */
HELIOSVIEW_API const char* heliosview_action_text(const heliosview_action_t* action);

/* The action's process-unique id: it is what MENU_SELECT carries in
 * heliosview_event_t::menu_item, and what an accelerator will trigger. 0 = none. */
HELIOSVIEW_API uint32_t heliosview_action_id(const heliosview_action_t* action);

/* Look up a live action by its id (0 / unknown id = NULL). Message-loop thread. */
HELIOSVIEW_API heliosview_action_t* heliosview_action_from_id(uint32_t id);

/* ================= Menu (popup / context menu) =================
 *
 * A menu is a standalone object, like a tray icon: it is created and filled
 * without any window, and only showing it needs an owner (the OS delivers the
 * selection to the owner's message queue).
 *
 * Two kinds exist, mirroring the Win32 factories behind them:
 *   - a POPUP menu (heliosview_menu_create; CreatePopupMenu on Windows): the
 *     normal case — a context menu (heliosview_menu_show), a tray menu, or a
 *     submenu (heliosview_menu_add_submenu);
 *   - a MENU BAR (heliosview_menu_create_bar; CreateMenu on Windows): the
 *     application menu bar installed with heliosview_menu_set_app_menu. Only a
 *     menu bar can serve as a window menu, so heliosview_menu_set_app_menu
 *     rejects popup menus and heliosview_menu_add_submenu rejects bars.
 *
 * heliosview_menu_show(menu, window) pops a menu up at the current cursor
 * position; `window` may be NULL, in which case the library uses its own hidden
 * owner window — so a menu can be shown from a tray icon even when the
 * application has no window at all (MENU_SELECT then carries window_id = 0).
 *
 * Each item is assigned a unique id; choosing an item posts a
 * HELIOSVIEW_EVENT_MENU_SELECT event (menu_item = the item id, userdata = the
 * menu's userdata) which flows through the normal event queue. Its window_id is
 * the window the menu belongs to; for an item of the application menu bar it is
 * the window that was active when the item was chosen (0 = none). Items are
 * added with heliosview_menu_add_item; the caller receives the item's id via
 * out_id (used to match the event). Item state flags (checkmark, disabled,
 * radio-style checkmark, default) are set at creation with
 * heliosview_menu_add_item_ex and toggled afterwards with the
 * heliosview_menu_set_*_item_* helpers (checked / enabled / default). Submenus
 * are added by handle: the parent keeps its own reference to them, so destroying
 * a submenu that is still attached only drops the caller's reference (the parent
 * keeps displaying it until the parent is released too).
 */

/* Item flags for heliosview_menu_add_item_ex (OR-able). */
enum {
    HELIOSVIEW_MENU_ITEM_CHECKED    = 1 << 0, /* show a checkmark next to the text */
    HELIOSVIEW_MENU_ITEM_DISABLED   = 1 << 1, /* grayed out and not selectable */
    HELIOSVIEW_MENU_ITEM_RADIOCHECK = 1 << 2, /* bullet (radio) instead of a checkmark */
    HELIOSVIEW_MENU_ITEM_DEFAULT    = 1 << 3  /* default item: bold, Enter / double-click activates */
};

/* Create an empty standalone POPUP menu. `userdata` is caller data (e.g. a C++
 * Menu object) copied verbatim into the MENU_SELECT events this menu produces;
 * it is how a selection is routed back to the menu, so no window is needed here.
 * A popup menu is shown with heliosview_menu_show, attached to a tray
 * (heliosview_tray_set_menu), or added as a submenu
 * (heliosview_menu_add_submenu). For the application menu bar use
 * heliosview_menu_create_bar instead. Returns NULL on failure. */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_create(void* userdata);

/* Create an empty window MENU BAR for the application menu bar. Unlike a popup
 * menu it can be installed as the menu bar of every window
 * (heliosview_menu_set_app_menu) — the Win32 factory is CreateMenu, while popup
 * menus are CreatePopupMenu, and the two are NOT interchangeable (SetMenu
 * rejects popup handles with ERROR_INVALID_PARAMETER). Its items are top-level
 * entries, typically submenus; it is never shown with heliosview_menu_show.
 * Returns NULL on failure. */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_create_bar(void* userdata);

/* Add an action to the menu (in order). The same action may be added to several
 * menus; the menu holds a reference (see heliosview_action_destroy) and borrows
 * nothing else. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_action(heliosview_menu_t* menu, heliosview_action_t* action);

/* Make `action` the menu's default item (bold; Enter / double-click activates
 * it; one per menu). NULL clears the current default. The action must already
 * be in this menu. Windows convention — other platforms may ignore it.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_default_action(heliosview_menu_t* menu,
                                                      heliosview_action_t* action);

/* ================= Menu kind & lazy population ================= */

/* What a submenu *is*, beyond its title. macOS needs this to wire the standard
 * menus (NSApp.servicesMenu / windowsMenu / helpMenu, and the App menu), where
 * the title is chosen by the system; other platforms treat it as a hint. */
typedef enum heliosview_menu_kind {
    HELIOSVIEW_MENU_KIND_NORMAL = 0,
    HELIOSVIEW_MENU_KIND_APP,       /* the application menu (macOS: first submenu of the bar) */
    HELIOSVIEW_MENU_KIND_SERVICES,  /* the Services menu (macOS) */
    HELIOSVIEW_MENU_KIND_WINDOW,    /* the Window menu (macOS) */
    HELIOSVIEW_MENU_KIND_HELP,      /* the Help menu (macOS) */
} heliosview_menu_kind_t;

/* Mark a menu with a standard kind (see above). 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_menu_set_kind(heliosview_menu_t* menu, heliosview_menu_kind_t kind);

/* The menu's kind (HELIOSVIEW_MENU_KIND_NORMAL when unset). */
HELIOSVIEW_API heliosview_menu_kind_t heliosview_menu_kind(const heliosview_menu_t* menu);

/* Called just before a menu is shown — including every time a submenu opens —
 * so a menu can populate itself lazily (recent files, state-dependent items).
 * Runs on the message-loop thread; the callback may add actions/submenus to
 * `menu` (item state is refreshed after it returns). NULL removes it. */
typedef void (*heliosview_menu_open_cb)(heliosview_menu_t* menu, void* userdata);

/* Register/remove the open callback. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_open_callback(heliosview_menu_t* menu,
                                                     heliosview_menu_open_cb callback,
                                                     void* userdata);

/* ================= Application menu bar =================
 *
 * Install `menu` as the application's menu bar (NULL removes it).
 *
 *   macOS        the one global menu bar (NSApp.mainMenu); the first submenu
 *                becomes the App menu.
 *   Windows      the menu bar of every HeliosView top-level window, including
 *                windows created later — the platform's native shape (Windows
 *                has no global menu bar).
 *   Linux/GTK    each window's GtkMenuBar.
 *
 * `menu` must be a MENU BAR (heliosview_menu_create_bar): the Win32 API only
 * attaches CreateMenu handles to windows and rejects popup menus
 * (CreatePopupMenu) with ERROR_INVALID_PARAMETER. The bar must outlive the
 * windows showing it; destroying it detaches it from every window first.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_app_menu(heliosview_menu_t* menu);

/* The current application menu bar (NULL when unset). */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_app_menu(void);

/* Translate a native keyboard message against the library's action shortcuts
 * (menu accelerators). Call it in your own message loop before
 * TranslateMessage/DispatchMessage — heliosview_pump_events/heliosview_run
 * already do. Returns 1 when the message was consumed as an accelerator (do not
 * dispatch it further), 0 when it is not ours.
 *
 * native_msg is platform-specific: a MSG* on Windows (an NSEvent* or GdkEvent*
 * on the other platforms once they exist). Only Windows needs this: macOS routes
 * key equivalents through the menu bar itself and the Linux toolkits do the same
 * for GTK/Qt menus, so there it is a no-op that always returns 0 — an application
 * with its own event loop can call it unconditionally. */
HELIOSVIEW_API int heliosview_translate_accelerator(void* native_msg);

/* Destroy the menu and all its submenus. */
HELIOSVIEW_API void heliosview_menu_destroy(heliosview_menu_t* menu);

/* Add a text item. Convenience over the action API: the menu creates and owns an
 * action with this text; its id is written to out_id (NULL = ignore) and its
 * state is reachable through the heliosview_menu_*_item_* helpers below.
 * Use heliosview_menu_add_action instead when the item must be shared or its
 * state managed explicitly. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_item(heliosview_menu_t* menu, const char* text,
                                            uint32_t* out_id);

/* Add a text item with initial state flags (HELIOSVIEW_MENU_ITEM_*); its
 * unique id is written to out_id (NULL = ignore). The flags are applied once
 * at creation; change them later with heliosview_menu_set_item_checked /
 * _enabled / _default. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_item_ex(heliosview_menu_t* menu, const char* text,
                                               uint32_t flags, uint32_t* out_id);

/* Convenience wrapper: heliosview_menu_add_item_ex with HELIOSVIEW_MENU_ITEM_CHECKED
 * (or 0) when `checked` is non-zero. See heliosview_menu_set_item_checked. */
HELIOSVIEW_API int heliosview_menu_add_checkable_item(heliosview_menu_t* menu, const char* text,
                                                      int checked, uint32_t* out_id);

/* Set (checked != 0) or clear the checkmark of the item with the given id.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_item_checked(heliosview_menu_t* menu, uint32_t id,
                                                    int checked);

/* Read whether the item with the given id is checked; 1/0 is written to
 * out_checked (NULL = ignore). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_is_item_checked(heliosview_menu_t* menu, uint32_t id,
                                                   int* out_checked);

/* Enable (enabled != 0) or disable (grayed out, not selectable) the item with
 * the given id. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_item_enabled(heliosview_menu_t* menu, uint32_t id,
                                                    int enabled);

/* Read whether the item with the given id is enabled; 1/0 is written to
 * out_enabled (NULL = ignore). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_is_item_enabled(heliosview_menu_t* menu, uint32_t id,
                                                   int* out_enabled);

/* Make (is_default != 0) or unmake the item with the given id the menu's
 * default item (shown bold; activated by Enter / double-click; one per menu).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_item_default(heliosview_menu_t* menu, uint32_t id,
                                                    int is_default);

/* Read whether the item with the given id is the default item; 1/0 is written
 * to out_default (NULL = ignore). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_is_item_default(heliosview_menu_t* menu, uint32_t id,
                                                   int* out_default);

/* Add a separator. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_separator(heliosview_menu_t* menu);

/* Add `submenu` as a submenu under `text`. The parent takes ownership of the
 * submenu. `submenu` must be a POPUP menu (heliosview_menu_create) — a menu bar
 * cannot be a submenu (Win32 MF_POPUP requires CreatePopupMenu handles).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_submenu(heliosview_menu_t* menu, const char* text,
                                               heliosview_menu_t* submenu);

/* Show the menu at the current cursor position. `window` is the owner: it
 * receives the resulting MENU_SELECT event (window_id = its native handle), and
 * the popup is dismissed when the user clicks elsewhere. Pass NULL to show it
 * without an application window: the library uses a hidden owner window and the
 * event carries window_id = 0.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_show(heliosview_menu_t* menu, heliosview_window_t* window);

/* ================= WebView (Windows: WebView2) =================
 *
 * A WebView handle independent of the window: it attaches to a parent window at
 * creation; afterwards operations involve only the webview itself, not
 * heliosview_window_t. Initialization is asynchronous; navigation requests made
 * before it completes are queued automatically (the last one wins).
 * Note: destroy the webview before the window; the window must not be destroyed
 * before initialization completes (usually a few milliseconds).
 *
 * Portability: the API is engine-neutral (Windows: WebView2; macOS: WKWebView;
 * Linux: WebKitGTK). Navigation, HTML loading, script evaluation, the JS bridge
 * (bind/resolve/reject/subscribe/broadcast), navigation callbacks, background
 * color and transparency, local-folder mapping, insets and the context-menu /
 * DevTools toggles all map to every engine. The few functions that depend on a
 * WebView2-only capability return HELIOSVIEW_ERROR_UNSUPPORTED (-4) elsewhere,
 * with the reason in heliosview_last_error_string; they are marked below.
 * Creation-time options that do not apply to an engine are ignored.
 */

typedef struct heliosview_webview heliosview_webview_t;

/* Why a navigation failed (heliosview_webview_navigation_cb). 0 = success; the
 * other values are portable — the engine's own code is available through
 * heliosview_webview_last_native_error. */
typedef enum heliosview_webview_error {
    HELIOSVIEW_WEBVIEW_OK = 0,
    HELIOSVIEW_WEBVIEW_ERROR_CANCELLED = 1,          /* navigation cancelled / aborted by the app */
    HELIOSVIEW_WEBVIEW_ERROR_HOST_NOT_FOUND = 2,     /* DNS / name resolution failed */
    HELIOSVIEW_WEBVIEW_ERROR_CONNECTION_FAILED = 3,  /* could not connect, disconnected, reset */
    HELIOSVIEW_WEBVIEW_ERROR_TIMEOUT = 4,
    HELIOSVIEW_WEBVIEW_ERROR_TLS = 5,                /* certificate / TLS problem */
    HELIOSVIEW_WEBVIEW_ERROR_HTTP = 6,               /* server returned an invalid response */
    HELIOSVIEW_WEBVIEW_ERROR_OTHER = 7,
} heliosview_webview_error_t;

/* Creation-time WebView2 environment options: the fields below are read when
 * the WebView2 environment is created and cannot be changed afterwards, so they
 * are only honored via heliosview_webview_create_ex. Zero-initialize the struct
 * (or pass NULL) for the pure runtime defaults and set only what you need.
 * Fields marked [Windows] have no equivalent on the other engines and are
 * ignored there.
 * String fields are UTF-8 and copied by the library; they may point at
 * temporary storage. Boolean fields are 0/1: 0 (the zero-initialized default)
 * = leave at the runtime default, 1 = enable. */
typedef struct heliosview_webview_env_opts {
    /* WebView2 browser data folder (profile, cache, cookies — the
     * <exe>.WebView2 folder). UTF-8 absolute path; NULL/"" = the default next
     * to the executable. Created by WebView2 if missing. */
    const char* user_data_folder;
    /* [Windows] Folder of a fixed WebView2 runtime (the directory that holds
     * msedgewebview2.exe). UTF-8; NULL = the system WebView2 Runtime. */
    const char* browser_executable_folder;
    /* Default page language / Accept-Language, e.g. "zh-CN". UTF-8;
     * NULL = the system default. */
    const char* language;
    /* [Windows] Extra Chromium command-line switches, e.g. "--disable-gpu".
     * UTF-8; NULL = none. */
    const char* additional_browser_arguments;
    /* [Windows] Target compatible browser version (used with
     * browser_executable_folder), e.g. "95.*"; NULL/"" = the latest available
     * on that runtime. UTF-8. */
    const char* target_compatible_browser_version;
    /* [Windows] Use the OS primary account for single sign-on (0/1). */
    int allow_sso_with_os_primary_account;
    /* [Windows] Exclusive access to the user data folder, so no other process
     * can share it (0/1). */
    int exclusive_user_data_folder_access;
    /* Tracking prevention (on by default in WebView2): 1 = turn it off,
     * 0 = keep it enabled. */
    int disable_tracking_prevention;
    /* [Windows] Browser extensions (e.g. ad blockers) enabled (0/1). */
    int are_browser_extensions_enabled;
} heliosview_webview_env_opts_t;

/* ================= WebView engine availability =================
 *
 * The WebView backend is the platform's web engine. On Windows that is the
 * WebView2 Runtime — a separate deploy-time component (preinstalled on Windows
 * 11, present on most but not all Windows 10 devices) — so an app may need to
 * know, before it creates a window or a WebView, whether an engine is available
 * and which version it is. The same call answers that on every platform:
 *
 *   Windows   WebView2 Runtime version, e.g. "131.0.2903.86". Empty = the
 *             Runtime is not installed; heliosview_webview_create/_ex then fail
 *             with HELIOSVIEW_ERROR_UNSUPPORTED (-4) and the window is left
 *             untouched, so an app can fall back to a non-WebView UI.
 *   macOS     The system WebKit version (part of the OS, always available).
 *   Linux     The WebKitGTK version, e.g. "2.44.3"; empty = the backend's web
 *             engine library is not present at runtime.
 *   other     Empty (no WebView backend on this platform yet).
 *
 * Writes a UTF-8 version string into buf (always NUL-terminated, truncated to
 * fit `size`; "" when no engine is available) and returns 0. Negative = invalid
 * arguments (buf == NULL or size == 0). */
HELIOSVIEW_API int heliosview_webview_engine_version(char* buf, size_t size);

/* Create a WebView in the parent window's client area (async initialization)
 * with creation-time WebView2 environment options (see
 * heliosview_webview_env_opts_t; NULL = all runtime defaults).
 * Returns NULL on failure: HELIOSVIEW_ERROR_UNSUPPORTED (-4) when no web engine
 * is available (see heliosview_webview_engine_version). */
HELIOSVIEW_API heliosview_webview_t* heliosview_webview_create_ex(
    heliosview_window_t* parent, const heliosview_webview_env_opts_t* opts);

/* Create a WebView in the parent window's client area (async initialization;
 * all WebView2 environment options at their runtime defaults).
 * Returns NULL on failure (see heliosview_webview_create_ex). */
HELIOSVIEW_API heliosview_webview_t* heliosview_webview_create(heliosview_window_t* parent);

/* Destroy the WebView (must be called before destroying the parent window) */
HELIOSVIEW_API void heliosview_webview_destroy(heliosview_webview_t* webview);

/* Navigate to a URL (queued if initialization is not complete) */
HELIOSVIEW_API int heliosview_webview_navigate(heliosview_webview_t* webview, const char* url);

HELIOSVIEW_API int heliosview_webview_navigate_html(heliosview_webview_t* webview, const char* html);

/* ================= WebView low-footprint mode (suspend / resume) =================
 *
 * WebView2 TrySuspend / Resume: suspending stops rendering and releases most of
 * the browser process's resources while the page stays alive; resume() restores
 * it where it left off. The typical use: suspend when the window is hidden (or
 * the app goes to the background), resume when it is shown again.
 *
 * [Windows only] — the other engines have no equivalent; these functions return
 * HELIOSVIEW_ERROR_UNSUPPORTED (-4) there (an app that needs the memory back can
 * hide the webview, or destroy and recreate it).
 */

/* Completion callback for heliosview_webview_suspend: error is 0 on success,
 * suspended is 1 if the WebView actually suspended (TrySuspend can decline,
 * e.g. while audio is playing). Runs on the UI thread. */
typedef void (*heliosview_webview_suspend_cb)(int error, int suspended, void* userdata);

/* Suspend the WebView (async; completion via callback, may be NULL for
 * fire-and-forget). A request made before initialization is recorded and
 * applied when the core becomes ready (after any queued navigation/scripts).
 * 0 = success (request accepted), negative = error code. */
HELIOSVIEW_API int heliosview_webview_suspend(heliosview_webview_t* webview,
                                              heliosview_webview_suspend_cb callback,
                                              void* userdata);

/* Resume a suspended WebView (synchronous; no-op if not suspended).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_webview_resume(heliosview_webview_t* webview);

/* Read whether the WebView is currently suspended; 1/0 is written to
 * out_suspended. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_webview_is_suspended(heliosview_webview_t* webview,
                                                   int* out_suspended);

/* ================= WebView background color =================
 *
 * WebView2 DefaultBackgroundColor (ICoreWebView2Controller2): the color the
 * WebView paints behind the page content (default: opaque white). An alpha of
 * 0 makes the background transparent — the parent window's own content shows
 * through the WebView (to see the desktop through it, the parent window must
 * itself be transparent, e.g. a layered window). Channels are (red, green,
 * blue, alpha), each 0-255.
 */

/* Set the WebView's default background color. Applies immediately when the
 * core is initialized, otherwise when it becomes ready. 0 = success,
 * negative = error code. */
HELIOSVIEW_API int heliosview_webview_set_background_color(heliosview_webview_t* webview,
                                                           uint8_t red, uint8_t green,
                                                           uint8_t blue, uint8_t alpha);

/* Convenience: make the WebView background transparent (transparent != 0) or
 * restore the default opaque white. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_webview_set_transparent_background(heliosview_webview_t* webview,
                                                                 int transparent);

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
 * Threading: every WebView API is a UI-thread call — resolve / reject /
 * broadcast included (they do not marshal). eval / eval_async are queued while
 * the WebView initializes.
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

/* Callback for heliosview_webview_eval_async. On success (error == 0),
 * result_json is the JSON encoding of the script's completion value. On failure,
 * error is negative and result_json carries the script's error message text
 * (not JSON). Runs on the UI thread. */
typedef void (*heliosview_webview_eval_cb)(int error, const char* result_json, void* userdata);

/* Callback for a broadcast subscription: fires when the page posts a message to
 * its BroadcastChannel(name) instance(s). data_json is the posted value, which
 * may be any JSON type ("" when the message had no data). Runs on the UI thread. */
typedef void (*heliosview_webview_subscribe_cb)(heliosview_webview_t* webview,
                                                const char* name, const char* data_json,
                                                void* userdata);

/* Callback for navigation events: fires when a navigation completes (page fully
 * loaded) or fails. error is HELIOSVIEW_WEBVIEW_OK (0) on success, else a
 * portable heliosview_webview_error_t value; the engine's own error code is
 * available through heliosview_webview_last_native_error.
 * Runs on the UI thread. Only one callback may be registered; setting a new one
 * replaces the previous (running its dtor). */
typedef void (*heliosview_webview_navigation_cb)(heliosview_webview_t* webview,
                                                 int error, void* userdata);

/* The engine's own code for the last completed navigation: a
 * COREWEBVIEW2_WEB_ERROR_STATUS_* value on Windows, an NSURLError code on
 * macOS, a GError code on Linux. 0 = success, or the engine reported no specific
 * code — use the navigation callback's portable error to tell the two apart.
 * Diagnostic. */
HELIOSVIEW_API int heliosview_webview_last_native_error(heliosview_webview_t* webview);

/* Register a native function under `name`, callable from JS via
 * window.helios.call(name, ...). Rebinding a name replaces the previous binding
 * and calls its dtor (if any). dtor(userdata) also runs when the WebView is
 * destroyed. UI-thread call.
 * Internal names: the library's built-in bridge uses "__hv."-prefixed names
 * (__hv.control / __hv.state / __hv.drag, called by the injected
 * <helios-window-controls> / <helios-window-title-bar> components). They contain
 * a dot, so they are not valid C identifiers and applications cannot bind (or
 * subscribe) them — the call fails with -2 like any invalid name. */
HELIOSVIEW_API int heliosview_webview_bind(heliosview_webview_t* webview, const char* name,
                                           heliosview_webview_bind_cb callback, void* userdata,
                                           heliosview_webview_userdata_dtor dtor);

/* The WebView instance no longer exists: returned by the bridge calls
 * (heliosview_webview_resolve / _reject / _broadcast) when the WebView
 * was already destroyed — e.g. destroyWebView/heliosview_webview_destroy ran
 * while this asynchronous call was still in flight. The call then never touches
 * the freed instance. */
#define HELIOSVIEW_WEBVIEW_DESTROYED (-3)

/* Resolve a pending JS Promise: result_json is any valid JSON value. UI-thread call.
 * Returns -3 (HELIOSVIEW_WEBVIEW_DESTROYED) when the WebView instance no longer
 * exists — e.g. destroyWebView/heliosview_webview_destroy ran while this
 * asynchronous call was still in flight. The call then never touches the freed
 * instance, so the misuse fails with a clear error code instead of a crash. */
HELIOSVIEW_API int heliosview_webview_resolve(heliosview_webview_t* webview,
                                              uint64_t call_id, const char* result_json);

/* Reject a pending JS Promise: error_json is any valid JSON value. UI-thread call.
 * Same stale-instance guard as resolve: returns -3 when the WebView was already
 * destroyed. */
HELIOSVIEW_API int heliosview_webview_reject(heliosview_webview_t* webview,
                                             uint64_t call_id, const char* error_json);

/* Run a JavaScript string (fire-and-forget). UI-thread call; queued while the
 * WebView is still initializing. */
HELIOSVIEW_API int heliosview_webview_eval(heliosview_webview_t* webview, const char* script);

/* Run a JavaScript string and get its JSON completion value. UI-thread call; queued
 * while the WebView is still initializing. The callback fires exactly once.
 * A returned Promise is awaited, so "fetch(...).then(r => r.json())" resolves to
 * the JSON value (the engine's own script evaluation returns the promise object
 * instead). A script that throws, or a rejected promise, reports a negative
 * error and the message text. */
HELIOSVIEW_API int heliosview_webview_eval_async(heliosview_webview_t* webview, const char* script,
                                                 heliosview_webview_eval_cb callback, void* userdata);

/* Broadcast a JSON value to the JS page's BroadcastChannel(name) instances; the
 * page receives it as a standard 'message' event. UI-thread call. Same
 * stale-instance guard as resolve: returns -3 when the WebView was already
 * destroyed. */
HELIOSVIEW_API int heliosview_webview_broadcast(heliosview_webview_t* webview,
                                                const char* name, const char* data_json);

/* Subscribe to broadcasts the page posts via its BroadcastChannel(name) instances:
 * callback(name, data_json, userdata) fires on the UI thread for every postMessage
 * to a channel of that name. Subscribing to a name replaces the previous
 * subscription (calling its dtor). dtor(userdata) also runs when the WebView is
 * destroyed. UI-thread call. The internal "__hv.*" bridge names (see
 * heliosview_webview_bind) are not valid identifiers and cannot be subscribed. */
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
 * (e.g. aborted). UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_navigation_callback(heliosview_webview_t* webview,
                                                              heliosview_webview_navigation_cb callback,
                                                              void* userdata,
                                                              heliosview_webview_userdata_dtor dtor);

/* Register a navigation-starting callback (replacing any previous one and
 * running its dtor). Fires on the UI thread just before a navigation begins;
 * returning non-zero cancels it (e.g. to block cross-origin or external links).
 * UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_navigation_starting_callback(
    heliosview_webview_t* webview,
    heliosview_webview_navigation_starting_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Register a source-changed (URL-changed) callback (replacing any previous one
 * and running its dtor). Fires on the UI thread whenever the WebView's current
 * URL changes. UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_source_changed_callback(
    heliosview_webview_t* webview,
    heliosview_webview_source_changed_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Register a document-title-changed callback (replacing any previous one and
 * running its dtor). Fires on the UI thread when the page title changes.
 * UI-thread call. */
HELIOSVIEW_API int heliosview_webview_set_title_changed_callback(
    heliosview_webview_t* webview,
    heliosview_webview_title_changed_cb callback,
    void* userdata,
    heliosview_webview_userdata_dtor dtor);

/* Map a local folder to a virtual host name so the page can load files from it
 * through heliosview_webview_local_url(). Used to serve images or other local
 * assets that are not part of the packaged frontend (game banners, avatars, ...).
 * Call before navigating, or the page must be reloaded for new mappings to take
 * effect.
 * Windows (WebView2) restricts mappings to the "trusted origin" host suffix
 * .local, e.g. "assets.local".
 * Returns 0 = success, negative = failure. */
HELIOSVIEW_API int heliosview_webview_map_local_folder(heliosview_webview_t* webview,
                                                       const char* host_name,
                                                       const char* folder_path);

/* Build the URL that serves `path` from the folder mapped to `host_name` (see
 * heliosview_webview_map_local_folder). The URL shape is engine-defined:
 * Windows produces "https://<host>/<path>", other engines register a custom
 * scheme instead — this helper is what keeps the difference out of application
 * code, so never build the URL by hand.
 * Writes a NUL-terminated URL into buf (truncated to fit `size`); `path` is
 * appended as given (percent-encode it yourself if it contains special
 * characters). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_webview_local_url(heliosview_webview_t* webview,
                                                const char* host_name, const char* path,
                                                char* buf, size_t size);

/* Keep the given insets (client pixels) clear around the WebView: the WebView
 * occupies the parent client area minus these insets on each side, and the
 * cleared strips remain the parent window's own surface. Re-applied on every
 * window resize. Useful when the window keeps native chrome of its own around
 * the WebView (e.g. a header strip drawn by the app). Zero insets restore the
 * default (WebView fills the client area). Applies immediately when the WebView
 * is initialized; when called during initialization the bounds are applied when
 * it becomes ready. Negative insets are clamped to 0. Message-loop thread.
 * 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_insets(heliosview_webview_t* webview,
                                                 int32_t top, int32_t right,
                                                 int32_t bottom, int32_t left);

/* Show (enabled != 0) or hide the WebView2 status bar, which displays the
 * target URL of a hovered link at the bottom-left corner of the WebView.
 * Disabled by default. Applies immediately when the WebView is initialized;
 * when called during initialization the setting is applied when it becomes
 * ready. [Windows only] — other engines return -4 (draw one in the page if
 * needed). Message-loop thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_status_bar(heliosview_webview_t* webview,
                                                     int enabled);

/* Enable (enabled != 0) or disable the WebView2 default right-click context
 * menu (copy/paste/inspect etc.). Enabled by default. Applies immediately
 * when the WebView is initialized; when called during initialization the
 * setting is applied when it becomes ready. On WebView2 runtimes older than
 * 100 the toggle has no effect and the menu always shows. Message-loop
 * thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_context_menu(heliosview_webview_t* webview,
                                                       int enabled);

/* Enable (enabled != 0) or disable WebView2 DevTools (F12, right-click
 * Inspect). When disabled, DevTools cannot be opened and an already-open
 * DevTools window is closed. Enabled by default. Applies immediately when
 * the WebView is initialized; when called during initialization the setting
 * is applied when it becomes ready. Message-loop thread. 0 = success,
 * negative = error. */
HELIOSVIEW_API int heliosview_webview_set_devtools(heliosview_webview_t* webview,
                                                   int enabled);

/* Enable (enabled != 0) or disable WebView2's built-in window controls overlay
 * (the min/max/restore/close buttons WebView2 draws over the page's top-right
 * corner). Disabled by default — apps that render their own title-bar buttons
 * (e.g. the injected <helios-window-controls> component) leave it off. Applies
 * immediately when the WebView is initialized; when called during
 * initialization the setting is applied when it becomes ready. Requires the
 * experimental WebView2 interface; on runtimes without it the call returns
 * negative and has no effect. [Windows only] — other engines return -4 (they
 * use the native title bar). Message-loop thread. 0 = success,
 * negative = error. */
HELIOSVIEW_API int heliosview_webview_set_window_controls_overlay(
    heliosview_webview_t* webview, int enabled);

/* Set the window controls overlay's background color (red/green/blue/alpha).
 * Default: fully transparent (alpha 0) — the page's own title bar shows
 * through and the buttons float over it. Applies immediately when the overlay
 * exists; when called before it is enabled the color is applied when the
 * overlay is created. Message-loop thread. 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_webview_set_window_controls_background_color(
    heliosview_webview_t* webview, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);

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

/* File dialog filter rule (display name and semicolon-separated extensions) */
typedef struct heliosview_file_filter {
    const char* name;        /* filter display name, e.g. "Image files" or "Text documents" */
    const char* extensions;  /* semicolon-separated extensions, e.g. "png;jpg;jpeg" or "*.png;*.jpg" */
} heliosview_file_filter_t;

/* Open-file dialog. `filters` is an array of `filter_count` filters (NULL or 0 = "All files");
 * `multi` != 0 enables multi-selection. On success (n > 0) out_paths receives a NULL-terminated
 * array of n UTF-8 paths (each string and the array itself are freed with heliosview_free, or in one
 * call with heliosview_free_paths). 0 = cancelled, negative = error. */
HELIOSVIEW_API int heliosview_open_files(heliosview_window_t* window, const char* title,
                                         const heliosview_file_filter_t* filters, size_t filter_count,
                                         int multi, char*** out_paths);

/* Free a path array returned by heliosview_open_files (each string and the array). NULL is ignored. */
HELIOSVIEW_API void heliosview_free_paths(char** paths);

/* Save-file dialog. On success (1) out_path receives the chosen path (heliosview_free). */
HELIOSVIEW_API int heliosview_save_file(heliosview_window_t* window, const char* title,
                                        const heliosview_file_filter_t* filters, size_t filter_count,
                                        const char* default_name,
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
    HELIOSVIEW_MESSAGE_ABORT_RETRY_IGNORE,
} heliosview_message_buttons_t;

typedef enum heliosview_message_result {
    HELIOSVIEW_MESSAGE_RESULT_NONE = 0,
    HELIOSVIEW_MESSAGE_RESULT_OK,
    HELIOSVIEW_MESSAGE_RESULT_CANCEL,
    HELIOSVIEW_MESSAGE_RESULT_YES,
    HELIOSVIEW_MESSAGE_RESULT_NO,
    HELIOSVIEW_MESSAGE_RESULT_RETRY,
    HELIOSVIEW_MESSAGE_RESULT_ABORT,
    HELIOSVIEW_MESSAGE_RESULT_IGNORE,
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

#endif /* HELIOSVIEW_HELIOSVIEW_H */
