#ifndef HELIOSVIEW_HELIOSVIEW_HOST_H
#define HELIOSVIEW_HELIOSVIEW_HOST_H

/**
 * HeliosView C API -- Child Viewport Host & Subclass Architecture
 *
 * A Host represents an isolated child viewport (a child window) inside a parent Window.
 * Subclasses (such as UI/Canvas rendering or embedded WebView) are attached to a Host,
 * decoupling OS window management from presentation logic.
 *
 * Part of the public C ABI; 0 platform dependencies.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>
#include <HeliosView/heliosview_canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heliosview_host heliosview_host_t;

/* ================= Generic Host (Child Window) ================= */

/**
 * Destroy the host and free its resources.
 * Detaches any active subclass before destroying the child window.
 */
HELIOSVIEW_API void heliosview_host_destroy(heliosview_host_t* host);

/**
 * Set the bounds (x, y, width, height) of the host relative to the parent window's client area.
 */
HELIOSVIEW_API void heliosview_host_set_bounds(heliosview_host_t* host, int x, int y, int width, int height);

/**
 * Get the current bounds of the host relative to the parent window's client area.
 * Any pointer can be NULL if that coordinate is not needed.
 */
HELIOSVIEW_API void heliosview_host_get_bounds(const heliosview_host_t* host, int* x, int* y, int* width, int* height);

/**
 * Set host visibility (1 = visible, 0 = hidden).
 */
HELIOSVIEW_API void heliosview_host_set_visible(heliosview_host_t* host, int visible);

/**
 * Check if the host is currently visible. Returns 1 for visible, 0 for hidden.
 */
HELIOSVIEW_API int heliosview_host_is_visible(const heliosview_host_t* host);

/**
 * Get the parent window this host is attached to.
 */
HELIOSVIEW_API heliosview_window_t* heliosview_host_get_parent(const heliosview_host_t* host);

/**
 * Native escape hatch to retrieve the underlying OS handle (HWND on Windows).
 */
HELIOSVIEW_API void* heliosview_host_native_handle(heliosview_host_t* host);

/**
 * Retrieve the subclass data pointer attached to this host.
 * Returns NULL if no subclass is attached.
 */
HELIOSVIEW_API void* heliosview_host_get_subclass(const heliosview_host_t* host);


/* ================= UI Subclass (Self-drawn Canvas Viewport) =================
 *
 * Attaches a Canvas and a BufferPresenter to the host.
 * Automatically manages backbuffer resizing and paint dispatching.
 */

/**
 * Create a Host pre-configured with a UI/Canvas subclass.
 */
HELIOSVIEW_API heliosview_host_t* heliosview_host_create_ui(
    heliosview_window_t* parent, int x, int y, int width, int height, heliosview_canvas_engine_t engine);

/**
 * Get the Canvas owned by the host's UI subclass.
 * Returns NULL if host does not have an active UI subclass.
 */
HELIOSVIEW_API heliosview_canvas_t* heliosview_host_ui_get_canvas(heliosview_host_t* host);

/**
 * Present the current Canvas contents to the host viewport.
 */
HELIOSVIEW_API void heliosview_host_ui_present(heliosview_host_t* host);

/**
 * Request a repaint for the UI host (triggers an internal paint cycle).
 */
HELIOSVIEW_API void heliosview_host_ui_request_repaint(heliosview_host_t* host);

/**
 * UI paint callback: called when the host needs to be repainted.
 * Parameters: host, painter, user_data.
 */
typedef void (*heliosview_host_ui_paint_cb)(heliosview_host_t* host, heliosview_painter_t* painter, void* user_data);

HELIOSVIEW_API void heliosview_host_ui_set_paint_callback(
    heliosview_host_t* host, heliosview_host_ui_paint_cb callback, void* user_data);

/**
 * UI mouse event types for local host hit-testing.
 */
typedef enum heliosview_host_mouse_action {
    HELIOSVIEW_HOST_MOUSE_MOVE = 0,
    HELIOSVIEW_HOST_MOUSE_DOWN = 1,
    HELIOSVIEW_HOST_MOUSE_UP   = 2,
    HELIOSVIEW_HOST_MOUSE_LEAVE = 3,
} heliosview_host_mouse_action_t;

typedef struct heliosview_host_mouse_event {
    heliosview_host_mouse_action_t action;
    int x; /* Local coordinates relative to host's top-left */
    int y;
    int button; /* 1 = Left, 2 = Right, 3 = Middle */
} heliosview_host_mouse_event_t;

typedef void (*heliosview_host_ui_mouse_cb)(heliosview_host_t* host, const heliosview_host_mouse_event_t* event, void* user_data);

HELIOSVIEW_API void heliosview_host_ui_set_mouse_callback(
    heliosview_host_t* host, heliosview_host_ui_mouse_cb callback, void* user_data);

/* ================= WebView Subclass =================
 *
 * Attaches an embedded web engine (WebView2 on Windows) to the host.
 * The host acts as the child window container for the WebView controller.
 */

struct heliosview_webview_env_opts;

/**
 * Create a Host pre-configured with a WebView subclass.
 */
HELIOSVIEW_API heliosview_host_t* heliosview_host_create_webview(
    heliosview_window_t* parent, int x, int y, int width, int height);

HELIOSVIEW_API heliosview_host_t* heliosview_host_create_webview_ex(
    heliosview_window_t* parent, int x, int y, int width, int height,
    const struct heliosview_webview_env_opts* opts);

/**
 * Retrieve the WebView subclass pointer from the host.
 * Returns NULL if the host does not have a WebView subclass.
 */
HELIOSVIEW_API heliosview_webview_t* heliosview_host_get_webview(heliosview_host_t* host);

#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_HOST_H */

