#pragma once

/**
 * Internal shared header: state shared across implementation files
 * (src/*.cpp and src/win32/*.cpp). Not part of the public API.
 */

#include <HeliosView/heliosview.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <new>
#include <utility>

namespace hv {

/* ---------- Configurable allocator ----------
 * The library routes its object allocations through this. Defaults to the
 * standard allocator (alloc/free null → malloc/free). Set via the public
 * heliosview_set_allocator(). */

inline heliosview_allocator_t g_allocator{}; /* {null, null, null} = malloc/free */

template <class T, class... Args>
T* hv_alloc(Args&&... args)
{
    void* p;
    if (g_allocator.alloc)
        p = g_allocator.alloc(sizeof(T), g_allocator.context);
    else
        p = std::malloc(sizeof(T));
    if (!p)
        throw std::bad_alloc(); /* preserve the throwing semantics of operator new */
    return new (p) T(std::forward<Args>(args)...);
}

template <class T>
void hv_dealloc(T* p)
{
    if (!p)
        return;
    p->~T();
    if (g_allocator.free_)
        g_allocator.free_(p, g_allocator.context);
    else
        std::free(p);
}

/* ---------- Event queue (thread-local: events are tied to the message-loop thread.
 * Cross-thread event/window access is not supported, so the queue lives on the thread
 * that runs the loop; the WndProc (same thread) and post_event both touch this one. ---------- */

inline thread_local std::deque<heliosview_event_t> g_queue;
inline std::atomic<bool> g_quit{false}; /* process/loop-wide control flag; may be set from any thread */
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
