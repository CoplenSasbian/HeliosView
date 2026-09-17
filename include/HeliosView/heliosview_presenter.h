#ifndef HELIOSVIEW_HELIOSVIEW_PRESENTER_H
#define HELIOSVIEW_HELIOSVIEW_PRESENTER_H

/**
 * HeliosView C API -- Window buffer presentation (presenter / bit-blit to window).
 *
 * A BufferPresenter is a middleware component that mounts onto a window
 * (heliosview_window_t) and presents raw pixel views (heliosview_pixel_view_t)
 * directly into the window client area.
 *
 * Features:
 *   - Zero dependency on any drawing engine (Canvas / Skia / Blend2D / GDI+).
 *     Accepts any buffer described by heliosview_pixel_view_t.
 *   - Zero-flicker double-buffered presentation. Intercepts WM_ERASEBKGND
 *     and manages WM_PAINT via clean window subclassing.
 *   - Automatically retains the most recently presented frame to repaint
 *     on OS expose/damage events.
 *   - Optional resize / paint callbacks for dynamic rendering pipelines.
 */

#include <stdint.h>
#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heliosview_buffer_presenter heliosview_buffer_presenter_t;

/* Callback when the host window client area size changes. */
typedef void (*heliosview_presenter_resize_cb)(heliosview_buffer_presenter_t* presenter,
                                              int32_t width, int32_t height,
                                              void* user_data);

/* Callback when the host window requests a repaint (e.g. WM_PAINT). */
typedef void (*heliosview_presenter_paint_cb)(heliosview_buffer_presenter_t* presenter,
                                             void* user_data);

/* Create and attach a buffer presenter to a window. Returns NULL on failure. */
HELIOSVIEW_API heliosview_buffer_presenter_t* heliosview_buffer_presenter_create(heliosview_window_t* window);

/* Create and attach a buffer presenter directly to a native window handle (HWND on Windows). Returns NULL on failure. */
HELIOSVIEW_API heliosview_buffer_presenter_t* heliosview_buffer_presenter_create_for_hwnd(void* hwnd);

/* Destroy and detach a buffer presenter. */
HELIOSVIEW_API void heliosview_buffer_presenter_destroy(heliosview_buffer_presenter_t* presenter);

/* Present a pixel view to the window client area (placed at 0, 0).
 * Copies/blits the pixel view to the window and stores it for repaints.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_buffer_presenter_present(heliosview_buffer_presenter_t* presenter,
                                                      const heliosview_pixel_view_t* view);

/* Present a pixel view to a specific destination offset (dst_x, dst_y).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_buffer_presenter_present_rect(heliosview_buffer_presenter_t* presenter,
                                                           const heliosview_pixel_view_t* view,
                                                           int32_t dst_x, int32_t dst_y);

/* Request the window to repaint immediately. */
HELIOSVIEW_API int heliosview_buffer_presenter_invalidate(heliosview_buffer_presenter_t* presenter);

/* Set callback for client-area resize notifications. */
HELIOSVIEW_API void heliosview_buffer_presenter_set_resize_callback(heliosview_buffer_presenter_t* presenter,
                                                                   heliosview_presenter_resize_cb cb,
                                                                   void* user_data);

/* Set callback for OS repaint requests. */
HELIOSVIEW_API void heliosview_buffer_presenter_set_paint_callback(heliosview_buffer_presenter_t* presenter,
                                                                  heliosview_presenter_paint_cb cb,
                                                                  void* user_data);

/* Query the associated window handle. */
HELIOSVIEW_API heliosview_window_t* heliosview_buffer_presenter_get_window(const heliosview_buffer_presenter_t* presenter);

#if defined(_WIN32)
/* Render the current presenter frame directly to a target Win32 DC.
 * clip_left/top/right/bottom specify a clip rectangle (all zeroes disables clipping).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_buffer_presenter_render_to_dc(heliosview_buffer_presenter_t* presenter,
                                                            void* hdc,
                                                            int32_t clip_left, int32_t clip_top,
                                                            int32_t clip_right, int32_t clip_bottom);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_PRESENTER_H */
