#ifndef HELIOSVIEW_HELIOSVIEW_SCREEN_H
#define HELIOSVIEW_HELIOSVIEW_SCREEN_H

/**
 * HeliosView C API -- screen and monitor geometry
 *
 * Work-area and cursor queries: enough multi-monitor and DPI-aware geometry to place
 * a window correctly without the windowing API.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

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


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_SCREEN_H */
