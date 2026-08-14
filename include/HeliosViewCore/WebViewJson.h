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
 * Broadcasts (the reverse direction) are covered by subscribeJson: the page's
 * BroadcastChannel(name).postMessage(value) is deserialized into a Req DTO and
 * delivered to a void(Req) callback on the UI thread. The shim subclassing
 * BroadcastChannel keeps native broadcasts (broadcast()) and the standard
 * same-origin channel working together.
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

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
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

/* ---------- allocation through the default PMR memory resource ---------- */

// The C bridge stores each binding's callable in a void* userdata that must outlive
// the bind call. The lambda overloads of bindJson/subscribeJson therefore have to
// store the handler somewhere; they allocate it through the **default PMR memory
// resource** (std::pmr::get_default_resource()), so the app can redirect where these
// allocations land with std::pmr::set_default_resource() (e.g. a pool / monotonic
// buffer). The member-function overloads avoid this entirely (they point the userdata
// at the user-owned object directly).

// A heap copy of the handler whose first member records the memory_resource that
// owns it: pmrRelease reads it back instead of re-querying the process default,
// so the binding stays correct even if the default resource is swapped while the
// binding is alive.
template <class Fn>
struct pmr_box {
    std::pmr::memory_resource* resource;
    Fn value;

    template <class... Args>
    pmr_box(std::pmr::memory_resource* r, Args&&... args)
        : resource(r), value(std::forward<Args>(args)...)
    {
    }
};

// Allocate and construct a callable of type T with the default PMR resource.
template <class T, class... Args>
pmr_box<T>* pmrAllocate(Args&&... args)
{
    auto* res = std::pmr::get_default_resource();
    void* p = res->allocate(sizeof(pmr_box<T>), alignof(pmr_box<T>));
    try {
        return ::new (p) pmr_box<T>(res, std::forward<Args>(args)...);
    } catch (...) {
        res->deallocate(p, sizeof(pmr_box<T>), alignof(pmr_box<T>));
        throw;
    }
}

// Destroy and deallocate a pmr_box that was allocated via pmrAllocate, using the
// resource recorded inside it (never re-queried from the process default).
template <class T>
void pmrRelease(pmr_box<T>* box) noexcept
{
    if (!box)
        return;
    auto* res = box->resource;
    box->~pmr_box<T>();
    res->deallocate(box, sizeof(pmr_box<T>), alignof(pmr_box<T>));
}

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

// Parse each JS call argument (args[i]) into the matching Args[i] and run the
// handler as a detached task. Supports zero, one, or many parameters.
template <class Sender, class Fn, class... Args>
struct JsonHandler {
    static void invoke(heliosview_webview_t* wv, uint64_t call_id,
                       const char* name, const char* args_json, void* userdata)
    {
        (void)name;
        auto* fn = &static_cast<pmr_box<Fn>*>(userdata)->value;
        try {
            nlohmann::json args = (args_json && *args_json)
                                      ? nlohmann::json::parse(args_json)
                                      : nlohmann::json::array();

            auto make = [&]<std::size_t... I>(std::index_sequence<I...>) {
                return fn->operator()(args[I].template get<Args>()...);
            };
            auto coro = make(std::index_sequence_for<Args...>{});

            auto* state = new JsonCallState<Sender>(std::move(coro), wv, call_id);
            std::execution::start(state->op); // state may be deleted inline on sync completion
        } catch (const std::exception& e) {
            heliosview_webview_reject(wv, call_id, (nlohmann::json{{"error", e.what()}}).dump().c_str());
        } catch (...) {
            heliosview_webview_reject(wv, call_id, R"({"error":"unknown error"})");
        }
    }
};

// Member-function bind trampoline removed: the member overloads now wrap (obj, method)
// into a lambda and forward to the generic bindJson<Args...> / subscribeJson<Req>.
} // namespace detail

// ---------------------------------------------------------------------------
// WebViewWindow::bindJson implementation (declared in WebViewWindow.h).
//
// Args...: the parameter types deserialized from the JS call's arguments array
//      (args[0] -> Args[0], args[1] -> Args[1], ...), each any type nlohmann::json
//      can construct. Handler signature: (Args...) -> std::execution::task<Resp>.
//      The handler may capture anything it needs (e.g. the window or an Async).
// ---------------------------------------------------------------------------
template <class... Args, class Fn>
void WebViewWindow::bindJson(const char* name, Fn&& handler)
{
    using Sender = std::decay_t<decltype(handler(std::declval<Args>()...))>;
    static_assert(std::execution::sender<Sender>,
                  "bindJson handler must return a sender (e.g. std::execution::task<Resp>)");

    // The userdata is a pmr_box (resource + handler copy) allocated through the
    // default PMR resource (lifetime = the binding); the C layer destroys it via the
    // userdata_dtor when the binding is replaced or the webview dies.
    auto* box = detail::pmrAllocate<Fn>(std::forward<Fn>(handler));
    heliosview_webview_bind(m_webview, name,
                            &detail::JsonHandler<Sender, Fn, Args...>::invoke,
                            box,
                            [](void* userdata) { detail::pmrRelease(static_cast<detail::pmr_box<Fn>*>(userdata)); });
}

// Member-function overload of bindJson: wraps (obj, method) into a lambda and forwards
// to the generic bindJson<Args...>. The object must outlive the binding.
template <class... Args, class Obj, class MFPtr>
void WebViewWindow::bindJson(const char* name, Obj* obj, MFPtr method)
{
    static_assert(std::is_member_function_pointer_v<MFPtr>,
                  "bindJson member overload expects a member function pointer");
    bindJson<Args...>(name, [obj, method](Args... args) {
        return std::invoke(method, obj, std::move(args)...);
    });
}

// ---------------------------------------------------------------------------
// WebViewWindow::subscribeJson implementation (declared in WebViewWindow.h).
//
// Req: the DTO to deserialize the page's BroadcastChannel(name) postMessage value
//      into (any type nlohmann::json can construct). Callback signature:
//      (Req) -> void, invoked on the UI thread for every JS postMessage on that
//      channel. A value that fails to parse into Req is dropped (there is no
//      promise to reject on a broadcast).
// ---------------------------------------------------------------------------
namespace detail {

// The C subscribe callback: parse the JS postMessage value into Req and invoke the C++ callback.
template <class Req, class Fn>
struct SubscribeHandler {
    static void invoke(heliosview_webview_t* wv, const char* name,
                       const char* data_json, void* userdata)
    {
        (void)wv;
        (void)name;
        auto* fn = &static_cast<pmr_box<Fn>*>(userdata)->value;
        try {
            nlohmann::json data = (data_json && *data_json)
                                      ? nlohmann::json::parse(data_json)
                                      : nlohmann::json();
            Req req = data.get<Req>();
            (*fn)(std::move(req));
        } catch (...) { /* unparseable broadcast: dropped (void callback has no error channel) */ }
    }
};

} // namespace detail

template <class Req, class Fn>
void WebViewWindow::subscribeJson(const char* name, Fn&& callback)
{
    static_assert(std::invocable<Fn&, Req>,
                  "subscribeJson callback must be callable with (Req)");

    // The userdata is a pmr_box (resource + callback copy) allocated through the
    // default PMR resource (lifetime = the subscription); the C layer destroys it via
    // the userdata_dtor when replaced or the webview dies.
    auto* box = detail::pmrAllocate<Fn>(std::forward<Fn>(callback));
    heliosview_webview_subscribe(m_webview, name,
                                 &detail::SubscribeHandler<Req, Fn>::invoke,
                                 box,
                                 [](void* userdata) { detail::pmrRelease(static_cast<detail::pmr_box<Fn>*>(userdata)); });
}

// Member-function overload of subscribeJson: wraps (obj, method) into a lambda and
// forwards to the generic subscribeJson<Req>. The object must outlive the subscription.
template <class Req, class Obj, class MFPtr>
void WebViewWindow::subscribeJson(const char* name, Obj* obj, MFPtr method)
{
    static_assert(std::is_member_function_pointer_v<MFPtr>,
                  "subscribeJson member overload expects a member function pointer");
    subscribeJson<Req>(name, [obj, method](Req req) {
        std::invoke(method, obj, std::move(req));
    });
}

} // namespace helios
