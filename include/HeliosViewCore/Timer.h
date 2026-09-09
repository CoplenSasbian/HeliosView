#pragma once

/**
 * HeliosView.Core -- Loop timers (message-loop thread).
 *
 * One-shot (delay) and repeating (interval) callbacks that run on the
 * message-loop thread — the same thread as App::exec. These are the UI-thread
 * timers: the callback may safely touch windows. They are deliberately
 * DISTINCT from the asio-based timers in Async.h (AsyncContext::timer /
 * ::sleep / ::interval), whose handlers run on a thread pool and must NOT touch
 * UI. Naming avoids the AsyncContext member names on purpose.
 *
 * Loop timers fire only while the message loop is running (App::exec /
 * heliosview_run); scheduling and cancelling are safe from any thread, and a
 * pending timer is dropped when the loop quits. The loop sleeps until the next
 * due timer instead of polling.
 */

#include <HeliosView/heliosview.h>

#include <chrono>
#include <cstdint>

namespace helios {

// Callback fired on the message-loop thread when a delay/interval timer runs.
using TimerCallback = heliosview_timer_cb;

// Run `cb` once on the message-loop thread after `d`. Returns a nonzero id
// (cancel with cancelTimer); 0 on failure (e.g. `cb` null).
inline uint32_t delay(std::chrono::milliseconds d, heliosview_timer_cb cb, void* userdata = nullptr)
{
    return heliosview_delay(static_cast<uint32_t>(d.count()), cb, userdata);
}

// Run `cb` on the message-loop thread every `d` until cancelled or the loop
// quits. Returns a nonzero id; 0 on failure (`d <= 0` is rejected).
inline uint32_t interval(std::chrono::milliseconds d, heliosview_timer_cb cb,
                         void* userdata = nullptr)
{
    return heliosview_interval(static_cast<uint32_t>(d.count()), cb, userdata);
}

// Cancel a pending delay or interval (safe from any thread; an unknown or
// already-fired id is a no-op).
inline void cancelTimer(uint32_t timer_id)
{
    heliosview_timer_cancel(timer_id);
}

} // namespace helios