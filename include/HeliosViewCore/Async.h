#pragma once

/**
 * HeliosView.Core — Async: background async I/O (thread pool + platform multiplexer).
 *
 * The Windows implementation is built on IOCP (thread pool); the API is
 * platform-independent, so other platforms only need to reimplement the C layer.
 * Callbacks are C++ callables (std::function / lambda; may safely capture handle
 * copies).
 *
 * Error semantics: callbacks receive error == 0 on success, otherwise a negative
 * value (a negated platform error code). The sender-based (*Async) APIs wrap the
 * same codes in IoError and deliver them via set_error(IoError).
 *
 * Threading model: all callbacks run on background worker threads (possibly
 * concurrently); shared state must be synchronized by the caller.
 * Handle semantics: TcpSocket / File are copyable handles (shared ownership);
 * the last copy automatically closes the underlying handle on destruction (RAII);
 * close() is idempotent.
 * Lifetime: no operations may be pending when Async is destroyed.
 *
 * std::execution support (C++26 P2300; via stdexec under C++23, see Execution.h):
 *   - get_scheduler(): exposes the thread pool as a std::execution::scheduler
 *       std::execution::schedule(async.get_scheduler()) | std::execution::then(...)
 *   - every callback API has a sender-based *Async counterpart (failures are
 *     reported as set_error(IoError)):
 *       fileOpenAsync / fileReadAsync / fileWriteAsync
 *       tcpConnectAsync / tcpWriteAsync / tcpReadAsync (single-shot read)
 */

#include <HeliosView/heliosview.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include <HeliosViewCore/Execution.h> /* C++26 <execution> compatibility shim: always use std::execution */

namespace helios {

/* ---------- TCP connection handle (copyable, refcounted; closed when the last copy dies) ---------- */

class TcpSocket {
public:
    // Default-construct an empty handle (owns no socket; operator bool() is false)
    TcpSocket() = default;
    // Wrap a raw C handle, taking ownership: the connection is closed when the last copy dies (RAII)
    explicit TcpSocket(heliosview_tcp_t* handle) : m_state(std::make_shared<State>(handle)) {}
    // Copy/move: share ownership with other handles (refcounted); the connection
    // is closed automatically when the last copy dies
    TcpSocket(const TcpSocket&) = default;
    TcpSocket& operator=(const TcpSocket&) = default;
    TcpSocket(TcpSocket&&) noexcept = default;
    TcpSocket& operator=(TcpSocket&&) noexcept = default;

    // True if this handle owns a live connection (i.e. connect succeeded); false when empty or closed
    explicit operator bool() const { return m_state && m_state->handle; }
    // The raw C handle of the underlying connection (nullptr when empty); do not close it directly
    heliosview_tcp_t* handle() const { return m_state ? m_state->handle : nullptr; }

    // Explicitly close the connection (idempotent): afterwards, no copy's destructor will close again
    void close() const { if (m_state) m_state->close(); }

private:
    struct State {
        heliosview_tcp_t* handle;
        ~State() { close(); }
        void close()
        {
            if (handle) {
                heliosview_tcp_close(handle);
                handle = nullptr;
            }
        }
    };
    std::shared_ptr<State> m_state;
};

/* ---------- File handle (copyable, refcounted; closed when the last copy dies) ---------- */

class File {
public:
    // Default-construct an empty handle (owns no file; operator bool() is false)
    File() = default;
    // Wrap a raw C handle, taking ownership: the file is closed when the last copy dies (RAII)
    explicit File(heliosview_file_t* handle) : m_state(std::make_shared<State>(handle)) {}
    // Copy/move: share ownership with other handles (refcounted); the file is
    // closed automatically when the last copy dies
    File(const File&) = default;
    File& operator=(const File&) = default;
    File(File&&) noexcept = default;
    File& operator=(File&&) noexcept = default;

    // True if this handle owns a live file (i.e. open succeeded); false when empty or closed
    explicit operator bool() const { return m_state && m_state->handle; }
    // The raw C handle of the underlying file (nullptr when empty); do not close it directly
    heliosview_file_t* handle() const { return m_state ? m_state->handle : nullptr; }

    // Explicitly close the file (idempotent): afterwards, no copy's destructor will close again
    void close() const { if (m_state) m_state->close(); }

private:
    struct State {
        heliosview_file_t* handle;
        ~State() { close(); }
        void close()
        {
            if (handle) {
                heliosview_file_close(handle);
                handle = nullptr;
            }
        }
    };
    std::shared_ptr<State> m_state;
};

/* ---------- Async I/O failure exception (the set_error payload of senders) ---------- */

// Async I/O failure payload: the exception carried by set_error(IoError) of the *Async senders
class IoError : public std::runtime_error {
public:
    // code: the negated platform error code (negative); 0 = success is never reported as an error
    explicit IoError(int code)
        : std::runtime_error("HeliosView async I/O error")
        , m_code(code)
    {
    }
    // The error code (negative, negated platform error code)
    int code() const noexcept { return m_code; }

private:
    int m_code;
};

/* ---------- Internal: C callbacks -> C++ callables ---------- */

namespace detail {

/* ---------- Allocator hooks (Ctx structs for async ops) ----------
 * Each in-flight operation's Ctx (the std::function + buffers) is allocated with
 * std::pmr::get_default_resource(), so the allocator can be swapped process-wide
 * via std::pmr::set_default_resource (the "P" in pmr). Every Ctx stores its owning
 * memory_resource as its first member, so the trampoline's destroyCtx (which only
 * receives the raw userdata pointer) frees through the same resource even if the
 * process default is swapped while the operation is in flight. Buffers inside the
 * Ctx use std::pmr::vector, which obtains its resource from the Ctx's own.
 * Note: the C layer takes the userdata pointer as-is, so Ctx must be freed here
 * (the C API only forwards it). */

// Allocate a Ctx with the given resource: the Ctx's first member must be that
// resource (see destroyCtx below)
template <typename Ctx, typename... Args>
inline Ctx* makeCtx(std::pmr::memory_resource* resource, Args&&... args)
{
    void* mem = resource->allocate(sizeof(Ctx), alignof(Ctx));
    try {
        return ::new (mem) Ctx(resource, std::forward<Args>(args)...);
    } catch (...) {
        resource->deallocate(mem, sizeof(Ctx), alignof(Ctx));
        throw;
    }
}

// Deallocate a Ctx: the owning resource is read from the Ctx itself (first
// member), never re-queried from the process default
template <typename Ctx>
inline void destroyCtx(Ctx* ctx) noexcept
{
    if (ctx) {
        std::pmr::memory_resource* resource = ctx->resource;
        ctx->~Ctx();
        resource->deallocate(ctx, sizeof(Ctx), alignof(Ctx));
    }
}

// One heap-allocated Ctx per operation (holds the std::function and any buffers); freed when the callback fires
template <typename Ctx>
void completionTramp(int error, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->fn) ctx->fn(error);
    destroyCtx(ctx);
}

template <typename Ctx>
void postTramp(int error, void* userdata)
{
    (void)error;
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->fn) ctx->fn();
    destroyCtx(ctx);
}

template <typename Ctx>
void transferTramp(int error, uint32_t bytes, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->fn) ctx->fn(error, bytes);
    destroyCtx(ctx);
}

template <typename Ctx>
void readTramp(int error, const char* data, uint32_t len, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->fn) ctx->fn(error, data, len);
    destroyCtx(ctx);
}

template <typename Ctx>
void connectTramp(int error, heliosview_tcp_t* tcp, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->fn) ctx->fn(error, TcpSocket(tcp));
    destroyCtx(ctx);
}

template <typename Ctx>
void openTramp(int error, heliosview_file_t* file, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    if (ctx->fn) ctx->fn(error, File(file));
    destroyCtx(ctx);
}

} // namespace detail

/* ---------- Thread-pool scheduler (std::execution::scheduler) ----------
 *
 * When the sender returned by schedule() completes, downstream runs on the
 * loop thread pool:
 *   std::execution::schedule(async.get_scheduler()) | std::execution::then(fn)
 */

struct loop_scheduler {
    // The underlying C loop; nullptr means an empty (never schedulable) scheduler
    heliosview_loop_t* loop = nullptr;

    // Two schedulers are equal when they wrap the same loop
    bool operator==(const loop_scheduler&) const = default;

    struct sender {
        using sender_concept = std::execution::sender_t;
        using completion_signatures = std::execution::completion_signatures<
            std::execution::set_value_t(),
            std::execution::set_error_t(std::exception_ptr),
            std::execution::set_stopped_t()>;

        heliosview_loop_t* loop;

        template <std::execution::receiver Recv>
        auto connect(Recv recv) const
        {
            struct operation_state {
                using operation_state_concept = std::execution::operation_state_t;
                heliosview_loop_t* loop;
                Recv recv;

                /* The operation state itself is used as the C callback's userdata
                 * (its lifetime spans the whole operation), so no per-op heap
                 * allocation */
                void start() & noexcept
                {
                    if (heliosview_loop_post(loop, &operation_state::tramp, this) != 0)
                        std::execution::set_error(std::move(recv),
                                           std::make_exception_ptr(
                                               std::runtime_error("loop post failed")));
                }

                static void tramp(int, void* userdata) noexcept
                {
                    auto* self = static_cast<operation_state*>(userdata);
                    std::execution::set_value(std::move(self->recv));
                }
            };
            return operation_state{loop, std::move(recv)};
        }
    };

    // Schedule a task: the downstream of the returned sender completes on a worker
    // thread of the pool (see the usage example above)
    auto schedule() const noexcept { return sender{loop}; }
};

static_assert(std::execution::scheduler<loop_scheduler>);

/* ---------- Internal: sender-based async operations (*Async) ---------- */

namespace detail {

struct file_open_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(File),
        std::execution::set_error_t(std::exception_ptr)>;

    heliosview_loop_t* loop;
    std::string path;
    bool write_mode = false;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        struct operation_state {
            using operation_state_concept = std::execution::operation_state_t;
            heliosview_loop_t* loop;
            std::string path;
            bool write_mode;
            Recv recv;

            /* The operation state itself is used as the C callback's userdata
             * (its lifetime spans the whole operation), so no per-op heap
             * allocation */
            void start() & noexcept
            {
                const int rc = heliosview_file_open(loop, path.c_str(), write_mode ? 1 : 0,
                                                    &operation_state::tramp, this);
                if (rc != 0)
                    std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
            }

            static void tramp(int error, heliosview_file_t* file, void* userdata) noexcept
            {
                auto* self = static_cast<operation_state*>(userdata);
                if (error == 0)
                    std::execution::set_value(std::move(self->recv), File(file));
                else
                    std::execution::set_error(std::move(self->recv), std::make_exception_ptr(IoError(error)));
            }
        };
        return operation_state{loop, path, write_mode, std::move(recv)};
    }
};

/* fileReadAsync: buf is caller-held and must stay valid until completion */
struct file_read_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(uint32_t),
        std::execution::set_error_t(std::exception_ptr)>;

    File file; /* keeps the handle alive for the operation */
    void* buf;
    uint32_t len;
    int64_t offset;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        struct operation_state {
            using operation_state_concept = std::execution::operation_state_t;
            File file;
            void* buf;
            uint32_t len;
            int64_t offset;
            Recv recv;

            /* The operation state itself is used as the C callback's userdata
             * (its lifetime spans the whole operation), so no per-op heap
             * allocation */
            void start() & noexcept
            {
                const int rc = heliosview_file_read(file.handle(), buf, len, offset,
                                                    &operation_state::tramp, this);
                if (rc != 0)
                    std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
            }

            static void tramp(int error, uint32_t bytes, void* userdata) noexcept
            {
                auto* self = static_cast<operation_state*>(userdata);
                if (error == 0)
                    std::execution::set_value(std::move(self->recv), bytes);
                else
                    std::execution::set_error(std::move(self->recv), std::make_exception_ptr(IoError(error)));
            }
        };
        return operation_state{file, buf, len, offset, std::move(recv)};
    }
};

/* fileWriteAsync: data is copied inside the sender */
struct file_write_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(uint32_t),
        std::execution::set_error_t(std::exception_ptr)>;

    File file;
    std::pmr::vector<char> data;
    int64_t offset;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        struct operation_state {
            using operation_state_concept = std::execution::operation_state_t;
            File file;
            std::pmr::vector<char> data;
            int64_t offset;
            Recv recv;

            /* The operation state itself is used as the C callback's userdata
             * (its lifetime spans the whole operation), so no per-op heap
             * allocation */
            void start() & noexcept
            {
                const int rc = heliosview_file_write(file.handle(), data.data(),
                                                     static_cast<uint32_t>(data.size()),
                                                     offset, &operation_state::tramp, this);
                if (rc != 0)
                    std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
            }

            static void tramp(int error, uint32_t bytes, void* userdata) noexcept
            {
                auto* self = static_cast<operation_state*>(userdata);
                if (error == 0)
                    std::execution::set_value(std::move(self->recv), bytes);
                else
                    std::execution::set_error(std::move(self->recv), std::make_exception_ptr(IoError(error)));
            }
        };
        return operation_state{file, data, offset, std::move(recv)};
    }
};

struct tcp_connect_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(TcpSocket),
        std::execution::set_error_t(std::exception_ptr)>;

    heliosview_loop_t* loop;
    std::string host;
    uint16_t port = 0;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        struct operation_state {
            using operation_state_concept = std::execution::operation_state_t;
            heliosview_loop_t* loop;
            std::string host;
            uint16_t port;
            Recv recv;

            /* The operation state itself is used as the C callback's userdata
             * (its lifetime spans the whole operation), so no per-op heap
             * allocation */
            void start() & noexcept
            {
                const int rc = heliosview_tcp_connect(loop, host.c_str(), port,
                                                      &operation_state::tramp, this);
                if (rc != 0)
                    std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
            }

            static void tramp(int error, heliosview_tcp_t* tcp, void* userdata) noexcept
            {
                auto* self = static_cast<operation_state*>(userdata);
                if (error == 0)
                    std::execution::set_value(std::move(self->recv), TcpSocket(tcp));
                else
                    std::execution::set_error(std::move(self->recv), std::make_exception_ptr(IoError(error)));
            }
        };
        return operation_state{loop, host, port, std::move(recv)};
    }
};

/* tcpWriteAsync: data is copied inside the sender */
struct tcp_write_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(uint32_t),
        std::execution::set_error_t(std::exception_ptr)>;

    TcpSocket socket;
    std::pmr::vector<char> data;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        struct operation_state {
            using operation_state_concept = std::execution::operation_state_t;
            TcpSocket socket;
            std::pmr::vector<char> data;
            Recv recv;

            /* The operation state itself is used as the C callback's userdata
             * (its lifetime spans the whole operation), so no per-op heap
             * allocation */
            void start() & noexcept
            {
                const int rc = heliosview_tcp_write(socket.handle(), data.data(),
                                                    static_cast<uint32_t>(data.size()),
                                                    &operation_state::tramp, this);
                if (rc != 0)
                    std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
            }

            static void tramp(int error, uint32_t bytes, void* userdata) noexcept
            {
                auto* self = static_cast<operation_state*>(userdata);
                if (error == 0)
                    std::execution::set_value(std::move(self->recv), bytes);
                else
                    std::execution::set_error(std::move(self->recv), std::make_exception_ptr(IoError(error)));
            }
        };
        return operation_state{socket, data, std::move(recv)};
    }
};

/* tcpReadAsync: single-shot read (at most len bytes); stops re-reading after
   completion. Payload uint32_t (bytes read; 0 = peer closed); buf is caller-held */
struct tcp_read_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(uint32_t),
        std::execution::set_error_t(std::exception_ptr)>;

    TcpSocket socket;
    void* buf;
    uint32_t len;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        struct operation_state {
            using operation_state_concept = std::execution::operation_state_t;
            TcpSocket socket;
            void* buf;
            uint32_t len;
            Recv recv;

            /* The operation state itself is used as the C callback's userdata
             * (its lifetime spans the whole operation), so no per-op heap
             * allocation */
            void start() & noexcept
            {
                const int rc = heliosview_tcp_read_start(socket.handle(),
                                                         &operation_state::tramp, this);
                if (rc != 0)
                    std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
            }

            static void tramp(int error, const char* data, uint32_t len, void* userdata) noexcept
            {
                auto* self = static_cast<operation_state*>(userdata);
                if (error == 0) {
                    const uint32_t got = std::min<uint32_t>(len, self->len);
                    if (got != 0 && self->buf)
                        std::memcpy(self->buf, data, got);
                    heliosview_tcp_read_stop(self->socket.handle()); /* single-shot read: stop re-reading */
                    std::execution::set_value(std::move(self->recv), got);
                } else {
                    std::execution::set_error(std::move(self->recv), std::make_exception_ptr(IoError(error)));
                }
            }
        };
        return operation_state{socket, buf, len, std::move(recv)};
    }
};

} // namespace detail

/* ---------- Async (thread pool + platform multiplexer) ---------- */

class Async {
public:
    // Create a background I/O thread pool + platform multiplexer.
    // threadCount: number of worker threads; 0 = hardware concurrency.
    // The object must outlive all pending operations (destroying it with
    // operations in flight is undefined behavior).
    explicit Async(unsigned threadCount = 0);
    // Destroy the loop: stops the worker threads and waits for them to exit.
    // Precondition: no operations may be pending (see the class doc above).
    ~Async();
    // Non-copyable and non-movable: an Async is the sole owner of its loop
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;

    // Block the calling thread until stop() is requested (typically run on the
    // main thread). Returns 0 on normal exit.
    int run() { return heliosview_loop_run(m_loop); }
    // Request the loop to stop: worker threads exit after all posted tasks complete.
    void stop() { heliosview_loop_stop(m_loop); }

    // Post a task to the thread pool: fn runs on a worker thread.
    // If the pool cannot accept the task (e.g. already stopped), fn runs
    // synchronously on the calling thread instead.
    void post(std::function<void()> fn)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void()> fn;
        };
        auto fnCopy = std::move(fn);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fnCopy);
        if (heliosview_loop_post(m_loop, &detail::postTramp<Ctx>, ctx) != 0) {
            detail::destroyCtx(ctx);
            fnCopy();
        }
    }

    // ---- async TCP (callback API) ----
    // Async success callbacks fire on a background worker thread (may run
    // concurrently); on a synchronous submission error the callback is invoked
    // inline on the calling thread. error == 0 means success; otherwise error is
    // a negative (negated platform) error code.

    // Async TCP connect.
    // onConnect(error, socket):
    //   error == 0 -> socket owns the connected connection (valid until closed/destroyed)
    //   error != 0 -> failed; socket is empty (operator bool() is false)
    void tcpConnect(const std::string& host, uint16_t port,
                    std::function<void(int error, TcpSocket socket)> onConnect)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void(int, TcpSocket)> fn;
        };
        auto fn = std::move(onConnect);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fn);
        const int rc = heliosview_tcp_connect(m_loop, host.c_str(), port, &detail::connectTramp<Ctx>, ctx);
        if (rc != 0) {
            detail::destroyCtx(ctx);
            fn(rc, TcpSocket{});
        }
    }

    // Async write of len bytes at data to the connection.
    // data is copied internally, so the caller's buffer may be freed/reused as soon
    // as this returns (data must be non-null when len > 0).
    // onWrite(error, bytes):
    //   error == 0 -> bytes bytes written (may be less than len)
    //   error != 0 -> failed; bytes is 0
    void tcpWrite(const TcpSocket& socket, const void* data, uint32_t len,
                  std::function<void(int error, uint32_t bytes)> onWrite)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void(int, uint32_t)> fn;
            std::pmr::vector<char> buf;
        };
        auto fn = std::move(onWrite);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fn, std::pmr::vector<char>{});
        if (data && len != 0)
            ctx->buf.assign(static_cast<const char*>(data), static_cast<const char*>(data) + len);
        const int rc = heliosview_tcp_write(socket.handle(), ctx->buf.data(), len, &detail::transferTramp<Ctx>, ctx);
        if (rc != 0) {
            detail::destroyCtx(ctx);
            fn(rc, 0);
        }
    }

    // Start streaming reads: the callback fires once per received chunk and reads
    // resume automatically. data is valid only during the callback.
    // onRead(error, data, len):
    //   error == 0 && len > 0 -> a data chunk of len bytes
    //   error == 0 && len == 0 -> the peer closed; no further callbacks
    //   error != 0             -> read failed; no further callbacks
    // Stop with tcpReadStop() (unless already ended via the end/error callback).
    void tcpReadStart(const TcpSocket& socket,
                      std::function<void(int error, const char* data, uint32_t len)> onRead)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void(int, const char*, uint32_t)> fn;
        };
        auto fn = std::move(onRead);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fn);
        const int rc = heliosview_tcp_read_start(socket.handle(), &detail::readTramp<Ctx>, ctx);
        if (rc != 0) {
            detail::destroyCtx(ctx);
            fn(rc, nullptr, 0);
        }
    }

    // Stop streaming reads (cancels the pending read; at most one callback may
    // still be in flight). Not needed after the end/error callback.
    void tcpReadStop(const TcpSocket& socket) { heliosview_tcp_read_stop(socket.handle()); }
    // Explicitly close the connection (idempotent; also closed when the last copy
    // dies). Pending writes complete with an error callback; do not use the handle after.
    void tcpClose(const TcpSocket& socket) { socket.close(); }

    // ---- async file (callback API) ----

    // Async open. writeMode: true = create/truncate for writing; false = open an
    // existing file read-only.
    // onOpen(error, file):
    //   error == 0 -> file owns the opened file (valid until closed/destroyed)
    //   error != 0 -> failed; file is empty
    void fileOpen(const std::string& path, bool writeMode,
                  std::function<void(int error, File file)> onOpen)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void(int, File)> fn;
        };
        auto fn = std::move(onOpen);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fn);
        const int rc = heliosview_file_open(m_loop, path.c_str(), writeMode ? 1 : 0, &detail::openTramp<Ctx>, ctx);
        if (rc != 0) {
            detail::destroyCtx(ctx);
            fn(rc, File{});
        }
    }

    // Async positional read of up to len bytes at the given file offset into buf.
    // buf must remain valid until the callback fires.
    // onRead(error, bytes):
    //   error == 0 -> bytes bytes read (fewer than len at EOF)
    //   error != 0 -> failed; bytes is 0
    void fileRead(const File& file, void* buf, uint32_t len, int64_t offset,
                  std::function<void(int error, uint32_t bytes)> onRead)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void(int, uint32_t)> fn;
        };
        auto fn = std::move(onRead);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fn);
        const int rc = heliosview_file_read(file.handle(), buf, len, offset, &detail::transferTramp<Ctx>, ctx);
        if (rc != 0) {
            detail::destroyCtx(ctx);
            fn(rc, 0);
        }
    }

    // Async positional write of len bytes at the given file offset from buf.
    // buf is copied internally, so the caller's buffer may be freed/reused as soon
    // as this returns (buf must be non-null when len > 0).
    // onWrite(error, bytes):
    //   error == 0 -> bytes bytes written (may be less than len)
    //   error != 0 -> failed; bytes is 0
    void fileWrite(const File& file, const void* buf, uint32_t len, int64_t offset,
                   std::function<void(int error, uint32_t bytes)> onWrite)
    {
        struct Ctx {
            std::pmr::memory_resource* resource;
            std::function<void(int, uint32_t)> fn;
            std::pmr::vector<char> data;
        };
        auto fn = std::move(onWrite);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), fn, std::pmr::vector<char>{});
        if (buf && len != 0)
            ctx->data.assign(static_cast<const char*>(buf), static_cast<const char*>(buf) + len);
        const int rc = heliosview_file_write(file.handle(), ctx->data.data(), len, offset, &detail::transferTramp<Ctx>, ctx);
        if (rc != 0) {
            detail::destroyCtx(ctx);
            fn(rc, 0);
        }
    }

    // Explicitly close the file (idempotent; also closed when the last copy dies)
    void fileClose(const File& file) { file.close(); }

    // ---- std::execution: thread-pool scheduler ----

    // The loop thread pool as a scheduler. Downstream of the returned sender runs
    // on a worker thread:
    //   std::execution::schedule(async.get_scheduler()) | std::execution::then(...)
    loop_scheduler get_scheduler() const noexcept { return {m_loop}; }

    // ---- std::execution: sender-based async ops ----
    // Failures are reported as set_error(std::exception_ptr(IoError)) (code() holds
    // the negated platform error code). Hold the sender until completion; the sender
    // keeps the handle/file alive for the duration of the operation.

    // Async open: set_value(File) on success
    auto fileOpenAsync(const std::string& path, bool writeMode) const
    {
        return detail::file_open_sender{m_loop, path, writeMode};
    }

    // Async positional read of up to len bytes at offset into buf:
    // set_value(uint32_t bytes). buf is caller-held and must stay valid until completion
    auto fileReadAsync(const File& file, void* buf, uint32_t len, int64_t offset) const
    {
        return detail::file_read_sender{file, buf, len, offset};
    }

    // Async positional write of len bytes at offset: set_value(uint32_t bytes).
    // buf is copied into the sender, so it may be freed as soon as this returns
    auto fileWriteAsync(const File& file, const void* buf, uint32_t len, int64_t offset) const
    {
        std::pmr::vector<char> data;
        if (buf && len != 0)
            data.assign(static_cast<const char*>(buf), static_cast<const char*>(buf) + len);
        return detail::file_write_sender{file, std::move(data), offset};
    }

    // Async TCP connect: set_value(TcpSocket) on success
    auto tcpConnectAsync(const std::string& host, uint16_t port) const
    {
        return detail::tcp_connect_sender{m_loop, host, port};
    }

    // Async write: set_value(uint32_t bytes). data is copied into the sender, so it
    // may be freed as soon as this returns
    auto tcpWriteAsync(const TcpSocket& socket, const void* data, uint32_t len) const
    {
        std::pmr::vector<char> copy;
        if (data && len != 0)
            copy.assign(static_cast<const char*>(data), static_cast<const char*>(data) + len);
        return detail::tcp_write_sender{socket, std::move(copy)};
    }

    // Single-shot read (at most len bytes): set_value(uint32_t got) where 0 = peer
    // closed. buf is caller-held; re-reading stops automatically on completion
    auto tcpReadAsync(const TcpSocket& socket, void* buf, uint32_t len) const
    {
        return detail::tcp_read_sender{socket, buf, len};
    }

private:
    heliosview_loop_t* m_loop;
};

/* ---------- Implementation ---------- */

inline Async::Async(unsigned threadCount)
    : m_loop(heliosview_loop_create(threadCount))
{
}

inline Async::~Async()
{
    heliosview_loop_destroy(m_loop);
}

} // namespace helios
