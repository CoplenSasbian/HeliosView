#pragma once

/**
 * Internal shared header: state shared across implementation files
 * (src/*.cpp and src/win32/*.cpp). Not part of the public API.
 */

#include <HeliosView/heliosview.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>

namespace hv {

/* ---------- Event queue (lock-free: only the message-loop thread accesses it; push and pop on the same thread) ---------- */

inline std::deque<heliosview_event_t> g_queue;
inline std::atomic<bool> g_quit{false};
inline heliosview_native_handler_fn g_native_handler = nullptr;

/* Platform wake callback: the win32 implementation registers SetEvent (wakes the message-loop wait); may be null */
inline void (*g_platform_wake)(void) = nullptr;

inline int64_t now_ms()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

inline void queue_push(const heliosview_event_t& event)
{
    g_queue.push_back(event);
    if (g_platform_wake)
        g_platform_wake();
}

} // namespace hv
