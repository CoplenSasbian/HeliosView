#pragma once

/**
 * HeliosView.Core - App: message loop + event queue + idle task scheduling.
 *
 * Event dispatch goes through the C-layer window userdata (Window object
 * pointer), so Window's complete definition is not required here
 * (see the static_cast in loopCallback).
 *
 * Also acts as a std::execution::scheduler: senders from get_scheduler()
 * complete on the UI thread while the loop is idle.
 */

#include <HeliosViewCore/Execution.h>
#include <HeliosViewCore/Types.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <flat_map>
#include <functional>
#include <mutex>
#include <utility>

namespace helios {

struct app_scheduler; /* Forward declaration (defined below). */

class Window; /* Only used for the userdata static_cast. */
class Tray;   /* Only used for the friend (extension sink registry); defined in Tray.h. */
class Menu;   /* Only used for the friend (extension sink registry); defined in Menu.h. */

class App {
public:
    // Create the application object; registers itself as the current instance
    // (exactly one App is expected per process)
    App() { s_instance = this; }
    // Destroy the application; clears the current instance if it is this object.
    virtual ~App() { if (s_instance == this) s_instance = nullptr; }
    // Non-copyable: a process owns a single App
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // The current App instance, or nullptr before any App is constructed
    static App* instance() { return s_instance; }

    // Run the message loop. Dispatches queued events to windows and runs idle
    // tasks. Exits automatically when the last window is destroyed or after
    // quit(). Returns 0 on normal exit.
    int exec() { return heliosview_run(&App::loopCallback, this); }

    // Request the message loop to exit. exec() returns normally.
    void quit() { heliosview_quit(); }

    // Event queue:
    //   pollEvent - non-blocking fetch of the next pending event.
    //   Returns true and fills out when an event is available, false when the queue is empty.
    bool pollEvent(Event& out)
    {
        heliosview_event_t c{};
        if (heliosview_poll(&c) == 1) {
            out = Event::fromC(c);
            return true;
        }
        return false;
    }
    //   waitEvent - blocking fetch of the next pending event (1 ms polling granularity).
    //   Returns true and fills out when an event is available; false on quit request/error.
    //   Blocks the calling thread; do not call on the message-loop thread.
    bool waitEvent(Event& out)
    {
        heliosview_event_t c{};
        if (heliosview_wait(&c) == 1) {
            out = Event::fromC(c);
            return true;
        }
        return false;
    }

    // Post an event to the queue; must be called on the message-loop thread
    void postEvent(const Event& e)
    {
        heliosview_event_t c = e.toC();
        heliosview_post_event(&c);
    }

    // Register a native-message -> event converter callback (see heliosview.h for
    // the return-value contract). The library's built-in conversion always runs
    // first; registered converters are tried in order, and the first returning 1/0
    // wins. Returns an id (0 = failure).
    static uint32_t addNativeHandler(heliosview_native_handler_fn handler)
    {
        return heliosview_add_native_handler(handler);
    }

    // Remove a converter registered with addNativeHandler.
    static void removeNativeHandler(uint32_t id)
    {
        heliosview_remove_native_handler(id);
    }

    // Post a task to the message loop; callable from any thread. fn runs on the
    // UI thread while exec() is idle (this is the backing store of the App scheduler,
    // the path for background tasks to return to the UI thread).
    void postTask(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            m_tasks.push_back(std::move(fn));
        }
        heliosview_wake_loop(); /* Wake the idle message loop. */
    }

    // App-level event callback for unhandled window events.
    // Ignored by default; override to handle app-wide events.
    // Return true to mark the event handled.
    virtual bool event(const Event&) { return false; }

    // Scheduler for the UI thread: senders from schedule(get_scheduler()) complete
    // on the UI thread while the loop is idle (see app_scheduler).
    app_scheduler get_scheduler() noexcept;

private:
    // Run queued scheduler tasks while idle (UI thread only)
    void drainTasks()
    {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            tasks.swap(m_tasks);
        }
        for (auto& fn : tasks)
            if (fn)
                fn();
    }

    static int loopCallback(void* userdata); /* Defined in Window.h (needs Window's complete type). */

    std::mutex m_task_mutex;
    std::deque<std::function<void()>> m_tasks;
    static inline App* s_instance = nullptr;

    /* ---- extension event sinks (see Tray.h / Menu.h) ----
     * Decoupled objects (a Tray/Menu attached to a raw heliosview_window_t,
     * without a C++ Window wrapper) register here to receive the events they
     * handle. The message loop consults these sinks before window dispatch; a
     * sink returns true when it handled the event. Tray and Menu are friends. */
    using SinkId = std::size_t;
    std::flat_map<SinkId, std::function<bool(const Event&)>> m_sinks;
    SinkId m_nextSink = 1;
    friend class Tray;
    friend class Menu;

    SinkId addSink(std::function<bool(const Event&)> sink)
    {
        const SinkId id = m_nextSink++;
        m_sinks.emplace(id, std::move(sink));
        return id;
    }

    void removeSink(SinkId id) { m_sinks.erase(id); }
};

/* ---------- App scheduler (std::execution::scheduler) ----------
 *
 * A sender from schedule(app.get_scheduler()) completes downstream on the
 * UI thread while the message loop is idle:
 *   std::execution::schedule(app.get_scheduler()) | std::execution::then(fn)
 *
 * Note: avoid co_await-ing this scheduler inside coroutines (completion
 * races await_suspend - stdexec frame-lifetime issue); prefer sync_wait/then
 * or postTask for UI delivery.
 */

struct app_scheduler {
    // The App that owns this scheduler; nullptr means an empty (never schedulable) scheduler
    App* app = nullptr;

    // Two schedulers are equal when they wrap the same App
    bool operator==(const app_scheduler&) const = default;

    struct sender {
        using sender_concept = std::execution::sender_t;
        using completion_signatures = std::execution::completion_signatures<
            std::execution::set_value_t(),
            std::execution::set_error_t(std::exception_ptr),
            std::execution::set_stopped_t()>;

        App* app;

        template <std::execution::receiver Recv>
        auto connect(Recv recv) const
        {
            struct operation_state {
                using operation_state_concept = std::execution::operation_state_t;
                App* app;
                Recv recv;

                void start() & noexcept
                {
                    app->postTask([this] {
                        std::execution::set_value(std::move(recv));
                    });
                }
            };
            return operation_state{app, std::move(recv)};
        }
    };

    // Schedule a task: the downstream of the returned sender completes on the UI
    // thread while the message loop is idle (see the usage example above).
    auto schedule() const noexcept { return sender{app}; }
};

static_assert(std::execution::scheduler<app_scheduler>);

/* ---------- Implementation ---------- */

inline app_scheduler App::get_scheduler() noexcept
{
    return {this};
}

} // namespace helios
