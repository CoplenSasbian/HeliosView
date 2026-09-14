#ifndef HELIOSVIEW_HELIOSVIEW_EVENT_H
#define HELIOSVIEW_HELIOSVIEW_EVENT_H

/**
 * HeliosView C API -- events, the event queue and native-message hooks
 *
 * The event model: the event and keycode enums, the event record, the queue
 * (poll/wait/post), the native-message filter pipeline, and the native-message
 * handlers. A windowing application reads this header to consume events.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>

#ifdef __cplusplus
extern "C" {
#endif

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


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_EVENT_H */
