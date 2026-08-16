#pragma once

/**
 * HeliosView.Core -- Http: minimal HTTP/HTTPS client on the Async pool.
 *
 * Usage from a bindJson handler (see WebViewJson.h):
 *
 *   helios::Async async;                     // app-scoped member
 *   helios::http::Client client{async};      // lightweight handle; must not outlive async
 *
 *   window->bindJson<Req>("api", [&client](Req r) -> std::execution::task<helios::JsonResp<helios::http::Response>> {
 *       auto resp = co_await client.get(r.url);
 *       co_return helios::JsonResp<helios::http::Response>{"data", std::move(resp)};
 *   });
 *
 * Every request is one complete exchange on the pool: DNS resolve -> connect
 * -> (TLS handshake for https) -> write -> read -> close (a "Connection:
 * close" header is sent). No connection pooling yet — a keep-alive pool will
 * be added under the same API later. A non-2xx status is NOT an error: it is
 * returned in Response::status for the caller to interpret; network/timeout/
 * TLS errors throw (they surface on the task's error channel as
 * set_error(std::exception_ptr)).
 *
 * https:// uses boost::asio::ssl (OpenSSL, vendored at configure time). TLS
 * certificates are verified against the CA bundle passed to the constructor
 * (ca_bundle, a PEM file such as cacert.pem) — or the OpenSSL default paths
 * when empty. Pass an empty ca_bundle to skip loading a file (not to disable
 * verification; pass verify_none only if you really must, e.g. for testing).
 *
 * Lifetime: the Client is a cheap handle (executor + timeout); it may be
 * copied and may even be a temporary — but the Async it was created from must
 * outlive every in-flight request.
 */

#include <HeliosViewCore/Async.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

namespace detail {

struct UrlParts {
    std::string scheme; // "http" | "https"
    std::string host;
    std::string port;
    std::string target;
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

// Write the request and read the response on any beast async stream
// (tcp_stream or ssl_stream<tcp_stream>); the stream must already be
// connected (and, for TLS, handshaken).
template <class Stream>
std::execution::task<Response> exchange(Stream& stream,
                                        const boost::beast::http::request<boost::beast::http::string_body>& req,
                                        boost::beast::flat_buffer& buffer,
                                        std::chrono::milliseconds timeout)
{
    namespace http = boost::beast::http;

    boost::beast::get_lowest_layer(stream).expires_after(timeout); // request deadline
    co_await http::async_write(stream, req, helios::use_sender);

    http::response<http::string_body> res;
    co_await http::async_read(stream, buffer, res, helios::use_sender);
    co_return make_response(std::move(res));
}

} // namespace detail

class Client {
public:
    explicit Client(Async& async,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10),
                    std::string ca_bundle = {})
        : m_executor(async.get_executor())
        , m_timeout(timeout)
        , m_ca_bundle(std::move(ca_bundle))
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

    // One complete exchange: resolve -> connect -> (TLS handshake) -> write ->
    // read -> close. m_executor/m_timeout/m_ca_bundle are read before the
    // first suspension, so the Client may be a temporary or destroyed
    // mid-flight.
    std::execution::task<Response> request(std::string method, std::string url,
                                           std::string body = {}, std::string content_type = {}) const
    {
        namespace beast = boost::beast;
        namespace http = boost::beast::http;
        namespace aio = helios::asio;

        const auto parts = detail::parse_url(url);

        aio::asio_impl::ip::tcp::resolver resolver{m_executor};
        const auto results = co_await resolver.async_resolve(parts.host, parts.port, helios::use_sender);

        http::request<http::string_body> req;
        const auto verb = http::string_to_verb(method);
        if (verb == http::verb::unknown)
            throw std::invalid_argument("Http: unsupported method '" + method + "'");
        req.method(verb);
        req.target(parts.target);
        req.version(11);
        req.set(http::field::host, parts.host);
        req.set(http::field::user_agent, "HeliosView/1.0");
        req.set(http::field::connection, "close"); // no connection pool yet
        if (!body.empty()) {
            req.body() = std::move(body);
            req.prepare_payload();
        }
        if (!content_type.empty())
            req.set(http::field::content_type, std::move(content_type));

        beast::flat_buffer buffer;
        if (parts.scheme == "https") {
            aio::asio_impl::ssl::context ctx{aio::asio_impl::ssl::context::tls_client};
            ctx.set_verify_mode(aio::asio_impl::ssl::verify_peer);
            if (m_ca_bundle.empty())
                ctx.set_default_verify_paths();
            else
                ctx.load_verify_file(m_ca_bundle);

            // beast::ssl_stream is deprecated/empty in Boost 1.92; asio's
            // ssl::stream is the same thing. SNI is set manually below.
            aio::asio_impl::ssl::stream<beast::tcp_stream> stream{m_executor, ctx};
            auto& lowest = beast::get_lowest_layer(stream);
            lowest.expires_after(m_timeout); // connect deadline
            co_await lowest.async_connect(results, helios::use_sender);

            if (!SSL_set_tlsext_host_name(stream.native_handle(), parts.host.c_str()))
                throw std::runtime_error("Http: SSL_set_tlsext_host_name failed");
            lowest.expires_after(m_timeout); // handshake deadline
            co_await stream.async_handshake(aio::asio_impl::ssl::stream_base::client,
                                            helios::use_sender);

            auto resp = co_await detail::exchange(stream, req, buffer, m_timeout);
            try {
                co_await stream.async_shutdown(helios::use_sender); // best-effort
            } catch (...) { /* the server may close first */ }
            co_return resp;
        }

        beast::tcp_stream stream{m_executor};
        stream.expires_after(m_timeout); // connect deadline
        co_await stream.async_connect(results, helios::use_sender);
        co_return co_await detail::exchange(stream, req, buffer, m_timeout);
    }

private:
    helios::asio::asio_impl::any_io_executor m_executor;
    std::chrono::milliseconds m_timeout;
    std::string m_ca_bundle;
};

} // namespace helios::http
