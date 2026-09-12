#pragma once

/**
 * HeliosView.Core -- WebViewJson: Boost.JSON auto-binding for the WebView JS <-> native bridge.
 *
 * On top of the raw C bridge (heliosview_webview_bind / resolve / reject, JSON strings)
 * this header adds type-safe, JSON-serialized bindings:
 *
 *   struct AddReq { int a; int b; };          // + BOOST_DESCRIBE_STRUCT(AddReq, (), (a, b))
 *   helios::Async async;                      // background thread pool (asio-backed), app-scoped
 *   window->bindJson("add", [&async](AddReq req) -> std::execution::task<std::string> {
 *       co_await std::execution::schedule(async.get_scheduler());  // run off the UI thread
 *       co_return std::format("{}", req.a + req.b);
 *   });
 *
 * The parameter types come from the handler (bindJson<AddReq>(...) spells them out
 * explicitly instead); see "handler argument deduction" below for what can be deduced.
 *
 * How a JS call round-trips:
 *   - The JS call's arguments are parsed into the handler's parameter types
 *     (boost::json::parse + boost::json::value_to<T>()); if that fails, the Promise is
 *     rejected with {"error": ...}.
 *   - The handler runs as a detached std::execution::task. Its completion value is
 *     serialized with boost::json and delivered with resolve (thread-safe in the C
 *     layer, so the task may complete on any thread). A thrown exception / set_error /
 *     std::exception_ptr error is converted to a reject payload {"error": what()}.
 *   - The coroutine's error channel includes std::exception_ptr by default (the
 *     std::execution::task default), so handler exceptions and co_await failures are
 *     catchable.
 *   - The task is lazy: the argument deserialized for the handler is destroyed long
 *     before the handler's body runs, so handlers take their parameters by value (the
 *     deduced form rejects a reference parameter; see bind_args).
 *
 * Broadcasts (the reverse direction) are covered by subscribeJson: the page's
 * BroadcastChannel(name).postMessage(value) is deserialized into a Req DTO and
 * delivered to a void(Req) callback on the UI thread. The shim subclassing
 * BroadcastChannel keeps native broadcasts (broadcast()) and the standard
 * same-origin channel working together.
 *
 * Response shapes (the task's value type) — the natural way is to return
 * boost::json::value, built with Boost.JSON's own constructors / value_from:
 *   - boost::json::value      -> resolve with that JSON directly
 *   - Resp (any type boost::json::value_from can convert: DTOs, containers,
 *     strings, numbers, ...) -> resolve with the JSON encoding of Resp
 *   - JsonError               -> reject with the top-level object {"<key>": value}
 *                                (the only value-channel way to reject; throwing
 *                                from the handler rejects with {"error": what()})
 *   - void                    -> resolve with null
 *
 * Requirements: Boost.JSON (vendored with the rest of Boost; the types any
 * boost::json::value_to / value_from can convert, including BOOST_DESCRIBE_STRUCT-
 * annotated DTOs), HeliosViewCore/Execution.h (stdexec under C++23, std::execution
 * under C++26) and — for off-UI-thread work — HeliosViewCore/Async.h (the
 * asio-backed background pool behind `async.get_scheduler()` above).
 * Lifetime: destroy the WebView only when no bindJson task is still in flight,
 * and keep any Async the handlers use alive for at least as long as the bindings.
 */

#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/Execution.h>
#include <HeliosViewCore/WebViewWindow.h>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/json.hpp>

namespace helios {

/* ---------- convenience reject wrapper ---------- */

// Reject the Promise with the top-level JSON object {"<key>": value}
// (value is typically a human-readable message string). This is the only
// value-channel way to reject with a custom payload: handlers normally just
// return a boost::json::value (resolves) or throw (rejects with {"error": ...}).
struct JsonError {
    std::string key;
    boost::json::value value;

    template <class V>
    JsonError(std::string k, V&& v)
        : key(std::move(k)), value(boost::json::value_from(std::forward<V>(v))) {}
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

/* ---------- handler argument deduction ---------- */

// bindJson / subscribeJson can take their parameter types from the handler itself
// (see BindJsonInvoker below), so the common case needs no explicit <Args...>:
//
//   window->bindJson("add", [](AddReq req) -> std::execution::task<int> { ... });
//
// The machinery: type_list carries a parameter pack; fn_signature / operator_signature
// / callable_args extract the parameters of a lambda, functor, free function or member
// function pointer; bind_args then picks the explicit <Args...> when the caller wrote
// any, and falls back to the handler's own (decayed) parameters otherwise.

// A compile-time list of types, used to unpack a deduced parameter list into Args...
template <class... Ts>
struct type_list {
};

// The parameter / return types of the function type M. valid=false for anything that
// is not a function (pointer / reference) or a member function pointer.
template <class M>
struct fn_signature {
    static constexpr bool valid = false;
    using result = void;
    using args = type_list<>;
};

template <class R, class... A>
struct fn_signature<R(A...)> {
    static constexpr bool valid = true;
    using result = R;
    using args = type_list<A...>;
};

template <class R, class... A>
struct fn_signature<R (*)(A...)> : fn_signature<R(A...)> {
};

template <class R, class... A>
struct fn_signature<R (&)(A...)> : fn_signature<R(A...)> {
};

// Member function pointers: one specialization per cv / ref / noexcept combination a
// lambda's operator() (or a bound member function) can have. (Lambdas themselves can be
// cv-qualified but never ref-qualified; the ref-qualified forms cover hand-written
// functors and ref-qualified member functions.) MSVC cannot even parse a member pointer
// type that combines noexcept with a ref-qualifier, so those two combinations are only
// listed for other compilers; on MSVC they fall under the "no single signature" rule
// and have to spell out their types.
#define HELIOSVIEW_FN_SIGNATURE_MEMBER(Q)                                                  \
    template <class C, class R, class... A>                                                \
    struct fn_signature<R (C::*)(A...) Q> : fn_signature<R(A...)> {                        \
    };
HELIOSVIEW_FN_SIGNATURE_MEMBER()
HELIOSVIEW_FN_SIGNATURE_MEMBER(const)
HELIOSVIEW_FN_SIGNATURE_MEMBER(volatile)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const volatile)
HELIOSVIEW_FN_SIGNATURE_MEMBER(noexcept)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const noexcept)
HELIOSVIEW_FN_SIGNATURE_MEMBER(volatile noexcept)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const volatile noexcept)
HELIOSVIEW_FN_SIGNATURE_MEMBER(&)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const &)
HELIOSVIEW_FN_SIGNATURE_MEMBER(&&)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const &&)
#if !defined(_MSC_VER)
HELIOSVIEW_FN_SIGNATURE_MEMBER(noexcept &)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const noexcept &)
HELIOSVIEW_FN_SIGNATURE_MEMBER(noexcept &&)
HELIOSVIEW_FN_SIGNATURE_MEMBER(const noexcept &&)
#endif
#undef HELIOSVIEW_FN_SIGNATURE_MEMBER

// The parameter list of a class with one non-template operator() (lambdas, functors).
// valid=false for a generic lambda (operator() is a template) or an overloaded one,
// because &Fn::operator() then has no single type.
template <class Fn, class = void>
struct operator_signature {
    static constexpr bool valid = false;
    using args = type_list<>;
};

template <class Fn>
struct operator_signature<Fn, std::void_t<decltype(&Fn::operator())>> {
    static constexpr bool valid = fn_signature<decltype(&Fn::operator())>::valid;
    using args = typename fn_signature<decltype(&Fn::operator())>::args;
};

// What a handler can be bound with: a free function / function pointer, a lambda or a
// functor (its own operator()), or a member function pointer.
template <class Fn>
struct callable_args {
    using direct = fn_signature<Fn>;        // free function / function pointer handlers
    using member = operator_signature<Fn>;  // lambdas / functors
    static constexpr bool valid = direct::valid || member::valid;
    using args = std::conditional_t<direct::valid, typename direct::args, typename member::args>;
};

// Strip references / cv from every parameter (a handler declared (const Req&) is bound
// as Req and deserialized with value_to<Req>; the handler still receives it fine).
template <class List>
struct decay_args;

template <class... A>
struct decay_args<type_list<A...>> {
    using type = type_list<std::decay_t<A>...>;
};

// True when a parameter list holds a reference (see bind_args).
template <class List>
struct any_reference;

template <class... A>
struct any_reference<type_list<A...>> {
    static constexpr bool value = (std::is_reference_v<A> || ...);
};

// The effective parameter list of a binding: the explicit <Args...> when the caller
// wrote bindJson<A, B>(name, handler), otherwise the handler's own parameter types.
template <class Fn, class... Explicit>
struct bind_args {
    static constexpr bool explicit_args = sizeof...(Explicit) > 0;
    static constexpr bool deducible = callable_args<Fn>::valid;

    static_assert(explicit_args || deducible,
                  "bindJson: cannot deduce the handler's parameter types because it has no "
                  "single fixed signature (a generic lambda, an overloaded or templated "
                  "operator(), std::function, ...). Pass them explicitly, e.g. "
                  "bindJson<Req>(name, handler).");

    using deduced = typename callable_args<Fn>::args;

    // A by-reference parameter would dangle: the handler runs as a lazy
    // std::execution::task (its body does not start until the sender is started), so the
    // argument deserialized from the JS call is long destroyed by then. Pass by value.
    // (Only the deduced list is checked: an explicit bindJson<Req> keeps its historical
    // behavior, where value_to<Req> would already have been the deserialized type.)
    static_assert(explicit_args || !any_reference<deduced>::value,
                  "bindJson: a deduced handler parameter must be taken by value. The handler "
                  "runs as a lazy std::execution::task, so the deserialized argument is "
                  "already gone by the time its body runs and a reference parameter would "
                  "dangle - write [](Req req) instead of [](const Req& req).");

    using type = std::conditional_t<explicit_args, type_list<Explicit...>,
                                    typename decay_args<deduced>::type>;
};

// The single parameter of a broadcast callback (subscribeJson): the list must hold
// exactly one type, since the C layer hands the callback one deserialized value.
template <class List>
struct first_arg {
    static constexpr bool valid = false;
    using type = void;
};

template <class A, class... Rest>
struct first_arg<type_list<A, Rest...>> {
    static constexpr bool valid = sizeof...(Rest) == 0;
    using type = std::decay_t<A>;
};

// The Req of a subscribeJson binding: the explicit <Req>, or the callback's own
// (decayed) parameter type.
template <class Fn, class Explicit>
struct subscribe_arg {
    static constexpr bool explicit_arg = !std::is_void_v<Explicit>;

    static_assert(explicit_arg || callable_args<Fn>::valid,
                  "subscribeJson: cannot deduce the callback's parameter type because it "
                  "has no single fixed signature (a generic lambda, an overloaded or "
                  "templated operator(), std::function, ...). Pass it explicitly, e.g. "
                  "subscribeJson<Req>(name, callback).");

    using deduced = first_arg<typename callable_args<Fn>::args>;

    static_assert(explicit_arg || deduced::valid,
                  "subscribeJson: a callback bound without an explicit <Req> must take "
                  "exactly one parameter (the value deserialized from the broadcast), "
                  "e.g. [](StatusReq req) { ... }.");

    using type = std::conditional_t<explicit_arg, Explicit, typename deduced::type>;
};

/* ---------- response serialization ---------- */

// boost::json::value values serialize directly
inline std::string jsonDump(const boost::json::value& j)
{
    return boost::json::serialize(j);
}

// Serialize any value through boost::json. A std::string / const char* result that
// is itself JSON text (e.g. built by a boost::json::serialize()) is parsed once and
// re-dumped so it stays a single JSON value instead of being double-encoded.
template <class T>
std::string jsonDumpValue(const T& value)
{
    boost::json::value j = boost::json::value_from(value);
    if constexpr (std::is_same_v<std::decay_t<T>, std::string>
                  || std::is_same_v<std::decay_t<T>, const char*>) {
        try {
            j = boost::json::parse(value);
        } catch (...) { /* not JSON text: keep the plain string value */ }
    }
    return boost::json::serialize(j);
}

// Generic response serialization
template <class Resp>
std::string jsonDump(const Resp& resp)
{
    return jsonDumpValue(resp);
}

/* ---------- error payload ---------- */

inline boost::json::value errorToJson(std::exception_ptr eptr)
{
    boost::json::object obj;
    if (!eptr) {
        obj["error"] = "unknown error";
        return obj;
    }
    try {
        std::rethrow_exception(eptr);
    } catch (const boost::system::system_error& e) {
        obj["error"] = e.what();
    } catch (const std::exception& e) {
        obj["error"] = e.what();
    } catch (...) {
        obj["error"] = "unknown error";
    }
    return obj;
}

/* ---------- completion delivery ---------- */

// Resolve / reject the pending JS Promise from the handler's completion value.
// JsonError is the only shape that rejects; everything else resolves.
template <class Resp>
void replyJson(heliosview_webview_t* wv, uint64_t call_id, Resp&& resp)
{
    heliosview_webview_resolve(wv, call_id, jsonDump(resp).c_str());
}

inline void replyJson(heliosview_webview_t* wv, uint64_t call_id, JsonError&& err)
{
    boost::json::object obj;
    obj[err.key] = err.value;
    heliosview_webview_reject(wv, call_id, boost::json::serialize(obj).c_str());
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
                                      boost::json::serialize(errorToJson(std::move(eptr))).c_str());
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
// handler as a detached task. Supports zero, one, or many parameters. The handler is
// invoked through std::invoke so a lambda / functor and a plain function (pointer or
// reference) both work.
template <class Sender, class Fn, class... Args>
struct JsonHandler {
    static void invoke(heliosview_webview_t* wv, uint64_t call_id,
                       const char* name, const char* args_json, void* userdata)
    {
        (void)name;
        auto* fn = &static_cast<pmr_box<Fn>*>(userdata)->value;
        try {
            boost::json::value args = (args_json && *args_json)
                                          ? boost::json::parse(args_json)
                                          : boost::json::value(boost::json::array_kind);

            auto make = [&]<std::size_t... I>(std::index_sequence<I...>) {
                return std::invoke(*fn, boost::json::value_to<Args>(args.at(I))...);
            };
            auto coro = make(std::index_sequence_for<Args...>{});

            auto* state = new JsonCallState<Sender>(std::move(coro), wv, call_id);
            std::execution::start(state->op); // state may be deleted inline on sync completion
        } catch (const std::exception& e) {
            boost::json::object obj;
            obj["error"] = e.what();
            heliosview_webview_reject(wv, call_id, boost::json::serialize(obj).c_str());
        } catch (...) {
            heliosview_webview_reject(wv, call_id, R"({"error":"unknown error"})");
        }
    }
};

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
            boost::json::value data = (data_json && *data_json)
                                          ? boost::json::parse(data_json)
                                          : boost::json::value{};
            Req req = boost::json::value_to<Req>(data);
            (*fn)(std::move(req));
        } catch (...) { /* unparseable broadcast: dropped (void callback has no error channel) */ }
    }
};

/* ---------- the typed binding cores (Args... / Req are always fully spelled out) ---------- */

// Shared by bindJson and its member-function overload. Args... are the parameter types
// deserialized from the JS call's arguments array (args[0] -> Args[0], ...), each any
// type boost::json::value_to can convert (fundamentals, strings, containers,
// BOOST_DESCRIBE_STRUCT-annotated DTOs, boost::json::value itself, ...). The handler's
// signature is (Args...) -> std::execution::task<Resp>. The handler may capture anything
// it needs (e.g. the window or an Async).
template <class... Args, class Fn>
void bindJsonTyped(heliosview_webview_t* wv, const char* name, Fn&& handler)
{
    static_assert(std::invocable<Fn&, Args...>,
                  "bindJson handler must be callable with (Args...). Deduced arguments are "
                  "deserialized as values, so a handler cannot take one of them by "
                  "non-const lvalue reference.");
    using Sender = std::decay_t<decltype(handler(std::declval<Args>()...))>;
    static_assert(std::execution::sender<Sender>,
                  "bindJson handler must return a sender (e.g. std::execution::task<Resp>)");

    // The userdata is a pmr_box (resource + handler copy) allocated through the
    // default PMR resource (lifetime = the binding); the C layer destroys it via the
    // userdata_dtor when the binding is replaced or the webview dies. The C layer
    // rejects invalid names (anything that is not a C identifier, e.g. with a dot);
    // on failure the box is released here and std::invalid_argument is thrown, so a
    // bad name fails loudly at setup instead of silently never being callable.
    auto* box = pmrAllocate<Fn>(std::forward<Fn>(handler));
    const int rc = heliosview_webview_bind(wv, name,
                                           &JsonHandler<Sender, Fn, Args...>::invoke,
                                           box,
                                           [](void* userdata) { pmrRelease(static_cast<pmr_box<Fn>*>(userdata)); });
    if (rc != 0) {
        pmrRelease(box); /* the C layer never took ownership on failure */
        throwLastError<std::invalid_argument>("bindJson");
    }
}

// Shared by subscribeJson and its member-function overload. Req is the DTO to
// deserialize the page's BroadcastChannel(name) postMessage value into (any type
// boost::json::value_to can convert). The callback's signature is (Req) -> void and it
// runs on the UI thread for every JS postMessage on that channel. A value that fails to
// parse into Req is dropped (there is no promise to reject on a broadcast).
template <class Req, class Fn>
void subscribeJsonTyped(heliosview_webview_t* wv, const char* name, Fn&& callback)
{
    static_assert(std::invocable<Fn&, Req>,
                  "subscribeJson callback must be callable with (Req)");

    // The userdata is a pmr_box (resource + callback copy) allocated through the
    // default PMR resource (lifetime = the subscription); the C layer destroys it via
    // the userdata_dtor when replaced or the webview dies. Like bindJson, an invalid
    // name (not a C identifier, e.g. with a dot) releases the box and throws
    // std::invalid_argument at setup.
    auto* box = pmrAllocate<Fn>(std::forward<Fn>(callback));
    const int rc = heliosview_webview_subscribe(wv, name,
                                                &SubscribeHandler<Req, Fn>::invoke,
                                                box,
                                                [](void* userdata) { pmrRelease(static_cast<pmr_box<Fn>*>(userdata)); });
    if (rc != 0) {
        pmrRelease(box); /* the C layer never took ownership on failure */
        throwLastError<std::invalid_argument>("subscribeJson");
    }
}

/* ---------- explicit / deduced dispatch ---------- */

// Unpack a type_list into the explicit template arguments of the typed cores above.
template <class List, class Fn>
struct BindJsonInvoker;

template <class... Args, class Fn>
struct BindJsonInvoker<type_list<Args...>, Fn> {
    static void apply(heliosview_webview_t* wv, const char* name, Fn&& handler)
    {
        bindJsonTyped<Args...>(wv, name, std::forward<Fn>(handler));
    }
};

template <class List, class Fn>
struct SubscribeJsonInvoker;

template <class Req, class Fn>
struct SubscribeJsonInvoker<type_list<Req>, Fn> {
    static void apply(heliosview_webview_t* wv, const char* name, Fn&& callback)
    {
        subscribeJsonTyped<Req>(wv, name, std::forward<Fn>(callback));
    }
};

// The member-function overloads wrap (obj, method) into a lambda once the parameter
// list is known, then forward to the callback forms (which then have explicit types,
// so no further deduction happens). The object must outlive the binding.
template <class List>
struct MemberBindJson;

template <class... Args>
struct MemberBindJson<type_list<Args...>> {
    template <class Win, class Obj, class MFPtr>
    static void apply(Win& win, const char* name, Obj* obj, MFPtr method)
    {
        win.template bindJson<Args...>(name, [obj, method](Args... args) {
            return std::invoke(method, obj, std::move(args)...);
        });
    }
};

template <class List>
struct MemberSubscribeJson;

template <class Req>
struct MemberSubscribeJson<type_list<Req>> {
    template <class Win, class Obj, class MFPtr>
    static void apply(Win& win, const char* name, Obj* obj, MFPtr method)
    {
        win.template subscribeJson<Req>(name, [obj, method](Req req) {
            std::invoke(method, obj, std::move(req));
        });
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// WebViewWindow::bindJson implementation (declared in WebViewWindow.h).
//
// Args... is optional: when the caller spells it out (bindJson<AddReq>("add", ...))
// those types are used verbatim; when it is omitted the handler's own parameter types
// are deduced (decayed to values), so the common case is just
//
//   window->bindJson("add", [](AddReq req) -> std::execution::task<int> { ... });
//
// Deduction needs a single, non-template signature: a lambda, a functor with one
// operator(), a free function, or (for the member overloads) a member function pointer.
// A generic lambda / std::function has no single signature and must be given the
// explicit <Args...>.
// ---------------------------------------------------------------------------
template <class... Args, class Fn>
void WebViewWindow::bindJson(const char* name, Fn&& handler)
{
    using List = typename detail::bind_args<std::decay_t<Fn>, Args...>::type;
    detail::BindJsonInvoker<List, Fn>::apply(m_webview, name, std::forward<Fn>(handler));
}

// Member-function overload of bindJson: bind a member function of `obj` whose signature
// is `Sender (Obj::*)(Args...)`. The parameter types are deduced from the member pointer
// unless the caller spells them out.
template <class... Args, class Obj, class MFPtr>
void WebViewWindow::bindJson(const char* name, Obj* obj, MFPtr method)
{
    static_assert(std::is_member_function_pointer_v<MFPtr>,
                  "bindJson member overload expects a member function pointer");
    using List = typename detail::bind_args<MFPtr, Args...>::type;
    detail::MemberBindJson<List>::apply(*this, name, obj, method);
}

// ---------------------------------------------------------------------------
// WebViewWindow::subscribeJson implementation (declared in WebViewWindow.h).
//
// Req is optional in the same way: subscribeJson<MsgReq>("status", cb) uses that type,
// subscribeJson("status", [](MsgReq req) { ... }) deduces it from the callback, which
// must take exactly one parameter (the deserialized broadcast value).
// ---------------------------------------------------------------------------
template <class Req, class Fn>
void WebViewWindow::subscribeJson(const char* name, Fn&& callback)
{
    using R = typename detail::subscribe_arg<std::decay_t<Fn>, Req>::type;
    detail::SubscribeJsonInvoker<detail::type_list<R>, Fn>::apply(m_webview, name,
                                                                  std::forward<Fn>(callback));
}

// Member-function overload of subscribeJson: subscribe a member function of `obj` with
// signature `void (Obj::*)(Req)`. Req is deduced from the member pointer unless the
// caller spells it out.
template <class Req, class Obj, class MFPtr>
void WebViewWindow::subscribeJson(const char* name, Obj* obj, MFPtr method)
{
    static_assert(std::is_member_function_pointer_v<MFPtr>,
                  "subscribeJson member overload expects a member function pointer");
    using R = typename detail::subscribe_arg<MFPtr, Req>::type;
    detail::MemberSubscribeJson<detail::type_list<R>>::apply(*this, name, obj, method);
}

} // namespace helios
