// HeliosView.Core example: WebView window (WebView2 under the hood on win32).
// Demonstrates the JS <-> native bridge with nlohmann auto-binding:
//   - bindJson<Req>(): native functions callable from JS via window.helios.call(...) -> Promise
//     the JS call's first argument is deserialized into a Req DTO (nlohmann), and the
//     handler's task<Resp> result is serialized back automatically (resolve / reject)
//   - each native function prints its arguments and return value with std::println (C++23)
//   - a log panel on the page shows the same round-trip
//   - broadcast() pushes a native -> JS message via BroadcastChannel
//   - subscribeJson<Req>(): the page's BroadcastChannel postMessage -> native (JS -> native)
//   - bindJson / subscribeJson also accept a member function: bindJson<Req>(name, obj, &Class::method)
//   - eval() / evalAsync(): run JS from native (output on the terminal)
// The bridge shim is injected into every page automatically.
#include <HeliosViewCore/HeliosView.h>

#include <format>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

// Request DTOs: the JS call's first argument is deserialized into these (nlohmann)
struct AddReq { int a; int b; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AddReq, a, b)

struct GreetReq { std::string name; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GreetReq, name)

struct MsgReq { std::string from; int n; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MsgReq, from, n)

struct RepeatReq { std::string s; int times; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RepeatReq, s, times)

// A class whose member functions are bound to the JS bridge (member-function overload).
struct Service {
    // task<Resp> (Service::*)(RepeatReq)
    std::execution::task<std::string> repeat(RepeatReq req)
    {
        std::string out;
        for (int i = 0; i < req.times; ++i)
            out += req.s;
        std::println("[member] repeat(\"{}\", {}) -> {} chars", req.s, req.times, out.size());
        co_return out;
    }

    // void (Service::*)(MsgReq), used by subscribeJson
    void onStatus(MsgReq req)
    {
        std::println("[member] received JS broadcast on 'status': from={} n={}", req.from, req.n);
    }
};

int main()
{
    std::println("HeliosView {}", helios::version());

    auto app = std::make_shared<helios::App>();

    // Background thread pool (asio-backed, see HeliosViewCore/Async.h): handlers
    // hop off the UI thread with `co_await schedule(async.get_scheduler())`.
    // Must outlive the bindings below.
    helios::Async async;

    // Frameless: a fully frameless window (no system title bar). The page's
    // title bar is the injected <helios-window-title-bar> web component - it
    // auto-registers as the drag region (built-in __hv.drag) and hosts
    // <helios-window-controls> for the buttons (built-in __hv.control / __hv.state).
    auto window = std::make_shared<helios::WebViewWindow>(
        900, 640, "HeliosView WebView Demo", helios::WindowStyle::Frameless);
    window->show();
    window->createWebView();

    /* ---- auto-bound native functions (nlohmann deserializes arguments, serializes return values) ---- */

    // add({a, b}) -> a + b
    window->bindJson<AddReq>("add", [](AddReq req) -> std::execution::task<int> {
        const int result = req.a + req.b;
        std::println("[native] add({}, {}) -> {}", req.a, req.b, result);
        co_return result;
    });

    // echo(obj) -> returns the whole argument object back (round-trips any JSON)
    window->bindJson<nlohmann::json>("echo", [](nlohmann::json req) -> std::execution::task<nlohmann::json> {
        std::println("[native] echo({}) -> {}", req.dump(), req.dump());
        co_return req;
    });

    // greet({name}) -> {"msg":"hello, name"}
    window->bindJson<GreetReq>("greet", [](GreetReq req) -> std::execution::task<helios::JsonResp<std::string>> {
        const std::string msg = std::format("hello, {}", req.name);
        std::println("[native] greet(\"{}\") -> {{\"msg\":\"{}\"}}", req.name, msg);
        co_return helios::JsonResp<std::string>{"msg", msg};
    });

    // fail() -> the Promise rejects with {"error":"nope"}
    window->bindJson<nlohmann::json>("fail", [](nlohmann::json) -> std::execution::task<helios::JsonError<std::string>> {
        std::println("[native] fail() -> reject");
        co_return helios::JsonError<std::string>{"error", "nope"};
    });

    // spin({a,b}) -> hops off the UI thread onto the asio worker pool, does some
    // busy work there, and resolves from the pool thread (resolve/reject is
    // thread-safe in the C bridge, so no marshalling back is needed).
    window->bindJson<AddReq>("spin", [&async](AddReq req) -> std::execution::task<helios::JsonResp<std::string>> {
        co_await std::execution::schedule(async.get_scheduler()); // UI thread -> pool worker
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // simulated work
        const std::string msg = std::format("{} + {} = {} on worker thread {}",
                                            req.a, req.b, req.a + req.b, std::this_thread::get_id());
        std::println("[native] spin: computed on pool thread {}", std::this_thread::get_id());
        co_return helios::JsonResp<std::string>{"msg", msg};
    });

    // emit() -> resolves {"ok":true}, then pushes a native broadcast to the "status" channel
    // (raw pointer capture is safe: the binding lives exactly as long as the window)
    window->bindJson<nlohmann::json>("emit", [win = window.get()](nlohmann::json) -> std::execution::task<helios::JsonResp<bool>> {
        std::println("[native] emit() -> {{ok:true}}, broadcast 'status'");
        win->broadcast("status", R"({"from":"native","n":1})");
        co_return helios::JsonResp<bool>{"ok", true};
    });

    // Subscribe to the page's BroadcastChannel("status").postMessage (JS -> native).
    // The page's JS can send a broadcast on the same channel the native code pushes to;
    // the value is deserialized into MsgReq and delivered on the UI thread.
    window->subscribeJson<MsgReq>("status", [](MsgReq req) {
        std::println("[native] received JS broadcast on 'status': from={} n={}", req.from, req.n);
    });

    /* ---- member-function overloads: bind a Service member instead of a lambda ---- */
    auto service = std::make_shared<Service>();
    window->bindJson<RepeatReq>("repeat", service.get(), &Service::repeat);
    window->subscribeJson<MsgReq>("status", service.get(), &Service::onStatus);

    /* ---- a page that uses the bridge ---- */

    window->navigateHtml(
        "<html><head><meta charset='utf-8'></head>"
        "<body style='font-family:system-ui;background:#1e1e2e;color:#cdd6f4;margin:0;"
        "height:100%;display:flex;flex-direction:column;overflow:hidden'>"
        // The title bar is the injected <helios-window-title-bar> web component:
        // it drags the window through WebView2's native app-region:drag (the
        // library enables IsNonClientRegionSupportEnabled - verified that this
        // is the active drag mechanism, not WM_NCHITTEST, which a full-bleed
        // WebView swallows). <helios-window-controls> inside it renders the
        // min/max/close buttons.
        "<helios-window-title-bar style='padding:0 16px;padding-right:150px;"
        "box-sizing:border-box;background:#24243a;border-bottom:1px solid #3a3a55;"
        "color:#cdd6f4;white-space:nowrap;overflow:hidden'>"
        "<span style='font-weight:600;font-size:14px'>HeliosView WebView Bridge Demo (nlohmann auto-binding)</span>"
        "<helios-window-controls></helios-window-controls>"
        "</helios-window-title-bar>"
        "<div style='padding:12px;flex:0 0 auto'>"
        "<div style='display:flex;gap:8px;flex-wrap:wrap'>"
        "<button onclick=\"run('add', {a: 40, b: 2})\">add({a:40, b:2})</button>"
        "<button onclick=\"run('echo', {x:1, y:'hi'})\">echo({x:1,y:'hi'})</button>"
        "<button onclick=\"run('greet', {name:'helios'})\">greet({name:'helios'})</button>"
        "<button onclick=\"run('fail', {})\">fail()</button>"
        "<button onclick=\"run('spin', {a: 20, b: 22})\">spin({a:20,b:22}) -&gt; Async pool</button>"
        "<button onclick=\"run('emit', {})\">emit() -&gt; broadcast</button>"
        "<button onclick=\"run('repeat', {s: 'ab', times: 3})\">repeat({s:'ab',times:3})</button>"
        "<button onclick=\"bcSend()\">bc.postMessage -&gt; native</button>"
        "</div>"
        "</div>"
        "<div id='log' style='flex:1;overflow:auto;padding:0 12px 12px;"
        "font-family:ui-monospace,Consolas,monospace;font-size:13px;line-height:1.5'>"
        "<div style='opacity:.6'>Click the buttons above and watch the arguments and return values</div></div>"
        "<script>"
        "  const log = document.getElementById('log');"
        "  function line(txt, cls) {"
        "    const d = document.createElement('div');"
        "    if (cls) d.className = cls;"
        "    d.textContent = txt;"
        "    log.appendChild(d);"
        "    log.scrollTop = log.scrollHeight;"
        "  }"
        "  async function run(name, ...args) {"
        "    line('call: ' + name + '(' + args.map(a => JSON.stringify(a)).join(', ') + ')');"
        "    try {"
        "      const r = await window.helios.call(name, ...args);"
        "      line('return: ' + JSON.stringify(r), 'ok');"
        "    } catch (e) {"
        "      line('error: ' + e.message, 'err');"
        "    }"
        "  }"
        "  const bc = new BroadcastChannel('status');"
        "  bc.onmessage = e => line('[bc] ' + JSON.stringify(e.data), 'bc');"
        "  function bcSend() {"
        "    bc.postMessage({from: 'js', n: Math.floor(Math.random() * 100)});"
        "    line('[bc] posted to native', 'bc');"
        "  }"
        "</script>"
        "</body></html>");

    /* ---- run JS / broadcast (queued automatically until the WebView is ready) ---- */

    window->eval("console.log('hi from native eval');");
    window->evalAsync("1 + 1", [](int error, const char* result, void*) {
        std::println("[evalAsync] error={} result={}", error, result ? result : "(null)");
    });

    std::println("[main] entering UI loop (Esc to close)...");
    return app->exec();
}
