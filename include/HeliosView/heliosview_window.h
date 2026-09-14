#ifndef HELIOSVIEW_HELIOSVIEW_WINDOW_H
#define HELIOSVIEW_HELIOSVIEW_WINDOW_H

/**
 * HeliosView C API -- top-level windows
 *
 * Creating and driving a top-level window: geometry, state, chrome, taskbar
 * integration and the Win11 backdrop. Event types are in heliosview_event.h.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>
#include <HeliosView/heliosview_screen.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Windows ================= */

/* heliosview_window_t is declared in heliosview_base.h */

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


/* (heliosview_rect_t is defined in heliosview_base.h -- see Screen / monitor
 * geometry below for how it is used.) */

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


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_WINDOW_H */
