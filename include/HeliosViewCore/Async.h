#pragma once

/**
 * HeliosView.Core -- Async: background thread pool for off-UI-thread work,
 * backed by standalone asio (asio::thread_pool) through stdexec's exec::asio
 * adapter (exec::asio::asio_thread_pool).
 *
 * Usage from a bindJson handler (see WebViewJson.h):
 *
 *   helios::Async async;                     // app-scoped member
 *   window->bindJson<AddReq>("add", [&async](AddReq req) -> std::execution::task<std::string> {
 *       co_await std::execution::schedule(async.get_scheduler());  // hop off the UI thread
 *       // ... CPU work / blocking I/O ...
 *       co_await std::execution::schedule(app.get_scheduler());    // only if you must touch UI state
 *       co_return std::format("{}", req.a + req.b);
 *   });
 *
 * The C bridge's resolve/reject is thread-safe, so the task may complete on a
 * pool thread without marshalling back; marshal back only to touch UI state.
 *
 * The pool is a real asio::thread_pool: use impl() to post asio work (timers,
 * sockets, ...) on the same worker threads, e.g. impl().get_executor().
 *
 * Requires the stdexec implementation path (Execution.h): the exec::asio
 * adapters are stdexec-only and are not part of C++26 <execution>.
 */

#include <HeliosViewCore/Execution.h>

#if defined(HELIOSVIEW_HAVE_STD_EXECUTION) && HELIOSVIEW_HAVE_STD_EXECUTION
#  error "HeliosViewCore/Async.h requires the stdexec implementation path (C++23 + stdexec, see Execution.h): the exec::asio adapters are not part of C++26 <execution>."
#endif

#include <exec/asio/asio_thread_pool.hpp>

#include <algorithm>
#include <cstdint>
#include <thread>

namespace helios {

class Async {
public:
    // One worker thread per hardware thread.
    Async()
        : Async(std::max(1u, std::thread::hardware_concurrency()))
    {
    }

    explicit Async(std::uint32_t num_threads)
        : m_pool(num_threads)
    {
    }

    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;

    // A std::execution::scheduler: senders from schedule(get_scheduler())
    // complete on one of the pool's worker threads.
    auto get_scheduler() noexcept
    {
        return m_pool.get_scheduler();
    }

    // The underlying asio thread pool — post asio work on the same threads.
    auto& impl() noexcept { return m_pool; }

    std::uint32_t available_parallelism() const noexcept
    {
        return m_pool.available_parallelism();
    }

private:
    experimental::execution::asio::asio_thread_pool m_pool;
};

static_assert(std::execution::scheduler<decltype(std::declval<Async&>().get_scheduler())>);

} // namespace helios
