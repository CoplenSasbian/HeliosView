#ifndef HELIOSVIEW_HELIOSVIEW_LOOP_H
#define HELIOSVIEW_HELIOSVIEW_LOOP_H

/**
 * HeliosView C API -- message loop and loop timers
 *
 * Running the message loop (heliosview_run / pump) and the timers that fire on the
 * message-loop thread without a window.
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

/* ================= Message loop ================= */

/* Called once after each pump drains the queue; return non-zero to exit the loop */
typedef int (*heliosview_loop_callback)(void* userdata);

/* Pump all pending native messages once and queue them as events (non-blocking) */
HELIOSVIEW_API void heliosview_pump_events(void);

/* Message loop: pump native messages, convert and queue, then call
 * frame_callback (application frame logic).
 * 0 = normal exit (heliosview_quit / WM_QUIT / callback returned non-zero) */
HELIOSVIEW_API int heliosview_run(heliosview_loop_callback frame_callback, void* userdata);

/* ================= Loop timers (message-loop thread) =================
 *
 * One-shot (delay) and repeating (interval) callbacks scheduled on the
 * message-loop thread. The loop is woken at a timer's due time rather than
 * polling, so an idle process sleeps until its next scheduled task. This is the
 * UI-thread timer: the callback runs on the same thread as the event loop and
 * may touch windows. It is intentionally separate from the asio-based timers in
 * HeliosViewCore/Async.h, whose handlers run on a thread pool (never touch UI
 * there). Timers fire only while the message loop (heliosview_run) is running;
 * scheduling and cancelling are safe from any thread. */

typedef void (*heliosview_timer_cb)(uint32_t timer_id, void* userdata);

/* Run cb once on the message-loop thread after delay_ms. Returns a nonzero id,
 * 0 on failure. Cancelled with heliosview_timer_cancel. */
HELIOSVIEW_API uint32_t heliosview_delay(uint32_t delay_ms, heliosview_timer_cb cb, void* userdata);

/* Run cb on the message-loop thread every interval_ms until cancelled or the
 * loop quits. Returns a nonzero id; 0 on failure (interval_ms == 0 is
 * rejected). */
HELIOSVIEW_API uint32_t heliosview_interval(uint32_t interval_ms, heliosview_timer_cb cb,
                                            void* userdata);

/* Cancel a pending delay or interval (safe from any thread; an unknown or
 * already-fired id is a no-op). */
HELIOSVIEW_API void heliosview_timer_cancel(uint32_t timer_id);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_LOOP_H */
