#ifndef HELIOSVIEW_HELIOSVIEW_BASE_H
#define HELIOSVIEW_HELIOSVIEW_BASE_H

/**
 * HeliosView C API -- baseline declarations shared by the whole interface.
 *
 * These are the few declarations that more than one public header needs: the
 * standard error codes, the opaque window handle, and the rectangle type. They live
 * here so the umbrella header (heliosview.h) and the canvas header
 * (heliosview_canvas.h) can both be included first, in either order, without a
 * circular include -- a canvas-only translation unit never has to pull in the
 * windowing API just to name a rectangle.
 *
 * Part of the public C ABI: the declarations here are stable and platform
 * independent, like everything else under include/HeliosView/.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Standard error codes =================
 *
 * The shared part of the return convention documented in heliosview.h: 0 = success,
 * negative = an error code, positive = a payload (a count of items or characters).
 * Other negative values are the platform's own code (a negated Win32 error status)
 * and large positive ones are negated HRESULTs; read heliosview_last_error_string
 * for the actual reason behind the most recent failure. */
#define HELIOSVIEW_SUCCESS                 0
#define HELIOSVIEW_ERROR_GENERIC          (-1)
#define HELIOSVIEW_ERROR_INVALID_ARGUMENT (-2)
#define HELIOSVIEW_WEBVIEW_DESTROYED      (-3)
#define HELIOSVIEW_ERROR_UNSUPPORTED      (-4)

/* ================= Opaque handles =================
 *
 * Forward declarations only -- the layout is private to the library, and these are
 * all the parts need to name the handle types. (heliosview_menu_t and
 * heliosview_action_t are used across the menu / tray headers, so they belong here
 * rather than in whichever header happens to define them first.) */

/* A top-level window (see heliosview_window.h) */
typedef struct heliosview_window heliosview_window_t;

/* A shareable menu command (see heliosview_menu.h) */
typedef struct heliosview_action heliosview_action_t;

/* A popup menu / menu bar (see heliosview_menu.h) */
typedef struct heliosview_menu heliosview_menu_t;

/* ================= Geometry =================
 *
 * A rectangle in the coordinate system of whatever it belongs to: screen
 * coordinates with the origin at the top-left of the primary display and y growing
 * downward (heliosview.h), or a canvas' own pixel space (heliosview_canvas.h). Width
 * and height are positive. */
typedef struct heliosview_rect {
    int32_t x;      /* left */
    int32_t y;      /* top */
    int32_t width;  /* positive */
    int32_t height; /* positive */
} heliosview_rect_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_BASE_H */
