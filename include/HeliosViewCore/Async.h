#pragma once

/**
 * HeliosView.Core -- Async: one Boost.Asio execution context (a thread pool)
 * plugged into std::execution. Compute, timers and socket I/O all run on the
 * same worker threads — no second context needed.
 *
 * Usage from a bindJson handler (see WebViewJson.h):
 *
 *   helios::Async async;                     // app-scoped member
 *   window->bindJson<AddReq>("add", [&async](AddReq req) -> std::execution::task<std::string> {
 *       co_await std::execution::schedule(async.get_scheduler());  // hop off the UI thread
 *       // ... CPU work / blocking I/O ...
 *       co_await async.timer(500ms);         // sleep off the UI thread (steady_timer)
 *       co_await std::execution::schedule(app.get_scheduler());    // only if you must touch UI state
 *       co_return std::format("{}", req.a + req.b);
 *   });
 *
 * The C bridge's resolve/reject is thread-safe, so the task may complete on a
 * pool thread without marshalling back; marshal back only to touch UI state.
 *
 * Timers and sockets on the same pool, each in two styles:
 *   sender:    co_await async.timer(500ms);            // self-owned timer
 *              co_await async.sleepAsync(500ms);       // same
 *              auto snd = async.waitAsync(timer);      // user-owned timer
 *              auto snd = async.readAsync(socket, buf);  // -> set_value(n)
 *              auto snd = async.writeAsync(socket, buf);
 *              auto snd = async.connectAsync(socket, ep);  // set_value()
 *              auto snd = async.acceptAsync(acceptor, peer);
 *   callback:  async.sleep(500ms, handler);  async.wait(timer, handler);
 *              async.read(socket, buf, handler);  async.write(...);
 *              async.connect(...);  async.accept(...);
 *   Errors map to set_error(std::exception_ptr); operation_aborted -> set_stopped.
 *
 * helios::use_sender is stdexec's official asio completion token (standalone
 * or Boost, whichever the generated asio_config.hpp selects): pass it to ANY
 * asio async operation to get a sender instead of a completion handler —
 * including Boost.Beast (HTTP/WebSocket):
 *
 *   auto snd = socket.async_read_some(buf, helios::use_sender);
 *   auto snd = beast::http::async_read(stream, buffer, req, helios::use_sender);
 *
 * The pool is a real asio thread pool: get_executor() / impl() expose the
 * underlying executor for raw asio work on the same threads.
 *
 * Lifetime: Async (and its pool) must outlive every pending sender it created
 * — like the bindings themselves. User-owned asio objects (timers, sockets)
 * used with waitAsync/use_sender must outlive the pending operation.
 *
 * Requires the stdexec implementation path (Execution.h): the exec::asio
 * adapters are stdexec-only and are not part of C++26 <execution>.
 */

#include <HeliosViewCore/Execution.h>
#include <HeliosViewCore/detail/BasicSender.h>

#if defined(HELIOSVIEW_HAVE_STD_EXECUTION) && HELIOSVIEW_HAVE_STD_EXECUTION
#  error "HeliosViewCore/Async.h requires the stdexec implementation path (C++23 + stdexec, see Execution.h): the exec::asio adapters are not part of C++26 <execution>."
#endif

/* stdexec's official asio adapters (backend selected by the generated
 * asio_config.hpp: standalone asio or Boost.Asio). The umbrella
 * <asio.hpp>/<boost/asio.hpp> it pulls in also provides steady_timer/read/
 * write, so no per-backend includes are needed here. */
#include <exec/asio/asio_thread_pool.hpp>
#include <exec/asio/use_sender.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

namespace helios {

// stdexec's official asio completion token: pass to any asio async operation
// to get a sender instead of a completion handler (see the header docs).
inline constexpr auto use_sender = experimental::execution::asio::use_sender;

// Backend-agnostic view of the configured asio (standalone or Boost), from
// the generated asio_config.hpp (asio_impl = ::asio or ::boost::asio).
namespace asio = experimental::execution::asio;

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

    /* ---------- the pool ---------- */

    // A std::execution::scheduler: senders from schedule(get_scheduler())
    // complete on one of the pool's worker threads.
    auto get_scheduler() noexcept
    {
        return m_pool.get_scheduler();
    }

    // The asio executor of the pool: raw asio work (sockets, ...) on the same
    // worker threads.
    auto get_executor() const
    {
        return m_pool.get_executor();
    }

    // The underlying asio thread pool — post asio work on the same threads.
    auto& impl() noexcept { return m_pool; }

    std::uint32_t available_parallelism() const noexcept
    {
        return m_pool.available_parallelism();
    }

    /* ---------- timers (on the pool) ---------- */

    // Completion functor for timer()/sleepAsync(): arms a pool-owned
    // steady_timer (held by shared_ptr so it outlives the async_wait without
    // a user-managed timer object) and completes the receiver when it fires.
    // The sender machinery is the generic detail::BasicSender; only this
    // functor is timer-specific (the HeliosExec sleepAsync design).
    struct SleepComplete {
        asio::asio_impl::any_io_executor executor;
        asio::asio_impl::steady_timer::duration duration;

        template <std::execution::receiver Recv>
        void operator()(Recv&& recv) const noexcept
        {
            auto timer = std::make_shared<asio::asio_impl::steady_timer>(executor);
            timer->expires_after(duration);
            timer->async_wait([timer, recv = std::move(recv)](const asio::error_code& ec) mutable {
                if (ec == asio::asio_impl::error::operation_aborted) {
                    std::execution::set_stopped(std::move(recv));
                } else if (ec) {
                    std::execution::set_error(
                        std::move(recv),
                        std::make_exception_ptr(asio::system_error(ec)));
                } else {
                    std::execution::set_value(std::move(recv));
                }
            });
        }
    };

    // A std::execution::sender that completes on a pool thread after `d`
    // elapses (set_value on success, set_stopped on cancellation,
    // set_error(std::exception_ptr) on failure):
    //   co_await async.timer(500ms);
    template <class Rep, class Period>
    auto timer(std::chrono::duration<Rep, Period> d) const
    {
        return detail::BasicSender{SleepComplete{m_pool.get_executor(), d}};
    }

    // Alias of timer(): sender-style sleep (HeliosExec AsioContext parity).
    template <class Rep, class Period>
    auto sleepAsync(std::chrono::duration<Rep, Period> d) const
    {
        return timer(d);
    }

    // Callback style: sleep for `d`; handler(error_code) runs on a pool
    // thread when the timer fires (the timer is owned by the operation).
    template <class Rep, class Period, class CompletionHandler>
    void sleep(const std::chrono::duration<Rep, Period>& d, CompletionHandler&& handler)
    {
        auto timer = std::make_shared<asio::asio_impl::steady_timer>(m_pool.get_executor());
        timer->expires_after(d);
        timer->async_wait([timer, h = std::forward<CompletionHandler>(handler)](
                              asio::error_code ec) mutable {
            std::move(h)(ec);
        });
    }

    // Wait for an existing (user-owned) timer object; handler(error_code).
    template <class Timer, class CompletionHandler>
    void wait(Timer& timer, CompletionHandler&& handler)
    {
        timer.async_wait(std::forward<CompletionHandler>(handler));
    }

    // Sender style of wait(): timer.async_wait(helios::use_sender).
    template <class Timer>
    auto waitAsync(Timer& timer)
    {
        return timer.async_wait(use_sender);
    }

    /* ---------- socket I/O (on the pool, two styles each) ----------
     * read / write / connect / accept        : callback (completion handler)
     * readAsync / writeAsync / connectAsync / acceptAsync : sender style
     *   (readAsync/writeAsync -> set_value(bytes_transferred); connectAsync /
     *   acceptAsync -> set_value(); errors -> set_error(exception_ptr),
     *   operation_aborted -> set_stopped). */

    // Read exactly `buffers` bytes; handler(error_code, bytes_transferred).
    template <class Socket, class MutableBufferSequence, class CompletionHandler>
    void read(Socket& socket, MutableBufferSequence&& buffers, CompletionHandler&& handler)
    {
        asio::asio_impl::async_read(socket, std::forward<MutableBufferSequence>(buffers),
                                    std::forward<CompletionHandler>(handler));
    }

    // Sender style of read(): completes with the number of bytes transferred.
    template <class Socket, class MutableBufferSequence>
    auto readAsync(Socket& socket, MutableBufferSequence&& buffers)
    {
        return asio::asio_impl::async_read(socket, std::forward<MutableBufferSequence>(buffers),
                                           use_sender);
    }

    // Write `buffers`; handler(error_code, bytes_transferred).
    template <class Socket, class ConstBufferSequence, class CompletionHandler>
    void write(Socket& socket, ConstBufferSequence&& buffers, CompletionHandler&& handler)
    {
        asio::asio_impl::async_write(socket, std::forward<ConstBufferSequence>(buffers),
                                     std::forward<CompletionHandler>(handler));
    }

    // Sender style of write(): completes with the number of bytes transferred.
    template <class Socket, class ConstBufferSequence>
    auto writeAsync(Socket& socket, ConstBufferSequence&& buffers)
    {
        return asio::asio_impl::async_write(socket, std::forward<ConstBufferSequence>(buffers),
                                            use_sender);
    }

    // Connect to `endpoint`; handler(error_code).
    // (Member async_connect: completion is (error_code) only, which the
    // use_sender machinery maps cleanly -- the free-function range overload
    // completes with (error_code, iterator) and is not supported by the
    // sender transform in the pinned stdexec.)
    template <class Socket, class Endpoint, class CompletionHandler>
    void connect(Socket& socket, const Endpoint& endpoint, CompletionHandler&& handler)
    {
        socket.async_connect(endpoint, std::forward<CompletionHandler>(handler));
    }

    // Sender style of connect(): completes with set_value() (no values).
    template <class Socket, class Endpoint>
    auto connectAsync(Socket& socket, const Endpoint& endpoint)
    {
        return socket.async_connect(endpoint, use_sender);
    }

    // Accept a connection into `peer`; handler(error_code).
    template <class Acceptor, class Socket, class CompletionHandler>
    void accept(Acceptor& acceptor, Socket& peer, CompletionHandler&& handler)
    {
        acceptor.async_accept(peer, std::forward<CompletionHandler>(handler));
    }

    // Sender style of accept().
    template <class Acceptor, class Socket>
    auto acceptAsync(Acceptor& acceptor, Socket& peer)
    {
        return acceptor.async_accept(peer, use_sender);
    }

private:
    experimental::execution::asio::asio_thread_pool m_pool;
};

static_assert(std::execution::scheduler<decltype(std::declval<Async&>().get_scheduler())>);

} // namespace helios
