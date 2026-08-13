#pragma once

/**
 * HeliosView.Core -- Http: client-style async HTTP client (header-only wrapper
 * over the C layer).
 *
 * A HttpClient owns an SSL context + is bound to an Async loop and issues
 * GET/POST/... requests over http:// or https://. TLS is implemented with
 * OpenSSL; connections are established through the loop's async socket layer;
 * responses are parsed with http-parser. Both a callback API and a
 * std::execution sender API are provided, so it composes with coroutines:
 *
 *   helios::HttpClient http(async);
 *   helios::HttpResponse resp = co_await http.requestAsync(
 *       helios::HttpRequest("GET", "https://store.steampowered.com/api/appdetails?appids=730"));
 *   if (resp.ok()) { auto j = resp.json(); ... }
 *
 * Convenience senders cover the common verbs (same semantics, less typing);
 * trailing headers (and a Content-Type for raw bodies) are optional:
 *   co_await http.get(url) / http.get(url, headers)
 *   co_await http.post(url, body[, contentType][, headers])  // raw string body
 *   co_await http.post(url, body, headers)                   // raw body, no Content-Type
 *   co_await http.post(url, value[, headers])                // any JSON-serializable value
 *   co_await http.put(url, body[, contentType][, headers])   // raw string body
 *   co_await http.put(url, value[, headers])                 // any JSON-serializable value
 *   co_await http.del(url)
 *
 * HttpRequest:
 *   method  "GET" / "POST" / ...
 *   url     http(s)://host[:port][/path][?query]
 *   headers std::vector<HttpHeader> (use addHeader() / setJsonBody())
 *   body    raw request body bytes
 *
 * HttpResponse:
 *   status  HTTP status code (200 ...); 0 when the transport failed
 *   headers response headers as a std::vector<HttpHeader> (de-duplicated, last wins)
 *   body    response body bytes
 *   ok() / header(name) / json() convenience helpers
 *
 * On transport failure the callback API delivers a response with status == 0;
 * the sender API throws helios::IoError at the co_await point.
 *
 * Limitations: HTTP/1.1 only; one connection per request (Connection: close, no
 * keep-alive or pooling); no redirect following; no IPv6-literal or userinfo
 * URLs; the response body is fully buffered without a size cap. Response header
 * de-duplication collapses legitimate duplicates (e.g. multiple Set-Cookie).
 * Do not set Host / Content-Length yourself — the client generates them.
 */

#include <HeliosViewCore/Async.h>

#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace helios {

/* ---------- request / response types ---------- */

// A single HTTP header (name / value).
struct HttpHeader {
    std::string name;
    std::string value;

    HttpHeader() = default;
    HttpHeader(std::string name_, std::string value_)
        : name(std::move(name_)), value(std::move(value_))
    {
    }
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;

    HttpRequest() = default;
    HttpRequest(std::string method_, std::string url_)
        : method(std::move(method_)), url(std::move(url_))
    {
    }

    // Append a header (order preserved; duplicates kept as given).
    HttpRequest& addHeader(std::string name, std::string value)
    {
        headers.emplace_back(std::move(name), std::move(value));
        return *this;
    }

    // Append multiple headers (order preserved).
    HttpRequest& addHeaders(std::vector<HttpHeader> hdrs)
    {
        for (auto& h : hdrs)
            headers.push_back(std::move(h));
        return *this;
    }

    // Convenience: set a JSON request body + Content-Type header.
    HttpRequest& setJsonBody(const nlohmann::json& j)
    {
        body = j.dump();
        return addHeader("Content-Type", "application/json");
    }
};

struct HttpResponse {
    int status = 0;                    // HTTP status code; 0 = transport failure
    std::vector<HttpHeader> headers;   // response headers (de-duplicated, last wins;
                                       // collapses duplicates such as multiple Set-Cookie)
    std::string body;                  // response body bytes

    bool ok() const { return status >= 200 && status < 300; }

    // Value of the first header named `name` (case-insensitive), or "" if absent.
    std::string header(const std::string& name) const
    {
        for (const auto& h : headers) {
            if (ciEqual(h.name, name))
                return h.value;
        }
        return {};
    }

    // The body parsed as JSON; null when it is not valid JSON.
    nlohmann::json json() const
    {
        return nlohmann::json::parse(body, nullptr, false);
    }

private:
    static bool ciEqual(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c; };
            if (lower(a[i]) != lower(b[i]))
                return false;
        }
        return true;
    }
};

/* ---------- implementation ---------- */

namespace detail {

// Build a C header collection from a vector<HttpHeader>. The returned shared_ptr
// owns the C handle (destroyed with heliosview_http_headers_destroy). May throw.
inline std::shared_ptr<heliosview_http_headers_t> makeCHeaders(const std::vector<HttpHeader>& hdrs)
{
    heliosview_http_headers_t* h = heliosview_http_headers_create();
    if (!h)
        throw std::bad_alloc();
    for (const auto& x : hdrs)
        heliosview_http_headers_add(h, x.name.c_str(), x.value.c_str());
    return std::shared_ptr<heliosview_http_headers_t>(h, heliosview_http_headers_destroy);
}

// Copy a C response into an HttpResponse.
inline void fillResponse(HttpResponse& resp, int error, const heliosview_http_response_t* response)
{
    if (error != 0 || !response)
        return;
    resp.status = response->status_code;
    const size_t n = heliosview_http_headers_count(response->headers);
    resp.headers.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const char* name = nullptr;
        const char* value = nullptr;
        if (heliosview_http_headers_get(response->headers, i, &name, &value) == 0)
            resp.headers.emplace_back(name, value);
    }
    if (response->body && response->body_len)
        resp.body.assign(response->body, response->body_len);
}

// Sender config for HttpClient::requestAsync (reuses the op_sender machinery in Async.h).
struct http_request_config {
    using value_t = HttpResponse;

    /* req/headers are shared so that op_sender::connect() (which copies the op
     * data) only bumps reference counts instead of copying the request again */
    struct data_t {
        heliosview_http_client_t* client = nullptr;
        std::shared_ptr<HttpRequest> req;
        std::shared_ptr<heliosview_http_headers_t> headers_guard;
        heliosview_http_headers_t* headers = nullptr;
    };

    static int start(void* sink, data_t& d)
    {
        const auto* handle = heliosview_http_client_request(
            d.client, d.req->method.c_str(), d.req->url.c_str(), d.headers,
            d.req->body.data(), d.req->body.size(), &http_tramp, sink);
        return handle ? 0 : -1;
    }

    static void http_tramp(heliosview_http_request_t* request, int error,
                           const heliosview_http_response_t* response, void* userdata)
    {
        auto* sink = static_cast<op_sink<value_t>*>(userdata);
        value_t resp;
        fillResponse(resp, error, response);
        sink->complete_move(error, &resp); /* move: no second copy of the body */
        heliosview_http_request_destroy(request); /* the C handle is caller-owned */
    }
};

// Tramp for the callback API: builds the response and destroys the C handle.
template <class Ctx>
void httpCallbackTramp(heliosview_http_request_t* request, int error,
                       const heliosview_http_response_t* response, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    HttpResponse resp;
    fillResponse(resp, error, response);
    ctx->fn(std::move(resp));
    destroyCtx(ctx);
    heliosview_http_request_destroy(request);
}

} // namespace detail

/* ---------- public API ---------- */

// An async HTTP client: owns an SSL context and is bound to the given Async loop.
// The client must outlive every in-flight request; destroy it only when none are
// pending. Construction loads the Windows system CA store synchronously (a
// one-time cost paid per client, on the constructing thread).
class HttpClient {
public:
    explicit HttpClient(const Async& async)
        : m_client(heliosview_http_client_create(async.handle()))
    {
    }
    ~HttpClient() { heliosview_http_client_destroy(m_client); }
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // The underlying C client handle (for direct C-layer use).
    heliosview_http_client_t* handle() const noexcept { return m_client; }

    // Set the per-request timeout (covers connect + the whole exchange). 0 = no
    // timeout. A request that exceeds it fails with error == HELIOSVIEW_HTTP_TIMEOUT.
    // Thread-safe; the value in effect at submission time applies to each request.
    void setTimeout(uint32_t timeoutMs) { heliosview_http_client_set_timeout(m_client, timeoutMs); }

    // Sender API: co_await it, or compose with the std::execution operators.
    // set_value(HttpResponse) on success; set_error(std::exception_ptr(IoError)) on
    // transport failure. The exchange runs on a loop worker thread.
    // Cancellation is NOT propagated: the awaiting task must not be destroyed
    // before the sender completes (e.g. via an exception while suspended) — the
    // request keeps running and its completion target would dangle. Await to
    // completion, or rely on the client timeout to finish the exchange.
    auto requestAsync(HttpRequest req) const
    {
        detail::http_request_config::data_t data{};
        data.client = m_client;
        data.headers_guard = detail::makeCHeaders(req.headers);
        data.headers = data.headers_guard.get();
        data.req = std::make_shared<HttpRequest>(std::move(req));
        return detail::op_sender<detail::http_request_config>{std::move(data)};
    }

    // ---- convenience senders (co_await them) ----
    // Shorthands for requestAsync(HttpRequest(...)): same semantics — the
    // exchange runs on a loop worker thread, transport failures throw
    // helios::IoError at the co_await point, and cancellation is not
    // propagated (see requestAsync).

    // GET
    auto get(std::string url) const { return requestAsync(HttpRequest("GET", std::move(url))); }
    // GET with request headers
    auto get(std::string url, std::vector<HttpHeader> headers) const
    {
        HttpRequest req("GET", std::move(url));
        req.headers = std::move(headers);
        return requestAsync(std::move(req));
    }

    // POST a raw body; contentType and headers are optional (omitted -> absent)
    auto post(std::string url, std::string body, std::string contentType = {},
              std::vector<HttpHeader> headers = {}) const
    {
        HttpRequest req("POST", std::move(url));
        req.body = std::move(body);
        if (!contentType.empty())
            req.addHeader("Content-Type", std::move(contentType));
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // POST a raw body + headers (no Content-Type)
    auto post(std::string url, std::string body, std::vector<HttpHeader> headers) const
    {
        HttpRequest req("POST", std::move(url));
        req.body = std::move(body);
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // POST a raw body given as a string_view (kept as text, like the
    // std::string overload). A constrained template so const char* still binds
    // the std::string overload without ambiguity. contentType / headers optional.
    template <class S>
        requires std::same_as<std::decay_t<S>, std::string_view>
    auto post(std::string url, S body, std::string_view contentType = {},
              std::vector<HttpHeader> headers = {}) const
    {
        HttpRequest req("POST", std::move(url));
        req.body.assign(body);
        if (!contentType.empty())
            req.addHeader("Content-Type", std::string(contentType));
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // POST a raw body given as a string_view + headers (no Content-Type)
    template <class S>
        requires std::same_as<std::decay_t<S>, std::string_view>
    auto post(std::string url, S body, std::vector<HttpHeader> headers) const
    {
        HttpRequest req("POST", std::move(url));
        req.body.assign(body);
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // POST a JSON body: any value nlohmann/json can serialize (an
    // nlohmann::json, a DTO with NLOHMANN_DEFINE_TYPE / an ADL to_json, maps,
    // vectors, numbers, ...) is serialized directly (Content-Type:
    // application/json). headers are optional. Text-like arguments (std::string
    // / const char* / std::string_view) are excluded so they keep binding to
    // the raw-body overloads above.
    template <class T>
        requires std::constructible_from<nlohmann::json, T> &&
                 (!std::same_as<std::decay_t<T>, std::string> &&
                  !std::same_as<std::decay_t<T>, std::string_view> &&
                  !std::convertible_to<T, const char*>)
    auto post(std::string url, T&& value, std::vector<HttpHeader> headers = {}) const
    {
        HttpRequest req("POST", std::move(url));
        req.setJsonBody(nlohmann::json(std::forward<T>(value)));
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // PUT a raw body; contentType and headers are optional (omitted -> absent)
    auto put(std::string url, std::string body, std::string contentType = {},
             std::vector<HttpHeader> headers = {}) const
    {
        HttpRequest req("PUT", std::move(url));
        req.body = std::move(body);
        if (!contentType.empty())
            req.addHeader("Content-Type", std::move(contentType));
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // PUT a raw body + headers (no Content-Type)
    auto put(std::string url, std::string body, std::vector<HttpHeader> headers) const
    {
        HttpRequest req("PUT", std::move(url));
        req.body = std::move(body);
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // PUT a raw body given as a string_view (kept as text, like the
    // std::string overload; constrained template, see the post overload).
    // contentType / headers optional.
    template <class S>
        requires std::same_as<std::decay_t<S>, std::string_view>
    auto put(std::string url, S body, std::string_view contentType = {},
             std::vector<HttpHeader> headers = {}) const
    {
        HttpRequest req("PUT", std::move(url));
        req.body.assign(body);
        if (!contentType.empty())
            req.addHeader("Content-Type", std::string(contentType));
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // PUT a raw body given as a string_view + headers (no Content-Type)
    template <class S>
        requires std::same_as<std::decay_t<S>, std::string_view>
    auto put(std::string url, S body, std::vector<HttpHeader> headers) const
    {
        HttpRequest req("PUT", std::move(url));
        req.body.assign(body);
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // PUT a JSON body: same as the post overload (any JSON-serializable value);
    // headers are optional
    template <class T>
        requires std::constructible_from<nlohmann::json, T> &&
                 (!std::same_as<std::decay_t<T>, std::string> &&
                  !std::same_as<std::decay_t<T>, std::string_view> &&
                  !std::convertible_to<T, const char*>)
    auto put(std::string url, T&& value, std::vector<HttpHeader> headers = {}) const
    {
        HttpRequest req("PUT", std::move(url));
        req.setJsonBody(nlohmann::json(std::forward<T>(value)));
        return requestAsync(req.addHeaders(std::move(headers)));
    }

    // DELETE
    auto del(std::string url) const { return requestAsync(HttpRequest("DELETE", std::move(url))); }

    // Callback API: onResponse(HttpResponse) fires exactly once, on a loop worker
    // thread (concurrent with other worker callbacks). status == 0 on failure.
    // The callback is owned by the pool until it fires; capture nothing that may be
    // destroyed first, and do not throw from it (it runs inside a C callback that
    // crosses the DLL boundary — an exception would escape into the loop and
    // terminate the process). On a synchronous submission failure (e.g. an invalid
    // URL) the callback runs inline on the calling thread instead.
    template <class Fn>
    void request(HttpRequest req, Fn&& onResponse)
    {
        using F = std::decay_t<Fn>;
        struct Ctx {
            std::pmr::memory_resource* resource;
            F fn;
        };
        auto headers = detail::makeCHeaders(req.headers);
        auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(), std::forward<Fn>(onResponse));

        auto* handle = heliosview_http_client_request(
            m_client, req.method.c_str(), req.url.c_str(), headers.get(),
            req.body.data(), req.body.size(), &detail::httpCallbackTramp<Ctx>, ctx);
        if (!handle) {
            ctx->fn(HttpResponse{});
            detail::destroyCtx(ctx);
        }
    }

private:
    heliosview_http_client_t* m_client;
};

} // namespace helios
