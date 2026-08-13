#pragma once

/**
 * HeliosView.Core -- Http: async HTTP client (header-only wrapper over the C layer).
 *
 * GET/POST/... requests over http:// or https:// (TLS handled by the platform
 * HTTP stack, Windows: WinHTTP). The request runs on a background worker thread
 * of the Async pool; both a callback API and a std::execution sender API are
 * provided, so it composes with coroutines:
 *
 *   helios::HttpResponse resp = co_await async.httpRequestAsync(
 *       helios::HttpRequest("GET", "https://store.steampowered.com/api/appdetails?appids=730"));
 *   if (resp.ok()) { auto j = resp.json(); ... }
 *
 * HttpRequest:
 *   method  "GET" / "POST" / ...
 *   url     http(s)://host[:port][/path][?query]
 *   headers nlohmann::json object of string values ("Name": "value")
 *   body    raw request body bytes (use setJsonBody() for a JSON payload)
 *
 * HttpResponse:
 *   status  HTTP status code (200 ...); 0 when the transport failed
 *   headers response headers as a nlohmann::json object
 *   body    response body bytes
 *   ok() / header(name) / json() convenience helpers
 *
 * On transport failure the callback API delivers a response with status == 0;
 * the sender API throws helios::IoError at the co_await point.
 */

#include <HeliosViewCore/Async.h>

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace helios {

/* ---------- request / response types ---------- */

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    nlohmann::json headers = nlohmann::json::object();
    std::string body;

    HttpRequest() = default;
    HttpRequest(std::string method_, std::string url_)
        : method(std::move(method_)), url(std::move(url_))
    {
    }

    // Convenience: set a JSON request body + Content-Type header
    HttpRequest& setJsonBody(const nlohmann::json& j)
    {
        body = j.dump();
        headers["Content-Type"] = "application/json";
        return *this;
    }
};

struct HttpResponse {
    int status = 0;         // HTTP status code; 0 = transport failure
    nlohmann::json headers; // response headers as a JSON object
    std::string body;       // response body bytes

    bool ok() const { return status >= 200 && status < 300; }

    std::string header(const std::string& name) const
    {
        if (headers.contains(name) && headers[name].is_string())
            return headers[name].get<std::string>();
        return {};
    }

    // The body parsed as JSON; null when it is not valid JSON
    nlohmann::json json() const
    {
        return nlohmann::json::parse(body, nullptr, false);
    }
};

/* ---------- implementation ---------- */

namespace detail {

// Sender config for httpRequestAsync (reuses the op_sender machinery in Async.h)
struct http_request_config {
    using value_t = HttpResponse;
    using data_t = struct {
        heliosview_loop_t* loop;
        HttpRequest req;
        std::string headers_json; /* pre-serialized: start() is noexcept */
    };

    static int start(void* sink, data_t& d)
    {
        const auto* handle = heliosview_http_request(
            d.loop, d.req.method.c_str(), d.req.url.c_str(), d.headers_json.c_str(),
            d.req.body.data(), d.req.body.size(), &http_tramp, sink);
        return handle ? 0 : -1;
    }

    static void http_tramp(heliosview_http_request_t* request, int error, int status_code,
                           const char* headers_json, const char* body, size_t body_len,
                           void* userdata)
    {
        auto* sink = static_cast<op_sink<value_t>*>(userdata);
        value_t resp;
        if (error == 0) {
            resp.status = status_code;
            try {
                resp.headers = nlohmann::json::parse(headers_json ? headers_json : "{}");
            } catch (...) { /* non-JSON headers: keep {} */ }
            if (body && body_len)
                resp.body.assign(body, body_len);
        }
        sink->complete(error, &resp);
        heliosview_http_destroy(request); /* the C handle is caller-owned */
    }
};

// Tramp for the callback API: builds the response and destroys the C handle.
template <class Ctx>
void httpCallbackTramp(heliosview_http_request_t* request, int error, int status_code,
                       const char* headers_json, const char* body, size_t body_len, void* userdata)
{
    auto* ctx = static_cast<Ctx*>(userdata);
    HttpResponse resp;
    if (error == 0) {
        resp.status = status_code;
        try {
            resp.headers = nlohmann::json::parse(headers_json ? headers_json : "{}");
        } catch (...) { /* non-JSON headers: keep {} */ }
        if (body && body_len)
            resp.body.assign(body, body_len);
    }
    ctx->fn(std::move(resp));
    destroyCtx(ctx);
    heliosview_http_destroy(request);
}

} // namespace detail

/* ---------- public API ---------- */

// Sender API: co_await it, or compose with the std::execution operators.
// set_value(HttpResponse) on success; set_error(std::exception_ptr(IoError)) on
// transport failure. The request runs on a loop worker thread.
inline auto httpRequestAsync(const Async& async, HttpRequest req)
{
    detail::http_request_config::data_t data{};
    data.loop = async.handle();
    data.req = std::move(req);
    data.headers_json = data.req.headers.dump();
    return detail::op_sender<detail::http_request_config>{std::move(data)};
}

// Callback API: onResponse(HttpResponse) fires exactly once, on a loop worker
// thread (concurrent with other worker callbacks). status == 0 on failure.
// The callback is owned by the pool until it fires; capture nothing that may be
// destroyed first.
template <class Fn>
void httpRequest(Async& async, HttpRequest req, Fn&& onResponse)
{
    using F = std::decay_t<Fn>;
    struct Ctx {
        std::pmr::memory_resource* resource;
        F fn;
    };
    auto* ctx = detail::makeCtx<Ctx>(std::pmr::get_default_resource(),
                                     std::forward<Fn>(onResponse));
    const std::string headers_json = req.headers.dump();
    auto* handle = heliosview_http_request(
        async.handle(), req.method.c_str(), req.url.c_str(), headers_json.c_str(),
        req.body.data(), req.body.size(), &detail::httpCallbackTramp<Ctx>, ctx);
    if (!handle) {
        ctx->fn(HttpResponse{});
        detail::destroyCtx(ctx);
    }
}

} // namespace helios
