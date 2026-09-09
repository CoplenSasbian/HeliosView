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
#include <vector>

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

/* ---------- Live-window registry (thread-local, message-loop thread) ----------
 * The core owns the registry, so every backend gets heliosview_window_count /
 * heliosview_window_from_id for free: a backend only calls hv_register_window
 * when the native window exists and hv_unregister_window before it is destroyed.
 * The window object itself stays backend-private (stored as void*). */
inline thread_local std::flat_map<uintptr_t, void*> tls_windows;

inline void hv_register_window(uintptr_t window_id, void* window)
{
    if (window_id)
        tls_windows[window_id] = window;
}

inline void hv_unregister_window(uintptr_t window_id)
{
    if (window_id)
        tls_windows.erase(window_id);
}

inline void* hv_find_window(uintptr_t window_id)
{
    const auto it = tls_windows.find(window_id);
    return it == tls_windows.end() ? nullptr : it->second;
}

inline int hv_window_count()
{
    return static_cast<int>(tls_windows.size());
}

/* ---------- Action registry (thread-local, message-loop thread) ----------
 * id → live action object. Menus reference actions by pointer directly; this
 * map exists for the paths that only have an id (accelerators, the legacy
 * by-id menu helpers) and for heliosview_action_from_id. The object itself is
 * backend-private (stored as void*). */
inline thread_local std::flat_map<uint32_t, void*> tls_actions;
inline std::atomic<uint32_t> g_next_action_id{1};

inline uint32_t hv_register_action(void* action)
{
    const uint32_t id = g_next_action_id.fetch_add(1);
    if (id == 0)
        return 0; /* id space exhausted */
    tls_actions[id] = action;
    return id;
}

inline void hv_unregister_action(uint32_t id)
{
    if (id)
        tls_actions.erase(id);
}

inline void* hv_find_action(uint32_t id)
{
    const auto it = tls_actions.find(id);
    return it == tls_actions.end() ? nullptr : it->second;
}

/* Native message interceptor filters (pipeline / middleware chain).
 * Only touched on the message-loop thread, so no locking is needed — the public
 * header documents that register/remove must happen on the loop thread.
 *
 * The dispatch snapshot is rebuilt lazily whenever the registry changes, so the
 * per-message path never allocates (the WndProc used to copy the whole map for
 * every message), and a filter that removes itself while running still finishes
 * the current message from the snapshot it was dispatched from. */
struct heliosview_filter_entry {
    heliosview_native_filter_fn filter;
    void* userdata;
};
inline std::atomic<uint32_t> g_next_filter_id{1};
inline std::flat_map<uint32_t, heliosview_filter_entry> g_native_filters;
inline std::vector<heliosview_filter_entry> g_native_filter_snapshot; /* id order */
inline bool g_native_filter_snapshot_dirty = true;

/* The filters to dispatch this message, in registration (id) order. */
inline const std::vector<heliosview_filter_entry>& native_filters_snapshot()
{
    if (g_native_filter_snapshot_dirty) {
        g_native_filter_snapshot.clear();
        g_native_filter_snapshot.reserve(g_native_filters.size());
        for (const auto& [id, entry] : g_native_filters)
            g_native_filter_snapshot.push_back(entry);
        g_native_filter_snapshot_dirty = false;
    }
    return g_native_filter_snapshot;
}

/* Legacy converter delegates (adapted into the filter pipeline). */
inline std::atomic<uint32_t> g_next_handler_id{1};
inline std::flat_map<uint32_t, heliosview_native_handler_fn> g_native_handlers;

/* Platform wake callback: the win32 implementation registers SetEvent (wakes the message-loop wait); may be null */
inline void (*g_platform_wake)(void) = nullptr;

/* ---------- Application identity + activation policy ----------
 * Set by heliosview_app_init / heliosview_set_activation_policy (core), read by
 * a backend before it creates its first window/tray (macOS: NSApplication
 * activation policy, bundle identity; Linux: desktop/application id). Written
 * once at startup, before any UI exists — a backend may cache the value. */
inline std::string g_app_id; /* UTF-8, "" = unset */
inline heliosview_activation_policy_t g_activation_policy = HELIOSVIEW_ACTIVATION_REGULAR;

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

/* ---------- Loop timers (min-heap keyed by due time) ----------
 *
 * Implemented in the core (src/heliosview.cpp): public heliosview_delay /
 * heliosview_interval / heliosview_timer_cancel manage a vector sorted by
 * due_ms. The backend's message loop calls these helpers so it wakes at exactly
 * the next due time instead of polling: run_due_timers() fires every timer whose
 * due time has passed (returning true if any ran); next_timer_wait_ms() reports
 * how long to wait (in ms) until the next due timer — -1 when none is pending,
 * 0 when one is already due. */

bool run_due_timers();
int64_t next_timer_wait_ms();

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
