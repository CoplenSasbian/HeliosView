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
#include <flat_map>
#include <new>
#include <string>
#include <utility>

namespace hv {

/* Naming convention: g_* = process/loop-wide state (shared; use atomics where
 * several threads may touch it); tls_* = thread-local state — each thread has
 * its own instance (e.g. the event queue lives per-thread by design). */

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

inline thread_local std::deque<heliosview_event_t> tls_event_queue;
inline std::atomic<bool> g_quit{false}; /* process/loop-wide control flag; may be set from any thread */

/* Native-message -> event converters. Registered handlers are tried in id order
 * after the library's built-in default_native_convert (which always runs first);
 * the first converter to return 1 (handled: it posted events via
 * heliosview_post_event) or 0 (consumed, no events) wins. Only ever touched
 * on the message-loop thread (add/remove happen during app setup, iteration in the
 * WndProc), so no locking is needed. */
inline std::atomic<uint32_t> g_next_handler_id{1};
inline std::flat_map<uint32_t, heliosview_native_handler_fn> g_native_handlers;

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
    /* The deque stores events by value: this emplace IS the one copy every
     * event pays — public posting and internal emission both land here, and
     * neither does a copy before this point. */
    const bool fill_timestamp = event.timestamp_ms == 0;
    heliosview_event_t& slot = tls_event_queue.emplace_back(event); /* one copy: caller/emitter → queue */
    if (fill_timestamp)
        slot.timestamp_ms = now_ms(); /* the element just inserted; no back() lookup */
    if (g_platform_wake)
        g_platform_wake();
}

} // namespace hv

/* ================= Last-error (platform-independent) =================
 *
 * Every failing public-API call site records WHY it failed through hv_fail
 * (or, on win32, the hv_fail_win32 / hv_fail_hresult wrappers that also append
 * the platform message): a thread-local (code, message) pair that the public
 * heliosview_last_error / heliosview_last_error_string read back. The message
 * is produced at the failure point, where the context is known — NOT in a
 * central decoder — so a new error is a one-line `return hv_fail(...)` and the
 * decoder never grows. The code follows the header's error-code space
 * (see "Error reporting" in heliosview.h: -1 generic / -2 invalid name /
 * -3 destroyed / negated Win32 codes as small negatives / negated HRESULTs as
 * large positives). The value is only meaningful immediately after a call failed
 * (returned < 0 or NULL); a later successful call does not clear it.
 * No platform dependency: any implementation file (src/*.cpp or a platform
 * backend) sets and reads these. */

inline thread_local int g_hv_last_error_code = 0;
inline thread_local std::string g_hv_last_error_message;

/* Record `message` (UTF-8, copied) as this thread's last error and return
 * `code`, so a failure site is one line:
 *     return hv_fail(-1, "window is not created yet");
 * Win32 backends prefer the formatting wrappers hv_fail_win32 / hv_fail_hresult
 * (heliosview_win32_internal.h), which append the platform's own message. */
inline int hv_fail(int code, const char* message)
{
    g_hv_last_error_code = code;
    g_hv_last_error_message = message ? message : "";
    return code;
}
