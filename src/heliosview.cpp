// HeliosView.dll - platform-independent core: version, event queue, conversion-delegate registration.
// Platform-specific implementation lives in src/win32/ (window/message loop, IOCP async I/O).
#include <HeliosView/heliosview.h>
#include "heliosview_internal.h"

#include <chrono>
#include <thread>

/* ================= Version ================= */

const char* heliosview_version(void)
{
    return HELIOSVIEW_VERSION_STR;
}

/* ================= Memory allocation ================= */

void heliosview_set_allocator(const heliosview_allocator_t* allocator)
{
    if (allocator)
        hv::g_allocator = *allocator;
    else
        hv::g_allocator = heliosview_allocator_t{}; /* restore malloc/free */
}

/* ================= Event queue (thread-local) =================
 * The queue is thread-local: it lives on the message-loop thread (WndProc posts
 * and poll/wait consume on the same thread). Cross-thread event access is not
 * supported. */

int heliosview_poll(heliosview_event_t* out_event)
{
    if (!out_event)
        return 0;
    if (hv::g_queue.empty())
        return 0;
    *out_event = hv::g_queue.front();
    hv::g_queue.pop_front();
    return 1;
}

int heliosview_wait(heliosview_event_t* out_event)
{
    if (!out_event)
        return 0;
    while (hv::g_queue.empty() && !hv::g_quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (hv::g_queue.empty())
        return -1; /* quit requested and queue empty */
    *out_event = hv::g_queue.front();
    hv::g_queue.pop_front();
    return 1;
}

void heliosview_post_event(const heliosview_event_t* event)
{
    if (!event)
        return;
    heliosview_event_t copy = *event;
    if (copy.timestamp_ms == 0)
        copy.timestamp_ms = hv::now_ms();
    hv::queue_push(copy);
}

void heliosview_quit(void)
{
    hv::g_quit = true;
    if (hv::g_platform_wake)
        hv::g_platform_wake();
}

void heliosview_wake_loop(void)
{
    if (hv::g_platform_wake)
        hv::g_platform_wake();
}

/* ================= Conversion delegate ================= */

void heliosview_set_native_handler(heliosview_native_handler_fn handler)
{
    hv::g_native_handler = handler;
}
