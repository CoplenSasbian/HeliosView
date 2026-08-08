#pragma once

/**
 * HeliosView.Core — Signal: signals and slots.
 *
 * Slots are std::function stored in a C++23 std::flat_set (ordered flat
 * container, cache-friendly):
 *   - connect returns a slot id; disconnect(id) removes it (heterogeneous lookup)
 *   - emission copies the slot table before iterating: connect/disconnect
 *     inside a slot does not affect the current emission
 *   - sync slots: void(Args...), invoked immediately on emission
 *   - async slots: functions returning a sender (std::execution::task
 *     coroutines / Async operations), started on emission (fire-and-forget,
 *     driven by an independent thread); may switch freely between the Async
 *     thread pool and the UI thread via co_await schedule
 *     Note: a task may outlive the signal/window that emitted it; it must
 *     own any captured objects itself (shared_ptr etc.), never capture only
 *     references to stack objects
 *
 * Depends on: Execution.h (std::execution compatibility layer).
 * Note: slots run on the message-loop thread (emission happens in App::exec's
 * frame callback).
 * Errors from async slots must be handled inside the function (unhandled
 * errors are ignored).
 */

#include <HeliosViewCore/Execution.h>

#include <cstddef>
#include <flat_set>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

namespace helios {

namespace detail {

// fire-and-forget receiver environment:
// provides an inline start scheduler, so a task slot starts on the emitting thread
// (task::as_awaitable requires the parent environment to provide its start scheduler; defaulting fails)
struct sink_env {
    std::execution::inline_scheduler query(std::execution::get_start_scheduler_t) const noexcept { return {}; }
    std::execution::inline_scheduler query(std::execution::get_scheduler_t) const noexcept { return {}; }
    std::execution::inline_scheduler query(std::execution::get_delegation_scheduler_t) const noexcept { return {}; }
};

// fire-and-forget operation state: after connect + start the opstate leaks
// (the operation itself cleans up on completion).
// The opstate must not be destroyed: the connect-await coroutine frame is
// embedded in it, and destroying the task value destroys that frame; premature
// destruction frees the frame while the task is suspended or its completion
// chain is unwinding (UAF verified in the debugger, 0xDD fill).
// The task frame is released once by the completion chain after normal
// completion; each emission leaks one opstate (~a few hundred bytes,
// acceptable at UI signal rates; deferred reclamation could be added later).
template <class Sender>
struct fire_and_forget_state {
    struct receiver {
        using receiver_concept = std::execution::receiver_t;
        sink_env get_env() const noexcept { return {}; }
        void set_value(auto&&...) && noexcept {}
        void set_error(std::exception_ptr) && noexcept {}
        void set_stopped() && noexcept {}
    };

    using op_t = std::execution::connect_result_t<Sender, receiver>;

    explicit fire_and_forget_state(Sender&& sndr)
        : op(std::execution::connect(std::move(sndr), receiver{}))
    {
    }

    void run() noexcept { std::execution::start(op); }

    op_t op; /* not movable: constructed in place */
};

// start the sender (fire-and-forget): completion is ignored; opstate deliberately leaks (see above)
inline void start_sender(std::execution::sender auto sndr)
{
    auto* state = new fire_and_forget_state<std::decay_t<decltype(sndr)>>(std::move(sndr));
    state->run();
}

} // namespace detail

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;
    using Id = std::size_t;

    // Connect a synchronous slot. The slot is invoked immediately on emission
    // with the signal's arguments.
    // Returns a slot id that can be passed to disconnect(Id) to remove the slot.
    Id connect(Slot slot)
    {
        const Id id = m_nextId++;
        m_slots.emplace(Entry{id, std::move(slot)});
        return id;
    }

    // Connect an asynchronous slot: fn must return a sender (a coroutine task or
    // an Async operation), which is started fire-and-forget on emission.
    // The task may outlive the signal/window that emitted it: it must own any
    // captured objects itself (shared_ptr etc.), never capture references to stack objects.
    // Returns a slot id (same semantics as the synchronous connect).
    // Note: errors from async slots must be handled inside the function (unhandled errors are ignored).
    template <typename Fn>
        requires std::invocable<Fn&, Args...>
              && std::execution::sender<std::invoke_result_t<Fn&, Args...>>
    Id connect(Fn&& fn)
    {
        return connect([fn = std::forward<Fn>(fn)](Args... args) mutable {
            detail::start_sender(fn(std::move(args)...));
        });
    }

    // Connect a member-function slot: connect(&MyWindow::onKeyPressed, this).
    // The member function may be sync or async (returns a sender, started fire-and-forget on emission).
    // Returns a slot id (same semantics as the synchronous connect).
    // Note: the object must outlive the signal.
    template <typename Obj, typename Ret>
        requires std::invocable<Ret Obj::*, Obj&, Args...>
    Id connect(Ret Obj::* member, Obj* obj)
    {
        using result_t = std::invoke_result_t<Ret Obj::*, Obj&, Args...>;
        if constexpr (std::execution::sender<result_t>) {
            /* async member function */
            return connect([obj, member](Args... args) mutable {
                detail::start_sender((obj->*member)(std::move(args)...));
            });
        } else {
            /* sync member function */
            return connect([obj, member](Args... args) {
                (obj->*member)(std::move(args)...);
            });
        }
    }

    // Disconnect a slot by its id. Removes the slot from future emissions.
    void disconnect(Id id) { m_slots.erase(id); }

    // Remove all connected slots
    void clear() { m_slots.clear(); }

    // Number of currently connected slots
    std::size_t slotCount() const { return m_slots.size(); }

    // True when no slots are connected
    bool empty() const { return m_slots.empty(); }

    // Emit the signal: invoke every connected slot with args.
    // The slot table is copied before iterating, so connect/disconnect called
    // inside a slot does not affect the current emission.
    void operator()(Args... args) const
    {
        const auto slots = m_slots; /* copy: few slots, cost negligible */
        for (const auto& entry : slots)
            if (entry.slot)
                entry.slot(args...);
    }

private:
    struct Entry {
        Id id;
        Slot slot;
    };

    // ordered by slot id; is_transparent enables heterogeneous lookup (erase(Id) without constructing an Entry)
    struct ById {
        using is_transparent = void;
        bool operator()(const Entry& a, const Entry& b) const { return a.id < b.id; }
        bool operator()(Id a, const Entry& b) const { return a < b.id; }
        bool operator()(const Entry& a, Id b) const { return a.id < b; }
    };

    std::flat_set<Entry, ById> m_slots;
    Id m_nextId = 1;
};

} // namespace helios
