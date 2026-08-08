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
 * Event model:
 *   - heliosview_run runs the message loop, converting native messages via a
 *     (registerable) conversion delegate into queued heliosview_event_t events
 *   - heliosview_poll / heliosview_wait dequeue events from the queue
 *   - heliosview_post_event posts events from any thread
 *
 * C++ users should include <HeliosViewCore/HeliosView.h> (the HeliosView.Core wrapper).
 */

#include <stdint.h>

#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Version ================= */

HELIOSVIEW_API const char* heliosview_version(void);

/* ================= Events ================= */

typedef enum heliosview_event_type {
    HELIOSVIEW_EVENT_QUIT = 1,          /* Quit request (posted via heliosview_post_event) */
    HELIOSVIEW_EVENT_WINDOW_CLOSE,      /* Window close request (user clicked X) */
    HELIOSVIEW_EVENT_WINDOW_RESIZE,     
    HELIOSVIEW_EVENT_KEY_DOWN,
    HELIOSVIEW_EVENT_KEY_UP,
    HELIOSVIEW_EVENT_MOUSE_MOVE,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_UP
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

/* Event: flat POD, safe to pass across the DLL boundary */
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
} heliosview_event_t;

/* ================= Event queue =================
 *
 * Lock-free: the queue is only accessed by the message-loop thread (posted and
 * consumed on the same thread); cross-thread posting requires external sync. */

/* Non-blocking fetch: 1 = event written to out, 0 = queue empty */
HELIOSVIEW_API int heliosview_poll(heliosview_event_t* out_event);

/* Blocking fetch (polling, 1 ms granularity): 1 = event, -1 = quit request
 * (heliosview_quit), 0 = other error. Blocks the calling thread; do not use it
 * on the message-loop thread. */
HELIOSVIEW_API int heliosview_wait(heliosview_event_t* out_event);

/* Post an event (message-loop thread). timestamp_ms is filled automatically when 0. */
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
 *  -1  -> not handled; fall back to the library's built-in conversion
 * If no delegate is registered, all messages use the built-in conversion. */
typedef int (*heliosview_native_handler_fn)(void* native_msg, heliosview_event_t* out_event);

HELIOSVIEW_API void heliosview_set_native_handler(heliosview_native_handler_fn handler);

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
                                                                const char* title,
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

/* Window id (source of window_id in events) */
HELIOSVIEW_API int32_t heliosview_window_id(const heliosview_window_t* window);

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

/* ================= Async I/O (background thread pool + platform multiplexer) =================
 *
 * Cross-platform design: these APIs are platform-neutral; each platform just
 * implements the corresponding .cpp (the Windows implementation is based on
 * IOCP, see src/win32/heliosview_io_win32.cpp).
 *
 * Thread model: all callbacks run on background worker threads (may run
 * concurrently); shared state must be synchronized by the caller.
 * Error codes: 0 = success; negative = failure (negated platform error code).
 * Buffer lifetime:
 *   - tcp_write / file_write data must stay valid until the callback fires (the C++ layer copies it)
 *   - file_read buf must stay valid until the callback fires
 *   - once the callback fires, the operation is complete; no further callbacks
 */

typedef struct heliosview_loop heliosview_loop_t;
typedef struct heliosview_tcp heliosview_tcp_t;
typedef struct heliosview_file heliosview_file_t;

/* Callback with no result (thread-pool tasks, etc.) */
typedef void (*heliosview_completion_cb)(int error, void* userdata);
/* Transfer callback (with byte count) */
typedef void (*heliosview_transfer_cb)(int error, uint32_t bytes, void* userdata);
/* Streaming read callback: data is valid only during the callback; error=0 with len=0 means the peer closed */
typedef void (*heliosview_read_cb)(int error, const char* data, uint32_t len, void* userdata);
/* Connect callback: tcp is valid when error=0 (the caller closes it after success) */
typedef void (*heliosview_tcp_connect_cb)(int error, heliosview_tcp_t* tcp, void* userdata);
/* Open callback: file is valid when error=0 */
typedef void (*heliosview_file_open_cb)(int error, heliosview_file_t* file, void* userdata);

/* ---- Thread pool + multiplexer ---- */

/* Create: start background worker threads (0 = hardware concurrency). Returns NULL on failure. */
HELIOSVIEW_API heliosview_loop_t* heliosview_loop_create(unsigned thread_count);

/* Destroy: stop and wait for the worker threads to exit. Ensure no operations are pending before calling. */
HELIOSVIEW_API void heliosview_loop_destroy(heliosview_loop_t* loop);

/* Block the calling thread until heliosview_loop_stop. 0 = normal. */
HELIOSVIEW_API int heliosview_loop_run(heliosview_loop_t* loop);

/* Request stop (worker threads exit after finishing posted tasks) */
HELIOSVIEW_API void heliosview_loop_stop(heliosview_loop_t* loop);

/* Post a task to the thread pool: fn runs on a worker thread (error is always 0) */
HELIOSVIEW_API int heliosview_loop_post(heliosview_loop_t* loop, heliosview_completion_cb fn, void* userdata);

/* ---- Async TCP client ---- */

HELIOSVIEW_API int heliosview_tcp_connect(heliosview_loop_t* loop, const char* host, uint16_t port,
                                          heliosview_tcp_connect_cb on_connect, void* userdata);
HELIOSVIEW_API int heliosview_tcp_write(heliosview_tcp_t* tcp, const void* data, uint32_t len,
                                        heliosview_transfer_cb on_write, void* userdata);
/* Start streaming read: callback once per chunk and auto-resume until EOF/error/read_stop */
HELIOSVIEW_API int heliosview_tcp_read_start(heliosview_tcp_t* tcp, heliosview_read_cb on_read, void* userdata);
/* Stop reading: cancels pending reads; one callback may still be in flight. Not needed after the end callback (EOF/error). */
HELIOSVIEW_API void heliosview_tcp_read_stop(heliosview_tcp_t* tcp);
/* Close the connection (pending writes complete with an error callback). Do not use the handle after closing. */
HELIOSVIEW_API void heliosview_tcp_close(heliosview_tcp_t* tcp);

/* ---- Async file ---- */

/* write_mode != 0: create/truncate for writing; otherwise open an existing file read-only */
HELIOSVIEW_API int heliosview_file_open(heliosview_loop_t* loop, const char* path, int write_mode,
                                        heliosview_file_open_cb on_open, void* userdata);
HELIOSVIEW_API int heliosview_file_read(heliosview_file_t* file, void* buf, uint32_t len, int64_t offset,
                                        heliosview_transfer_cb on_read, void* userdata);
HELIOSVIEW_API int heliosview_file_write(heliosview_file_t* file, const void* buf, uint32_t len, int64_t offset,
                                         heliosview_transfer_cb on_write, void* userdata);
HELIOSVIEW_API void heliosview_file_close(heliosview_file_t* file);

#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_H */
