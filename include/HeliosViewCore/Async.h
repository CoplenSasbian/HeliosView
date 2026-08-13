#pragma once

/**
 * HeliosView.Core — Async: background async I/O (thread pool + platform multiplexer).
 *
 * The Windows implementation is built on IOCP (thread pool); the API is
 * platform-independent, so other platforms only need to reimplement the C layer.
 * Callbacks are C++ callables (lambdas / move-only callables; may safely capture handle
 * copies).
 *
 * Error semantics: callbacks receive error == 0 on success, otherwise a negative
 * value (a negated platform error code). The sender-based (*Async) APIs wrap the
 * same codes in IoError and deliver them via set_error(IoError).
 *
 * Threading model: all callbacks run on background worker threads (possibly
 * concurrently); shared state must be synchronized by the caller.
 * Handle semantics: Socket / File are copyable handles (shared ownership);
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
 *       connectAsync / writeAsync / readAsync (single-shot read)
 */

#include <HeliosView/heliosview.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <HeliosViewCore/Execution.h> /* C++26 <execution> compatibility shim: always use std::execution */

namespace helios {

/* ---------- Copyable, refcounted RAII handle over a raw C handle ----------
 * Closed (idempotently) when the last copy dies. CloseFn is a C close function such
 * as heliosview_socket_close / heliosview_file_close. Socket and File are aliases. */

template <class Ctx, void (*Close)(Ctx*)>
class RefHandle {
public:
    // Default-construct an empty handle (owns nothing; operator bool() is false)
    RefHandle() = default;
    // Wrap a raw C handle, taking ownership: closed when the last copy dies (RAII)
    explicit RefHandle(Ctx* handle) : m_state(std::make_shared<State>(handle)) {}
    // Copy/move: share ownership with other handles (refcounted)
    RefHandle(const RefHandle&) = default;
    RefHandle& operator=(const RefHandle&) = default;
    RefHandle(RefHandle&&) noexcept = default;
    RefHandle& operator=(RefHandle&&) noexcept = default;

    // True if this handle owns a live resource; false when empty or closed
    explicit operator bool() const { return m_state && m_state->handle; }
    // The raw C handle (nullptr when empty); do not close it directly
    Ctx* handle() const { return m_state ? m_state->handle : nullptr; }
    // Explicitly close (idempotent): afterwards no copy's destructor closes again
    void close() const { if (m_state) m_state->close(); }

private:
    struct State {
        Ctx* handle;
        ~State() { close(); }
        void close()
        {
            if (handle) {
                Close(handle);
                handle = nullptr;
            }
        }
    };
    std::shared_ptr<State> m_state;
};

// A TCP connection handle; closes the connection when the last copy dies
using Socket = RefHandle<heliosview_socket_t, heliosview_socket_close>;
// A file handle; closes the file when the last copy dies
using File = RefHandle<heliosview_file_t, heliosview_file_close>;

/* ---------- Buffer: an owned or borrowed byte buffer for writes ----------
 * Used by write / fileWrite (callback) and writeAsync / fileWriteAsync (sender).
 * Ownership is explicit, not implicit:
 *   - Buffer::copy(ptr, n) / copy(container) : allocate + copy            -> owned
 *   - Buffer::alloc(n)                        : allocate n writable bytes  -> owned
 *   - Buffer::take(pmr::vector<char>)         : take ownership (no copy)   -> owned
 *   - Buffer::ref(ptr, n) / ref(container)    : borrow (no copy); the caller MUST
 *     keep the data alive until the write completes (ownership outlives the call).
 * copy/ref accept (pointer, size), std::span and any contiguous byte container
 * (std::vector<char>/<uint8_t>, std::string, std::array, std::string_view, ...).
 * The Buffer is moved into the operation (zero-copy): an owning Buffer carries its
 * data; a borrowed (ref) Buffer points at caller-owned data. Use copy() (or the
 * (const void*, len) convenience) when you cannot guarantee that lifetime. */
class Buffer {
public:
    Buffer() = default;

    // ref (explicit): borrowed view over [data, data+len); the caller keeps the data
    // alive until the send completes. Use Buffer::ref for a clear, explicit borrow.
    explicit Buffer(const void* data, size_t len)
        : m_ptr(static_cast<const char*>(data)), m_len(len), m_owns(false)
    {
    }
    // ref (explicit): borrowed view over a string
    explicit Buffer(const std::string& s) : Buffer(s.data(), s.size()) {}
    // ref (explicit): borrowed view over a contiguous byte range
    explicit Buffer(std::span<const char> s) : Buffer(s.data(), s.size()) {}

    // ref: explicit borrowed view; the caller must keep the data alive until the send
    // completes (ownership outlives the writeAsync call). No copy happens.
    static Buffer ref(const void* data, size_t len) { return Buffer(data, len); }
    static Buffer ref(std::span<const char> s) { return Buffer(s); }
    // ref: borrowed view over any contiguous byte container (vector, string, array, string_view, ...)
    template <class R>
        requires requires (const R& c) { c.data(); c.size(); }
    static Buffer ref(const R& c) { return ref(c.data(), c.size()); }

    // owned: allocate len writable bytes (e.g. for reads, or fill-then-send)
    static Buffer alloc(size_t len)
    {
        Buffer b;
        b.m_storage.resize(len);
        b.m_ptr = b.m_storage.data();
        b.m_len = len;
        b.m_owns = true;
        return b;
    }
    // owned: allocate + copy
    static Buffer copy(const void* data, size_t len)
    {
        Buffer b = alloc(len);
        if (len)
            std::memcpy(b.data(), data, len);
        return b;
    }
    static Buffer copy(std::span<const char> s) { return copy(s.data(), s.size()); }
    // copy: allocate + copy from any contiguous byte container (vector, string, array, string_view, ...)
    template <class R>
        requires requires (const R& c) { c.data(); c.size(); }
    static Buffer copy(const R& c) { return copy(c.data(), c.size()); }
    // owned: take ownership of an existing buffer (no copy); do not use `data` afterwards
    static Buffer take(std::pmr::vector<char> data)
    {
        Buffer b;
        b.m_storage = std::move(data);
        b.m_ptr = b.m_storage.data();
        b.m_len = b.m_storage.size();
        b.m_owns = true;
        return b;
    }

    // Copy: owned buffers copy their storage; a ref copies just the view.
    Buffer(const Buffer& o)
        : m_len(o.m_len), m_owns(o.m_owns), m_storage(o.m_storage)
    {
        m_ptr = m_owns ? m_storage.data() : o.m_ptr;
    }
    Buffer& operator=(const Buffer& o)
    {
        if (this != &o) {
            m_storage = o.m_storage;
            m_len = o.m_len;
            m_owns = o.m_owns;
            m_ptr = m_owns ? m_storage.data() : o.m_ptr;
        }
        return *this;
    }
    // Move: owned storage moves; the source is emptied.
    Buffer(Buffer&& o) noexcept
        : m_len(o.m_len), m_owns(o.m_owns), m_storage(std::move(o.m_storage))
    {
        m_ptr = m_owns ? m_storage.data() : o.m_ptr;
        o.m_ptr = nullptr; o.m_len = 0; o.m_owns = false;
    }
    Buffer& operator=(Buffer&& o) noexcept
    {
        if (this != &o) {
            m_storage = std::move(o.m_storage);
            m_len = o.m_len;
            m_owns = o.m_owns;
            m_ptr = m_owns ? m_storage.data() : o.m_ptr;
            o.m_ptr = nullptr; o.m_len = 0; o.m_owns = false;
        }
        return *this;
    }

    const char* data() const { return m_ptr; }
    char* data() { return const_cast<char*>(m_ptr); } /* writable only when owned (alloc) */
    size_t size() const { return m_len; }
    bool owns() const { return m_owns; }

private:
    const char* m_ptr = nullptr;
    size_t m_len = 0;
    bool m_owns = false;
    std::pmr::vector<char> m_storage; /* backing store when owned */
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
 * Each in-flight operation's Ctx (the concrete callable + buffers) is allocated with
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

// One heap-allocated Ctx per operation (holds the concrete callable and any buffers); freed when the callback fires
template <typename Ctx>
void completionTramp(int error, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn(error);
    destroyCtx(ctx);
}

template <typename Ctx>
void postTramp(int error, void* userdata)
{
    (void)error;
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn();
    destroyCtx(ctx);
}

// Timer task: the C timer's callback. Runs fn, then destroys the timer handle
// (dropping its caller ref; the timer thread's task ref frees it afterwards).
template <typename Ctx>
void timerTramp(int error, void* userdata)
{
    (void)error;
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn();
    heliosview_timer_destroy(ctx->timer);
    destroyCtx(ctx);
}

template <typename Ctx>
void transferTramp(int error, uint32_t bytes, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn(error, bytes);
    destroyCtx(ctx);
}

template <typename Ctx>
void readTramp(int error, const char* data, uint32_t len, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn(error, data, len);
    /* Streaming read: the C layer reuses the same userdata for every chunk, so
     * the Ctx must stay alive between chunks. Only the terminal callback
     * (error, or peer close with len == 0) frees it; a chunk callback
     * (error == 0 && len > 0) must not. */
    if (error != 0 || len == 0)
        destroyCtx(ctx);
}

template <typename Ctx>
void connectTramp(int error, heliosview_socket_t* tcp, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn(error, Socket(tcp));
    destroyCtx(ctx);
}

template <typename Ctx>
void openTramp(int error, heliosview_file_t* file, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    ctx->fn(error, File(file));
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

/* ---------- Generic one-shot sender over a C async op ----------
 *
 * The op's C callback signature is fixed per op, but the connected receiver (Recv)
 * is known only at connect() time. To let a shared C tramp reach it, the operation
 * state type-erases delivery through op_sink: a tramp decodes the raw C result into
 * a Value and calls sink->complete(), which the op_state overrides to deliver
 * set_value/set_error to the receiver. Each op is a small Config providing
 * value_t / data_t / start(); the sender/operation-state machinery is shared. */

namespace detail {

// Type-erased completion channel for a completion value of type ValueT.
// Implemented by the operation state (which owns the receiver).
template <class ValueT>
struct op_sink {
    virtual ~op_sink() = default;
    virtual void complete(int error, const void* value) = 0; /* value = &ValueT on success, else nullptr */
    /* like complete(), but the caller hands ownership of the value over
     * (*value is moved from): avoids copying a large value into the receiver */
    virtual void complete_move(int error, void* value) = 0;
    virtual void* op_data() noexcept = 0;                    /* the op's data (Config::data_t) */
};

// The per-connect operation state: owns the op data + the connected receiver.
template <class Config, class Recv>
struct op_state : op_sink<typename Config::value_t> {
    using operation_state_concept = std::execution::operation_state_t;

    typename Config::data_t data;
    Recv recv;

    op_state(typename Config::data_t d, Recv r)
        : data(std::move(d))
        , recv(std::move(r))
    {
    }

    void start() & noexcept
    {
        const int rc = Config::start(static_cast<op_sink<typename Config::value_t>*>(this), data);
        if (rc != 0)
            std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(rc)));
    }

    void complete(int error, const void* value) override
    {
        if (error == 0)
            std::execution::set_value(std::move(recv),
                                      *static_cast<const typename Config::value_t*>(value));
        else
            std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(error)));
    }

    void complete_move(int error, void* value) override
    {
        if (error == 0)
            std::execution::set_value(std::move(recv),
                                      std::move(*static_cast<typename Config::value_t*>(value)));
        else
            std::execution::set_error(std::move(recv), std::make_exception_ptr(IoError(error)));
    }

    void* op_data() noexcept override { return &data; }
};

// Generic sender: connect() wires a Config-provided C op to a receiver.
template <class Config>
struct op_sender {
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(typename Config::value_t),
        std::execution::set_error_t(std::exception_ptr)>;

    typename Config::data_t data;

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        return op_state<Config, Recv>{data, std::move(recv)};
    }
};

// Shared tramp for ops whose success value is built straight from one C arg:
// Config::make(arg) on success, ValueT{} on error.
template <class Config, class CArg>
void simple_tramp(int error, CArg arg, void* userdata)
{
    auto* sink = static_cast<op_sink<typename Config::value_t>*>(userdata);
    typename Config::value_t value = (error == 0) ? Config::make(arg) : typename Config::value_t{};
    sink->complete(error, &value);
}

/* ---- op configs: value_t + data_t + start(); make() where applicable ---- */

struct file_open_config {
    using value_t = File;
    using data_t = struct { heliosview_loop_t* loop; std::string path; bool write_mode; };
    static File make(heliosview_file_t* f) { return File(f); }
    static int start(void* sink, data_t& d)
    {
        return heliosview_file_open(d.loop, d.path.c_str(), d.write_mode ? 1 : 0,
                                    &simple_tramp<file_open_config, heliosview_file_t*>, sink);
    }
};

struct file_read_config {
    using value_t = uint32_t;
    using data_t = struct { File file; void* buf; uint32_t len; int64_t offset; };
    static uint32_t make(uint32_t b) { return b; }
    static int start(void* sink, data_t& d)
    {
        return heliosview_file_read(d.file.handle(), d.buf, d.len, d.offset,
                                    &simple_tramp<file_read_config, uint32_t>, sink);
    }
};

struct file_write_config {
    using value_t = uint32_t;
    using data_t = struct { File file; Buffer data; int64_t offset; };
    static uint32_t make(uint32_t b) { return b; }
    static int start(void* sink, data_t& d)
    {
        return heliosview_file_write(d.file.handle(), d.data.data(),
                                     static_cast<uint32_t>(d.data.size()), d.offset,
                                     &simple_tramp<file_write_config, uint32_t>, sink);
    }
};

struct socket_connect_config {
    using value_t = Socket;
    using data_t = struct { heliosview_loop_t* loop; std::string host; uint16_t port; };
    static Socket make(heliosview_socket_t* t) { return Socket(t); }
    static int start(void* sink, data_t& d)
    {
        return heliosview_socket_connect(d.loop, d.host.c_str(), d.port,
                                      &simple_tramp<socket_connect_config, heliosview_socket_t*>, sink);
    }
};

struct socket_write_config {
    using value_t = uint32_t;
    using data_t = struct { Socket socket; Buffer data; };
    static uint32_t make(uint32_t b) { return b; }
    static int start(void* sink, data_t& d)
    {
        return heliosview_socket_write(d.socket.handle(), d.data.data(),
                                     static_cast<uint32_t>(d.data.size()),
                                     &simple_tramp<socket_write_config, uint32_t>, sink);
    }
};

// tcp_read: single-shot read; its tramp needs the op data (len/buf/socket).
struct socket_read_config {
    using value_t = uint32_t;
    using data_t = struct { Socket socket; void* buf; uint32_t len; };
    static int start(void* sink, data_t& d)
    {
        return heliosview_socket_read_start(d.socket.handle(), &socket_read_tramp, sink);
    }
    static void socket_read_tramp(int error, const char* data, uint32_t len, void* userdata)
    {
        auto* sink = static_cast<op_sink<uint32_t>*>(userdata);
        auto& d = *static_cast<data_t*>(sink->op_data());
        if (error != 0) {
            sink->complete(error, nullptr);
            return;
        }
        const uint32_t got = std::min<uint32_t>(len, d.len);
        if (got != 0 && d.buf)
            std::memcpy(d.buf, data, got);
        heliosview_socket_read_stop(d.socket.handle()); /* single-shot read: stop re-reading */
        sink->complete(0, &got);
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
    template <class Fn>
    void post(Fn&& fn)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(fn));
        if (heliosview_loop_post(m_loop, &detail::postTramp<Ctx>, ctx) != 0)
            detail::postTramp<Ctx>(0, ctx); /* submission failed: run inline, then free */
    }

    // Run fn once, after delayMs milliseconds, on a worker thread. Backed by the
    // loop's one-shot timer service (heliosview_timer_create): all deadlines are
    // tracked by a single internal timer thread, so a pending task occupies no
    // worker. Fire-and-forget: not cancellable. On a synchronous submission error
    // fn runs inline on the calling thread.
    template <class Fn>
    void postAfter(uint32_t delayMs, Fn&& fn)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
            heliosview_timer_t* timer;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(fn));
        ctx->timer = heliosview_timer_create(m_loop, delayMs, &detail::timerTramp<Ctx>, ctx);
        if (!ctx->timer) {
            ctx->fn();
            detail::destroyCtx(ctx); /* submission failed: run inline, then free */
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
    template <class Fn>
    void connect(const std::string& host, uint16_t port, Fn&& onConnect)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onConnect));
        const int rc = heliosview_socket_connect(m_loop, host.c_str(), port, &detail::connectTramp<Ctx>, ctx);
        if (rc != 0) {
            ctx->fn(rc, Socket{});
            detail::destroyCtx(ctx);
        }
    }

    // write of a Buffer: an owning Buffer (alloc/copy/take) is moved into the operation
    // (zero-copy); a borrowed (ref) Buffer is used directly — its data must outlive the
    // write (fire-and-forget). Use Buffer::copy for a safe copy.
    template <class Fn>
    void write(const Socket& socket, Buffer data, Fn&& onWrite)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
            Buffer data;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onWrite), std::move(data));
        const int rc = heliosview_socket_write(socket.handle(), ctx->data.data(),
                                               static_cast<uint32_t>(ctx->data.size()),
                                               &detail::transferTramp<Ctx>, ctx);
        if (rc != 0) {
            ctx->fn(rc, 0);
            detail::destroyCtx(ctx);
        }
    }

    // write of len bytes: copies data (Buffer::copy); the caller's buffer may be freed
    // as soon as this returns (data must be non-null when len > 0).
    template <class Fn>
    void write(const Socket& socket, const void* data, uint32_t len, Fn&& onWrite)
    {
        write(socket, Buffer::copy(data, len), std::forward<Fn>(onWrite));
    }

    // Start streaming reads: the callback fires once per received chunk and reads
    // resume automatically. data is valid only during the callback.
    // onRead(error, data, len):
    //   error == 0 && len > 0 -> a data chunk of len bytes
    //   error == 0 && len == 0 -> the peer closed; no further callbacks
    //   error != 0             -> read failed; no further callbacks
    // Stop with readStop() (unless already ended via the end/error callback).
    template <class Fn>
    void read(const Socket& socket, Fn&& onRead)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onRead));
        const int rc = heliosview_socket_read_start(socket.handle(), &detail::readTramp<Ctx>, ctx);
        if (rc != 0) {
            ctx->fn(rc, nullptr, 0);
            detail::destroyCtx(ctx);
        }
    }

    // Stop streaming reads (cancels the pending read; at most one callback may
    // still be in flight). Not needed after the end/error callback.
    void readStop(const Socket& socket) { heliosview_socket_read_stop(socket.handle()); }
    // Explicitly close the connection (idempotent; also closed when the last copy
    // dies). Pending writes complete with an error callback; do not use the handle after.
    void close(const Socket& socket) { socket.close(); }

    // ---- async file (callback API) ----

    // Async open. writeMode: true = create/truncate for writing; false = open an
    // existing file read-only.
    // onOpen(error, file):
    //   error == 0 -> file owns the opened file (valid until closed/destroyed)
    //   error != 0 -> failed; file is empty
    template <class Fn>
    void fileOpen(const std::string& path, bool writeMode, Fn&& onOpen)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onOpen));
        const int rc = heliosview_file_open(m_loop, path.c_str(), writeMode ? 1 : 0, &detail::openTramp<Ctx>, ctx);
        if (rc != 0) {
            ctx->fn(rc, File{});
            detail::destroyCtx(ctx);
        }
    }

    // Async positional read of up to len bytes at the given file offset into buf.
    // buf must remain valid until the callback fires.
    // onRead(error, bytes):
    //   error == 0 -> bytes bytes read (fewer than len at EOF)
    //   error != 0 -> failed; bytes is 0
    template <class Fn>
    void fileRead(const File& file, void* buf, uint32_t len, int64_t offset, Fn&& onRead)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onRead));
        const int rc = heliosview_file_read(file.handle(), buf, len, offset, &detail::transferTramp<Ctx>, ctx);
        if (rc != 0) {
            ctx->fn(rc, 0);
            detail::destroyCtx(ctx);
        }
    }

    // fileWrite of a Buffer at offset: an owning Buffer (alloc/copy/take) is moved into
    // the operation (zero-copy); a borrowed (ref) Buffer is used directly — its data must
    // outlive the write (fire-and-forget). Use Buffer::copy for a safe copy.
    template <class Fn>
    void fileWrite(const File& file, Buffer data, int64_t offset, Fn&& onWrite)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
            Buffer data;
        };
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onWrite), std::move(data));
        const int rc = heliosview_file_write(file.handle(), ctx->data.data(),
                                             static_cast<uint32_t>(ctx->data.size()), offset,
                                             &detail::transferTramp<Ctx>, ctx);
        if (rc != 0) {
            ctx->fn(rc, 0);
            detail::destroyCtx(ctx);
        }
    }

    // fileWrite of len bytes at offset: copies buf (Buffer::copy); the caller's buffer
    // may be freed as soon as this returns (buf must be non-null when len > 0).
    template <class Fn>
    void fileWrite(const File& file, const void* buf, uint32_t len, int64_t offset, Fn&& onWrite)
    {
        fileWrite(file, Buffer::copy(buf, len), offset, std::forward<Fn>(onWrite));
    }

    // Explicitly close the file (idempotent; also closed when the last copy dies)
    void fileClose(const File& file) { file.close(); }

    // ---- std::execution: thread-pool scheduler ----

    // The loop thread pool as a scheduler. Downstream of the returned sender runs
    // on a worker thread:
    //   std::execution::schedule(async.get_scheduler()) | std::execution::then(...)
    loop_scheduler get_scheduler() const noexcept { return {m_loop}; }

    // The underlying native loop handle (for C-layer calls that take a loop,
    // e.g. the HTTP client in HeliosViewCore/Http.h)
    heliosview_loop_t* handle() const noexcept { return m_loop; }

    // ---- std::execution: sender-based async ops ----
    // Failures are reported as set_error(std::exception_ptr(IoError)) (code() holds
    // the negated platform error code). Hold the sender until completion; the sender
    // keeps the handle/file alive for the duration of the operation.

    // Async open: set_value(File) on success
    auto fileOpenAsync(const std::string& path, bool writeMode) const
    {
        return detail::op_sender<detail::file_open_config>{
            detail::file_open_config::data_t{m_loop, path, writeMode}};
    }

    // Async positional read of up to len bytes at offset into buf:
    // set_value(uint32_t bytes). buf is caller-held and must stay valid until completion
    auto fileReadAsync(const File& file, void* buf, uint32_t len, int64_t offset) const
    {
        return detail::op_sender<detail::file_read_config>{
            detail::file_read_config::data_t{file, buf, len, offset}};
    }

    // Async positional write of len bytes at offset: set_value(uint32_t bytes).
    // Convenience: copies buf (equivalent to Buffer::copy); the caller's buffer may
    // be freed as soon as this returns.
    auto fileWriteAsync(const File& file, const void* buf, uint32_t len, int64_t offset) const
    {
        return detail::op_sender<detail::file_write_config>{
            detail::file_write_config::data_t{file, Buffer::copy(buf, len), offset}};
    }

    // Async positional write of a Buffer at offset: set_value(uint32_t bytes). The
    // Buffer is moved into the operation (zero-copy). An owning Buffer (alloc/copy/
    // take) carries its data; a borrowed (ref) Buffer points at caller-owned data that
    // must stay alive until the write completes. Use Buffer::copy if you cannot
    // guarantee that lifetime.
    auto fileWriteAsync(const File& file, Buffer buf, int64_t offset) const
    {
        return detail::op_sender<detail::file_write_config>{
            detail::file_write_config::data_t{file, std::move(buf), offset}};
    }

    // Async TCP connect: set_value(Socket) on success
    auto connectAsync(const std::string& host, uint16_t port) const
    {
        return detail::op_sender<detail::socket_connect_config>{
            detail::socket_connect_config::data_t{m_loop, host, port}};
    }

    // Async write: set_value(uint32_t bytes). Convenience: copies data (equivalent to
    // Buffer::copy); the caller's buffer may be freed as soon as this returns.
    auto writeAsync(const Socket& socket, const void* data, uint32_t len) const
    {
        return detail::op_sender<detail::socket_write_config>{
            detail::socket_write_config::data_t{socket, Buffer::copy(data, len)}};
    }

    // Async write from a Buffer: set_value(uint32_t bytes). The Buffer is moved into
    // the operation (zero-copy). An owning Buffer (alloc/copy/take) carries its data;
    // a borrowed (ref) Buffer points at caller-owned data that must stay alive until
    // the send completes. Use Buffer::copy if you cannot guarantee that lifetime.
    auto writeAsync(const Socket& socket, Buffer buf) const
    {
        return detail::op_sender<detail::socket_write_config>{
            detail::socket_write_config::data_t{socket, std::move(buf)}};
    }

    // Single-shot read (at most len bytes): set_value(uint32_t got) where 0 = peer
    // closed. buf is caller-held; re-reading stops automatically on completion
    auto readAsync(const Socket& socket, void* buf, uint32_t len) const
    {
        return detail::op_sender<detail::socket_read_config>{
            detail::socket_read_config::data_t{socket, buf, len}};
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
