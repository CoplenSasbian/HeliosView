#pragma once

/**
 * HeliosView.Core -- Http: minimal HTTP/HTTPS client on the Async pool.
 *
 * Usage from a bindJson handler (see WebViewJson.h):
 *
 *   helios::Async async;                     // app-scoped member
 *   helios::http::Client client{async};      // lightweight handle; must not outlive async
 *
 *   window->bindJson<Req>("api", [&client](Req r) -> std::execution::task<boost::json::value> {
 *       auto resp = co_await client.get(r.url);
 *       co_return boost::json::value{{"status", resp.status},
 *                                    {"reason", resp.reason},
 *                                    {"body", resp.body}};
 *   });
 *
 * Every request is one exchange on the pool: (pooled or fresh) connection ->
 * write -> read. A non-2xx status is NOT an error: it is returned in
 * Response::status for the caller to interpret; network/timeout/TLS errors
 * throw (they surface on the task's error channel as
 * set_error(std::exception_ptr)).
 *
 * ============================ Connection pool ============================
 *
 * By default the client keeps connections alive and reuses them (HTTP/1.1
 * keep-alive): a request first tries an idle connection for the same origin
 * (scheme + host + port) and only dials a new one when the pool is empty. The
 * pool is per Client handle and SHARED by its copies, so
 * `helios::http::Client{async}` stored as an app-scoped member is the intended
 * usage — a Client created per request reuses nothing while it is alive, but
 * its idle connections are dropped when the last copy is destroyed.
 *
 *   PoolOptions opt;
 *   opt.idle_timeout = 30s;          // idle connections older than this are dropped
 *   opt.max_idle_per_origin = 2;     // keep at most 2 idle connections per origin
 *   opt.max_idle_total = 8;          // ... and at most 8 overall
 *   opt.dns_cache_ttl = 60s;         // re-resolve an origin after this
 *   helios::http::Client client{async, 10s, "cacert.pem", opt};
 *
 *   client.idle_connections();       // diagnostics / tests
 *   client.close_idle();             // drop every idle connection now
 *   client.clear_dns_cache();        // forget resolved endpoints
 *
 * Set `opt.keep_alive = false` to get the old one-request-per-connection
 * behavior (a "Connection: close" header is sent and nothing is pooled).
 *
 * Stale connections: a server may close an idle connection at any time, so a
 * pooled connection can be dead when it is picked up. Such a failure is
 * invisible for idempotent requests (GET / HEAD / OPTIONS / PUT / DELETE /
 * TRACE), which are retried once on a fresh connection; a non-idempotent
 * request (POST / PATCH) is NOT replayed — it reports the error instead, since
 * retrying could duplicate a side effect. Either keep idle_timeout below the
 * server's keep-alive timeout, or use keep_alive = false for POST-heavy
 * traffic.
 *
 * https:// uses boost::asio::ssl (OpenSSL, vendored at configure time). TLS
 * certificates are verified against the CA bundle passed to the constructor
 * (ca_bundle, a PEM file such as cacert.pem) — or the OpenSSL default paths
 * when empty. Pass an empty ca_bundle to skip loading a file (not to disable
 * verification; pass verify_none only if you really must, e.g. for testing).
 * The SSL context is created once per Client and reused by every TLS
 * connection, so pooling also saves a handshake, not just a TCP dial.
 *
 * Timeouts: resolve, connect, TLS handshake and the exchange are each bounded
 * by the Client's timeout, enforced by an explicit steady_timer per connection
 * (see detail::DeadlineGuard) rather than boost::beast::tcp_stream — beast's
 * stream timer completes on asio's dedicated timer thread, which does not fit
 * the completion machinery used here.
 *
 * Lifetime: the Client is a cheap handle (executor + timeout + shared pool); it
 * may be copied and may even be a temporary — but the Async it was created from
 * must outlive every in-flight request. An in-flight request keeps the pool
 * alive by itself, so the Client handle may be destroyed while a request runs.
 */

#include <HeliosViewCore/Async.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <flat_map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace helios::http {

// One parsed response. Headers preserve order and duplicates (a vector of
// pairs, not a map, so e.g. multiple Set-Cookie survive).
struct Response {
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// Connection-pool tuning (see the header docs). Defaults reuse connections.
struct PoolOptions {
    bool keep_alive = true;                              // reuse connections at all
    std::chrono::milliseconds idle_timeout{60000};       // drop idle connections older than this
    std::size_t max_idle_per_origin = 4;                 // idle connections kept per origin
    std::size_t max_idle_total = 16;                     // idle connections kept overall
    std::chrono::milliseconds dns_cache_ttl{60000};      // re-resolve an origin after this (0 = every request)
};

namespace detail {

struct UrlParts {
    std::string scheme; // "http" | "https"
    std::string host;
    std::string port;
    std::string target;

    // Pool key: connections are only reused for the same scheme/host/port.
    std::string origin() const { return scheme + "://" + host + ":" + port; }
};

// "http[s]://host[:port][/path?query]" -> parts.
inline UrlParts parse_url(std::string_view url)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
        throw std::invalid_argument("Http: URL has no scheme (expected http:// or https://)");
    const auto scheme = url.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https")
        throw std::invalid_argument("Http: unsupported scheme '" + std::string(scheme) + "'");

    const auto rest = url.substr(scheme_end + 3);
    const auto path_start = rest.find('/');
    const auto hostport = (path_start == std::string_view::npos) ? rest : rest.substr(0, path_start);
    const auto target = (path_start == std::string_view::npos) ? "/" : rest.substr(path_start);

    UrlParts out;
    out.scheme = std::string(scheme);
    out.target = std::string(target);
    out.port = (scheme == "https") ? "443" : "80";

    if (hostport.empty())
        throw std::invalid_argument("Http: URL has no host");
    if (hostport.front() == '[') { // [::1]:8080 IPv6 literal
        const auto close = hostport.find(']');
        if (close == std::string_view::npos)
            throw std::invalid_argument("Http: malformed IPv6 host");
        out.host = std::string(hostport.substr(1, close - 1));
        if (close + 1 < hostport.size() && hostport[close + 1] == ':')
            out.port = std::string(hostport.substr(close + 2));
    } else {
        const auto colon = hostport.rfind(':');
        if (colon != std::string_view::npos) {
            out.host = std::string(hostport.substr(0, colon));
            out.port = std::string(hostport.substr(colon + 1));
        } else {
            out.host = std::string(hostport);
        }
    }
    if (out.host.empty())
        throw std::invalid_argument("Http: URL has no host");
    return out;
}

// Build a Response from a beast response (move the body out).
inline Response make_response(boost::beast::http::response<boost::beast::http::string_body>&& res)
{
    Response out;
    out.status = res.result_int();
    out.reason = std::string(res.reason());
    out.body = std::move(res.body());
    for (const auto& f : res.base())
        out.headers.emplace_back(std::string(f.name_string()), std::string(f.value()));
    return out;
}

// May this request be replayed on a fresh connection after a pooled connection
// turned out to be dead? Only side-effect-free methods.
inline bool is_idempotent(boost::beast::http::verb verb)
{
    namespace http = boost::beast::http;
    switch (verb) {
    case http::verb::get:
    case http::verb::head:
    case http::verb::options:
    case http::verb::put:
    case http::verb::delete_:
    case http::verb::trace:
        return true;
    default:
        return false;
    }
}

/* The configured asio (standalone or Boost): helios::asio::asio_impl is the
 * backend namespace Async.h exposes. Aliased to a short name here because a
 * `using asio_impl = ...::asio_impl;` alias cannot be spelled in this scope. */
namespace aio = ::helios::asio::asio_impl;
using plain_stream = aio::ip::tcp::socket; /* beast's HTTP algorithms accept a socket */
using tls_stream = aio::ssl::stream<plain_stream>;
using endpoints_type = aio::ip::tcp::resolver::results_type;

/* ==================== asio operation -> sender ====================
 *
 * Every asio operation in this client is wrapped with the project's own
 * detail::BasicSender machinery (the same one Async's timers use) instead of
 * stdexec's `use_sender` token. Two reasons:
 *   - the error mapping is ours: a cancelled operation (the deadline firing)
 *     becomes a "timed out" exception instead of a `set_stopped` completion,
 *     which a caller could not distinguish from cancellation;
 *   - `use_sender`'s frame/mutex bookkeeping is only safe when an operation is
 *     initiated and completed in the same coroutine frame on the same thread;
 *     asio's resolver and timer threads break that assumption.
 * The completion handler runs on a pool thread; the receiver is moved into it. */

// Maps (error_code, values...) to set_value/set_error on the receiver.
template <class Start, class... Values>
struct OpComplete {
    Start start;
    const char* what;

    template <std::execution::receiver Recv>
    void operator()(Recv recv) const noexcept
    {
        start([recv = std::move(recv), what = what](helios::asio::error_code ec,
                                                    auto... values) mutable {
            if (ec == aio::error::operation_aborted) {
                std::execution::set_error(
                    std::move(recv),
                    std::make_exception_ptr(
                        std::runtime_error(std::string(what) + ": timed out")));
            } else if (ec) {
                std::execution::set_error(
                    std::move(recv),
                    std::make_exception_ptr(helios::asio::system_error(ec, what)));
            } else {
                std::execution::set_value(std::move(recv), values...);
            }
        });
    }
};

template <class Start, class... Values>
auto make_op(Start start, const char* what)
{
    using complete_t = OpComplete<Start, Values...>;
    return ::helios::detail::BasicSender<complete_t, Values...>{
        complete_t{std::move(start), what}};
}

// One HTTP write / read on any beast-compatible stream.
template <class Stream>
auto http_write(Stream& stream,
                const boost::beast::http::request<boost::beast::http::string_body>& req)
{
    auto start = [&stream, &req](auto handler) {
        boost::beast::http::async_write(stream, req, std::move(handler));
    };
    return make_op<decltype(start), std::size_t>(std::move(start), "Http: write");
}

template <class Stream>
auto http_read(Stream& stream, boost::beast::flat_buffer& buffer,
               boost::beast::http::response<boost::beast::http::string_body>& res)
{
    auto start = [&stream, &buffer, &res](auto handler) {
        boost::beast::http::async_read(stream, buffer, res, std::move(handler));
    };
    return make_op<decltype(start), std::size_t>(std::move(start), "Http: read");
}

template <class Socket>
auto socket_connect(Socket& socket, const aio::ip::tcp::endpoint& endpoint)
{
    auto start = [&socket, &endpoint](auto handler) {
        socket.async_connect(endpoint, std::move(handler));
    };
    return make_op<decltype(start)>(std::move(start), "Http: connect");
}

inline auto resolve_op(aio::ip::tcp::resolver& resolver, std::string_view host,
                       std::string_view service)
{
    auto start = [&resolver, host, service](auto handler) {
        resolver.async_resolve(host, service, std::move(handler));
    };
    return make_op<decltype(start), endpoints_type>(std::move(start), "Http: resolve");
}

inline auto tls_handshake(tls_stream& stream)
{
    auto start = [&stream](auto handler) {
        stream.async_handshake(aio::ssl::stream_base::client, std::move(handler));
    };
    return make_op<decltype(start)>(std::move(start), "Http: TLS handshake");
}

/* Per-operation deadline state, shared between the connection and the timer
 * handler: the handler may run on asio's timer thread after the operation (or
 * even the connection) is gone, so it holds a shared_ptr to this and only
 * touches the socket while the pointer is non-null. */
struct DeadlineState {
    std::atomic<aio::ip::tcp::socket*> socket{nullptr};
    std::atomic<std::uint64_t> epoch{0};
};

/* One reusable connection.
 *
 * Deliberately NOT boost::beast::tcp_stream: its per-stream timer completes on
 * asio's dedicated timer thread, and its internal cancellation is bound to that
 * timer. Here the deadline is an explicit steady_timer whose handler cancels
 * the socket (asio documents cancel() as thread-safe).
 *
 * `context` is declared before `stream` so the SSL context outlives the stream
 * that references it; the destructor clears the deadline's socket pointer so a
 * late timer firing cannot touch a destroyed socket. */
struct Connection {
    std::string origin;                                          // pool key
    bool tls = false;
    std::chrono::steady_clock::time_point idle_since{};          // valid while pooled
    std::shared_ptr<aio::ssl::context> context;                  // TLS only
    std::variant<std::unique_ptr<plain_stream>, std::unique_ptr<tls_stream>> stream;
    boost::beast::flat_buffer buffer;                            // empty between messages
    aio::steady_timer deadline;                                  // per-operation deadline
    std::shared_ptr<DeadlineState> deadline_state = std::make_shared<DeadlineState>();
    std::atomic<bool> reusable{true};                            // false = drop instead of pooling

    explicit Connection(aio::any_io_executor executor)
        : deadline(std::move(executor))
    {
    }

    ~Connection() { deadline_state->socket.store(nullptr); }

    // The socket under the (optional) TLS layer.
    aio::ip::tcp::socket& socket()
    {
        return std::visit([](auto& ptr) -> aio::ip::tcp::socket& {
            return boost::beast::get_lowest_layer(*ptr);
        }, stream);
    }
};

/* Bound one operation on `socket` by `timeout`: when the timer fires (on asio's
 * timer thread) the socket is cancelled, so the pending operation completes with
 * operation_aborted and detail::OpComplete turns that into a "timed out"
 * exception. Cancels on scope exit, so it is safe across suspension points and
 * exceptions. */
class DeadlineGuard {
public:
    DeadlineGuard(Connection& conn, aio::ip::tcp::socket& socket, std::chrono::milliseconds timeout)
        : m_conn(&conn)
    {
        conn.deadline_state->socket.store(&socket);
        const auto generation = conn.deadline_state->epoch.fetch_add(1) + 1;
        conn.deadline.expires_after(timeout);
        auto state = conn.deadline_state;
        conn.deadline.async_wait([state, generation](const helios::asio::error_code& ec) {
            if (ec || state->epoch.load() != generation)
                return; // cancelled, or a newer operation owns the deadline
            if (auto* socket = state->socket.load()) {
                helios::asio::error_code ignored;
                socket->cancel(ignored); // thread-safe; aborts the pending operation
            }
        });
    }

    ~DeadlineGuard()
    {
        m_conn->deadline.cancel();
        m_conn->deadline_state->socket.store(nullptr);
    }

    DeadlineGuard(const DeadlineGuard&) = delete;
    DeadlineGuard& operator=(const DeadlineGuard&) = delete;

private:
    Connection* m_conn;
};

// The client's shared pool. All container access is guarded by m_mutex (the
// Async pool has several worker threads); sockets themselves are never touched
// while idle, so destroying an idle connection needs no lock.
class ConnectionPool {
public:
    ConnectionPool(aio::any_io_executor executor, PoolOptions options)
        : m_executor(std::move(executor))
        , m_options(options)
    {
    }

    const PoolOptions& options() const noexcept { return m_options; }

    // A pooled connection for `origin`, or nullptr when none is usable.
    // Expired or unusable connections are dropped on the way out.
    std::unique_ptr<Connection> acquire(const std::string& origin)
    {
        std::vector<std::unique_ptr<Connection>> expired;
        std::unique_ptr<Connection> out;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_idle.find(origin);
            if (it != m_idle.end()) {
                auto& list = it->second;
                while (!list.empty()) {
                    auto conn = std::move(list.back()); // LIFO: the hottest connection
                    list.pop_back();
                    --m_idle_total;
                    if (conn->reusable.load() && now - conn->idle_since <= m_options.idle_timeout) {
                        out = std::move(conn);
                        break;
                    }
                    expired.push_back(std::move(conn));
                }
                if (list.empty())
                    m_idle.erase(it);
            }
        }
        return out; // `expired` is closed here, outside the lock
    }

    // Return a still-usable connection to the pool (or drop it when the pool is
    // full for that origin / overall).
    void release(std::unique_ptr<Connection> conn)
    {
        if (!conn)
            return;
        std::unique_ptr<Connection> dropped;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_idle.find(conn->origin);
            const std::size_t idle_for_origin = it == m_idle.end() ? 0 : it->second.size();
            if (!conn->reusable.load() || idle_for_origin >= m_options.max_idle_per_origin) {
                dropped = std::move(conn); // unusable, or the per-origin cap is reached
            } else {
                if (m_idle_total >= m_options.max_idle_total)
                    dropped = evict_oldest_locked(); // make room for this one
                conn->idle_since = std::chrono::steady_clock::now();
                m_idle[conn->origin].push_back(std::move(conn));
                ++m_idle_total;
            }
        }
    }

    std::size_t idle_count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_idle_total;
    }

    std::size_t idle_count(const std::string& origin) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_idle.find(origin);
        return it == m_idle.end() ? 0 : it->second.size();
    }

    // Drop every idle connection (does not affect checked-out ones).
    void close_idle()
    {
        std::flat_map<std::string, std::vector<std::unique_ptr<Connection>>> dropped;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            dropped.swap(m_idle);
            m_idle_total = 0;
        }
        // `dropped` closes the sockets here, outside the lock
    }

    // The (lazily created) SSL context shared by every TLS connection.
    std::shared_ptr<aio::ssl::context> tls_context(const std::string& ca_bundle)
    {
        std::lock_guard<std::mutex> lock(m_tls_mutex);
        if (!m_tls_context) {
            auto ctx = std::make_shared<aio::ssl::context>(aio::ssl::context::tls_client);
            ctx->set_verify_mode(aio::ssl::verify_peer);
            if (ca_bundle.empty())
                ctx->set_default_verify_paths();
            else
                ctx->load_verify_file(ca_bundle);
            m_tls_context = std::move(ctx);
        }
        return m_tls_context;
    }

    /* ---------- DNS cache ----------
     * Resolved endpoints per origin, so only the first request to an origin pays
     * for a resolve. */

    std::optional<endpoints_type> cached_endpoints(const std::string& origin) const
    {
        if (m_options.dns_cache_ttl.count() == 0)
            return std::nullopt;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_dns.find(origin);
        if (it == m_dns.end())
            return std::nullopt;
        if (std::chrono::steady_clock::now() - it->second.resolved_at > m_options.dns_cache_ttl)
            return std::nullopt;
        return it->second.endpoints;
    }

    void store_endpoints(const std::string& origin, endpoints_type endpoints)
    {
        if (m_options.dns_cache_ttl.count() == 0)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dns[origin] = dns_entry{std::move(endpoints), std::chrono::steady_clock::now()};
    }

    void clear_dns_cache()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dns.clear();
    }

    aio::any_io_executor executor() const { return m_executor; }

private:
    // Oldest idle connection in the pool; removes it from m_idle.
    std::unique_ptr<Connection> evict_oldest_locked()
    {
        auto victim_origin = m_idle.end();
        std::size_t victim_index = 0;
        std::chrono::steady_clock::time_point oldest{};
        for (auto it = m_idle.begin(); it != m_idle.end(); ++it) {
            auto& list = it->second;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (victim_origin == m_idle.end() || list[i]->idle_since < oldest) {
                    oldest = list[i]->idle_since;
                    victim_origin = it;
                    victim_index = i;
                }
            }
        }
        if (victim_origin == m_idle.end())
            return nullptr;
        auto& list = victim_origin->second;
        auto victim = std::move(list[victim_index]);
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(victim_index));
        if (list.empty())
            m_idle.erase(victim_origin);
        --m_idle_total;
        return victim;
    }

    aio::any_io_executor m_executor;
    PoolOptions m_options;
    mutable std::mutex m_mutex;
    std::flat_map<std::string, std::vector<std::unique_ptr<Connection>>> m_idle;
    std::size_t m_idle_total = 0; // sum of m_idle[*].size()
    struct dns_entry {
        endpoints_type endpoints;
        std::chrono::steady_clock::time_point resolved_at;
    };
    std::flat_map<std::string, dns_entry> m_dns; // guarded by m_mutex
    std::mutex m_tls_mutex;
    std::shared_ptr<aio::ssl::context> m_tls_context;
};

// Response plus whether the connection may be reused (HTTP keep-alive).
struct ExchangeResult {
    Response response;
    bool keep_alive = false;
};

// Write `req` and read the response on a connected (and, for TLS, handshaken)
// stream; `buffer` must be empty on entry and is empty again on success.
template <class Stream>
std::execution::task<ExchangeResult> exchange(Stream& stream,
                                              const boost::beast::http::request<boost::beast::http::string_body>& req,
                                              boost::beast::flat_buffer& buffer)
{
    co_await http_write(stream, req);

    boost::beast::http::response<boost::beast::http::string_body> res;
    co_await http_read(stream, buffer, res);

    ExchangeResult out;
    out.keep_alive = res.keep_alive(); // false for HTTP/1.0 or "Connection: close"
    out.response = make_response(std::move(res));
    co_return out;
}

// Exchange on a checked-out connection (plain or TLS), bounded by the
// connection's deadline.
inline std::execution::task<ExchangeResult> exchange_on(
    Connection& conn, const boost::beast::http::request<boost::beast::http::string_body>& req,
    std::chrono::milliseconds timeout)
{
    conn.reusable.store(true); // until the deadline says otherwise
    DeadlineGuard deadline(conn, conn.socket(), timeout);
    try {
        co_return co_await std::visit(
            [&](auto& ptr) -> std::execution::task<ExchangeResult> {
                co_return co_await exchange(*ptr, req, conn.buffer);
            },
            conn.stream);
    } catch (...) {
        /* A timeout (or any failure) leaves the connection in an unknown state:
         * never pool it again. */
        conn.reusable.store(false);
        throw;
    }
}

// Connect to the first reachable endpoint, bounded by the connection's deadline.
inline std::execution::task<void> connect_to(Connection& conn, aio::ip::tcp::socket& socket,
                                             const endpoints_type& endpoints,
                                             std::chrono::milliseconds timeout)
{
    DeadlineGuard deadline(conn, socket, timeout);
    std::exception_ptr last_error;
    for (const auto& entry : endpoints) {
        try {
            co_await socket_connect(socket, entry.endpoint());
            co_return;
        } catch (...) {
            last_error = std::current_exception();
        }
    }
    if (last_error)
        std::rethrow_exception(last_error);
    throw std::runtime_error("Http: no endpoint to connect to");
}

} // namespace detail

class Client {
public:
    explicit Client(Async& async,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10),
                    std::string ca_bundle = {},
                    PoolOptions pool = {})
        : m_executor(async.get_executor())
        , m_timeout(timeout)
        , m_ca_bundle(std::move(ca_bundle))
        , m_pool(std::make_shared<detail::ConnectionPool>(m_executor, pool))
    {
    }

    auto get_executor() const noexcept { return m_executor; }

    std::execution::task<Response> get(std::string url) const
    {
        return request("GET", std::move(url));
    }

    std::execution::task<Response> post(std::string url, std::string body, std::string content_type) const
    {
        return request("POST", std::move(url), std::move(body), std::move(content_type));
    }

    /* ---------- pool introspection ---------- */

    const PoolOptions& pool_options() const noexcept { return m_pool->options(); }

    // Idle (reusable) connections kept by this client, overall / for one URL's
    // origin. Diagnostics and tests.
    std::size_t idle_connections() const { return m_pool->idle_count(); }
    std::size_t idle_connections(std::string_view url) const
    {
        return m_pool->idle_count(detail::parse_url(url).origin());
    }

    // Drop every idle connection now (checked-out connections are unaffected).
    void close_idle() const { m_pool->close_idle(); }

    // Forget the cached DNS results (they expire on their own after
    // PoolOptions::dns_cache_ttl).
    void clear_dns_cache() const { m_pool->clear_dns_cache(); }

    // One exchange: a pooled connection when one is available, otherwise a new
    // one. Everything the coroutine needs is copied before the first suspension,
    // so the Client may be a temporary or destroyed mid-flight.
    std::execution::task<Response> request(std::string method, std::string url,
                                           std::string body = {}, std::string content_type = {}) const
    {
        namespace http = boost::beast::http;

        const auto parts = detail::parse_url(url);
        const auto origin = parts.origin();
        const auto timeout = m_timeout;
        const auto ca_bundle = m_ca_bundle;
        const auto keep_alive = m_pool->options().keep_alive;
        const bool tls = parts.scheme == "https";
        auto pool = m_pool; // keeps the pool (and its sockets) alive while in flight

        http::request<http::string_body> req;
        const auto verb = http::string_to_verb(method);
        if (verb == http::verb::unknown)
            throw std::invalid_argument("Http: unsupported method '" + method + "'");
        req.method(verb);
        req.target(parts.target);
        req.version(11);
        req.set(http::field::host, parts.host);
        req.set(http::field::user_agent, "HeliosView/1.0");
        req.keep_alive(keep_alive); // "Connection: close" when pooling is off
        if (!body.empty()) {
            req.body() = std::move(body);
            req.prepare_payload();
        }
        if (!content_type.empty())
            req.set(http::field::content_type, std::move(content_type));

        /* A pooled connection for this origin, if any. `reused` records where it
         * came from: only a pooled connection may be replaced and the request
         * replayed (and only when the method is idempotent). */
        std::unique_ptr<detail::Connection> conn;
        bool reused = false;
        if (keep_alive) {
            conn = pool->acquire(origin);
            reused = conn != nullptr;
        }

        for (;;) {
            if (!conn) {
                auto endpoints = pool->cached_endpoints(origin);
                if (!endpoints) {
                    detail::aio::ip::tcp::resolver resolver{m_executor};
                    endpoints = co_await detail::resolve_op(resolver, parts.host, parts.port);
                    pool->store_endpoints(origin, *endpoints);
                }

                conn = std::make_unique<detail::Connection>(m_executor);
                conn->origin = origin;
                conn->tls = tls;
                reused = false;
                if (tls) {
                    auto ctx = pool->tls_context(ca_bundle);
                    auto stream = std::make_unique<detail::tls_stream>(m_executor, *ctx);
                    co_await detail::connect_to(*conn, stream->next_layer(), *endpoints, timeout);
                    if (!SSL_set_tlsext_host_name(stream->native_handle(), parts.host.c_str()))
                        throw std::runtime_error("Http: SSL_set_tlsext_host_name failed");
                    conn->context = std::move(ctx);
                    conn->stream = std::move(stream);
                    detail::DeadlineGuard deadline(*conn, conn->socket(), timeout);
                    co_await detail::tls_handshake(
                        *std::get<std::unique_ptr<detail::tls_stream>>(conn->stream));
                } else {
                    auto stream = std::make_unique<detail::plain_stream>(m_executor);
                    co_await detail::connect_to(*conn, *stream, *endpoints, timeout);
                    conn->stream = std::move(stream);
                }
            }

            try {
                auto out = co_await detail::exchange_on(*conn, req, timeout);
                if (keep_alive && out.keep_alive)
                    pool->release(std::move(conn));
                co_return std::move(out.response);
            } catch (...) {
                /* The server closed the idle connection (or the exchange failed):
                 * the connection is gone either way. Replay a pooled connection
                 * once, and only for a side-effect-free method. */
                conn.reset();
                if (reused && detail::is_idempotent(verb)) {
                    reused = false;
                    continue;
                }
                throw;
            }
        }
    }

private:
    helios::asio::asio_impl::any_io_executor m_executor;
    std::chrono::milliseconds m_timeout;
    std::string m_ca_bundle;
    std::shared_ptr<detail::ConnectionPool> m_pool;
};

} // namespace helios::http
