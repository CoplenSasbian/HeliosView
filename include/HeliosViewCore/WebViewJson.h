#pragma once

/**
 * HeliosView.Core -- WebViewJson: nlohmann auto-binding for the WebView JS <-> native bridge.
 *
 * On top of the raw C bridge (heliosview_webview_bind / resolve / reject, JSON strings)
 * this header adds type-safe, JSON-serialized bindings:
 *
 *   struct AddReq { int a; int b; };          // + NLOHMANN_DEFINE_TYPE_INTRUSIVE / NON_INTRUSIVE
 *   window->bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<std::string> {
 *       co_await std::execution::schedule(async.get_scheduler());  // run off the UI thread
 *       co_return std::format("{}", req.a + req.b);
 *   });
 *
 * How a JS call round-trips:
 *   - The JS call's first argument is parsed into a Req (nlohmann::json::parse +
 *     get<Req>()); if that fails, the Promise is rejected with {"error": ...}.
 *   - The handler runs as a detached std::execution::task. Its completion value is
 *     serialized with nlohmann::json and delivered with resolve (thread-safe in the C
 *     layer, so the task may complete on any thread). A thrown exception / set_error /
 *     std::exception_ptr error is converted to a reject payload {"error": what()}.
 *   - The coroutine's error channel includes std::exception_ptr by default (the
 *     std::execution::task default), so handler exceptions and co_await failures are
 *     catchable.
 *
 * Response shapes (the task's value type):
 *   - Resp                    -> resolve with the JSON encoding of Resp
 *   - nlohmann::json          -> resolve with that JSON directly
 *   - JsonResp<T>             -> resolve with the top-level object {"<key>": value}
 *   - JsonError<T>            -> reject with the top-level object {"<key>": value}
 *   - void                    -> resolve with null
 *
 * Requirements: nlohmann::json (single header or FetchContent nlohmann_json) and
 * HeliosViewCore/Execution.h (stdexec under C++23, std::execution under C++26).
 * Lifetime: destroy the WebView only when no bindJson task is still in flight.
 */

#include <HeliosViewCore/Execution.h>
#include <HeliosViewCore/WebViewWindow.h>

#include <exception>
#include <string>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace helios {

/* ---------- convenience response wrappers ---------- */

// Resolve the Promise with the top-level JSON object {"<key>": value}
// (the value itself may be any JSON type, including a DTO)
template <class T>
struct JsonResp {
    std::string key;
    T value;

    JsonResp(std::string k, T v) : key(std::move(k)), value(std::move(v)) {}
};

// Reject the Promise with the top-level JSON object {"<key>": value}
// (value is typically a human-readable message string)
template <class T>
struct JsonError {
    std::string key;
    T value;

    JsonError(std::string k, T v) : key(std::move(k)), value(std::move(v)) {}
};

namespace detail {

/* ---------- response serialization ---------- */

// nlohmann::json values dump directly
inline std::string jsonDump(const nlohmann::json& j)
{
    return j.dump();
}

// Serialize any value through nlohmann::json. A std::string / const char* result that
// is itself JSON text (e.g. built by an nlohmann::json::dump()) is parsed once and
// re-dumped so it stays a single JSON value instead of being double-encoded.
template <class T>
std::string jsonDumpValue(const T& value)
{
    nlohmann::json j = value;
    if constexpr (std::is_same_v<std::decay_t<T>, std::string>
                  || std::is_same_v<std::decay_t<T>, const char*>) {
        try {
            j = nlohmann::json::parse(value);
        } catch (...) { /* not JSON text: keep the plain string value */ }
    }
    return j.dump();
}

// Generic response serialization
template <class Resp>
std::string jsonDump(const Resp& resp)
{
    return jsonDumpValue(resp);
}

// JsonResp<T>: {"<key>": value}
template <class T>
std::string jsonDump(const JsonResp<T>& resp)
{
    return (nlohmann::json{{resp.key, resp.value}}).dump();
}

/* ---------- error payload ---------- */

inline nlohmann::json errorToJson(std::exception_ptr eptr)
{
    if (!eptr)
        return {{"error", "unknown error"}};
    try {
        std::rethrow_exception(eptr);
    } catch (const nlohmann::json::exception& e) {
        return {{"error", e.what()}};
    } catch (const std::exception& e) {
        return {{"error", e.what()}};
    } catch (...) {
        return {{"error", "unknown error"}};
    }
}

/* ---------- completion delivery ---------- */

// Resolve / reject the pending JS Promise from the handler's completion value.
// JsonError is the only shape that rejects; everything else resolves.
template <class Resp>
void replyJson(heliosview_webview_t* wv, uint64_t call_id, Resp&& resp)
{
    heliosview_webview_resolve(wv, call_id, jsonDump(resp).c_str());
}

template <class T>
void replyJson(heliosview_webview_t* wv, uint64_t call_id, JsonError<T>&& err)
{
    heliosview_webview_reject(wv, call_id, (nlohmann::json{{err.key, err.value}}).dump().c_str());
}

/* ---------- the in-flight call state and its receiver ---------- */

// The state for one JS call: the webview + call id (for resolve/reject) plus the
// connected operation of the handler coroutine. Heap-allocated per call and
// self-destroying: the receiver deletes it when the coroutine completes.
template <class Sender>
struct JsonCallState;

// Receiver for the handler coroutine. set_value resolves the Promise with the
// serialized completion value (or rejects for JsonError); set_error rejects with
// {"error": ...}; set_stopped just discards. Every path deletes the state (the op
// state lives inside it), mirroring stdexec's self-destroying start_detached pattern:
// delete must be the last use of `state`, since it may run inline inside start().
template <class Sender>
struct JsonCallReceiver {
    using receiver_concept = std::execution::receiver_t;

    JsonCallState<Sender>* state;

    template <class... Values>
    void set_value(Values&&... values) const noexcept
    {
        static_assert(sizeof...(Values) <= 1,
                      "bindJson handler task must complete with at most one value");
        try {
            if constexpr (sizeof...(Values) == 0) {
                heliosview_webview_resolve(state->wv, state->call_id, "null");
            } else {
                replyJson(state->wv, state->call_id, std::forward<Values>(values)...);
            }
        } catch (...) { /* a serialization failure must not escape a receiver */ }
        delete state;
    }

    void set_error(std::exception_ptr eptr) const noexcept
    {
        try {
            heliosview_webview_reject(state->wv, state->call_id,
                                      errorToJson(std::move(eptr)).dump().c_str());
        } catch (...) { /* never throw out of a receiver */ }
        delete state;
    }

    void set_stopped() const noexcept
    {
        delete state;
    }

    // Provide a start scheduler so a connected std::execution::task can be awaited:
    // task's as_awaitable requires the parent environment to answer get_start_scheduler.
    // inline_scheduler runs the coroutine on the thread that started it (the UI thread
    // for a JS call); a co_await of a sender continues on that sender's completion thread.
    constexpr auto get_env() const noexcept
    {
        return std::execution::env{std::execution::prop{std::execution::get_start_scheduler,
                                                        std::execution::inline_scheduler{}}};
    }
};

template <class Sender>
struct JsonCallState {
    heliosview_webview_t* wv;
    uint64_t call_id;
    std::execution::connect_result_t<Sender, JsonCallReceiver<Sender>> op;

    template <class Coro>
    JsonCallState(Coro&& coro, heliosview_webview_t* wv_, uint64_t id)
        : wv(wv_)
        , call_id(id)
        , op(std::execution::connect(std::forward<Coro>(coro), JsonCallReceiver<Sender>{this}))
    {
    }
};

/* ---------- the C bind callback ---------- */

// Parse the JS call's first argument into Req and run the handler as a detached task.
template <class Sender, class Fn, class Req>
struct JsonHandler {
    static void invoke(heliosview_webview_t* wv, uint64_t call_id,
                       const char* name, const char* args_json, void* userdata)
    {
        (void)name;
        auto* fn = static_cast<Fn*>(userdata);
        try {
            nlohmann::json args = (args_json && *args_json)
                                      ? nlohmann::json::parse(args_json)
                                      : nlohmann::json::array();
            Req req = args[0].get<Req>();

            auto coro = fn->operator()(std::move(req));
            auto* state = new JsonCallState<Sender>(std::move(coro), wv, call_id);
            std::execution::start(state->op); // state may be deleted inline on sync completion
        } catch (const std::exception& e) {
            heliosview_webview_reject(wv, call_id, (nlohmann::json{{"error", e.what()}}).dump().c_str());
        } catch (...) {
            heliosview_webview_reject(wv, call_id, R"({"error":"unknown error"})");
        }
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// WebViewWindow::bindJson implementation (declared in WebViewWindow.h).
//
// Req: the DTO to deserialize the JS call's first argument into (any type
//      nlohmann::json can construct). Handler signature: (Req) -> std::execution::task<Resp>.
//      The handler may capture anything it needs (e.g. the window or an Async).
// ---------------------------------------------------------------------------
template <class Req, class Fn>
void WebViewWindow::bindJson(const char* name, Fn&& handler)
{
    using Sender = std::decay_t<decltype(handler(std::declval<Req>()))>;
    static_assert(std::execution::sender<Sender>,
                  "bindJson handler must return a sender (e.g. std::execution::task<Resp>)");

    // The userdata is a heap copy of the handler (lifetime = the binding); the C layer
    // destroys it via the userdata_dtor when the binding is replaced or the webview dies.
    auto* fn = new Fn(std::forward<Fn>(handler));
    heliosview_webview_bind(m_webview, name,
                            &detail::JsonHandler<Sender, Fn, Req>::invoke,
                            fn,
                            [](void* userdata) { delete static_cast<Fn*>(userdata); });
}

} // namespace helios
