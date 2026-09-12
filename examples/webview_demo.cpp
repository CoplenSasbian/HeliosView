// HeliosView.Core example: WebView window (WebView2 under the hood on win32).
// The JS <-> native bridge with Boost.JSON auto-binding, in isolation:
//   - bindJson(): native functions callable from JS via window.helios.call(...) -> Promise.
//     The handler's parameter type is deduced from its own signature and deserialized
//     from the JS argument (Boost.JSON + Boost.Describe DTOs); the handler's
//     task<Resp> result is serialized back automatically (resolve / reject).
//   - each native function prints its arguments and return value with std::println (C++23)
//   - a log panel on the page shows the same round-trip
//   - broadcast() pushes a native -> JS message via BroadcastChannel
//   - subscribeJson(): the page's BroadcastChannel postMessage -> native (JS -> native)
//   - bindJson / subscribeJson also accept a member function: bindJson(name, obj, &Class::method)
//   - eval() / evalAsync(): run JS from native (output on the terminal)
//   - handlers may hop off the UI thread with co_await schedule(async.get_scheduler())
//     (the `spin` handler) and call HTTP through helios::http::Client (the `fetch` handler)
//   - the right-click has one switch (setContextMenuEnabled: may the engine's own
//     menu open?) and one native interception gate (contextMenuGate); the page can
//     intercept too, and what to show is the application's decision
// The bridge shim is injected into every page automatically.
//
// Try it: click the buttons on the page and watch both the page log and this console.
#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/Http.h>

#include <format>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <boost/describe.hpp>
#include <boost/json.hpp>

// Request DTOs: the JS call's first argument is deserialized into these (Boost.JSON via Boost.Describe)
struct AddReq { int a; int b; };
BOOST_DESCRIBE_STRUCT(AddReq, (), (a, b))

struct GreetReq { std::string name; };
BOOST_DESCRIBE_STRUCT(GreetReq, (), (name))

struct MsgReq { std::string from; int n; };
BOOST_DESCRIBE_STRUCT(MsgReq, (), (from, n))

struct RepeatReq { std::string s; int times; };
BOOST_DESCRIBE_STRUCT(RepeatReq, (), (s, times))

struct FetchReq { std::string url; };
BOOST_DESCRIBE_STRUCT(FetchReq, (), (url))

struct MenuModeReq { std::string mode; };  // "engine" | "page" | "native"
BOOST_DESCRIBE_STRUCT(MenuModeReq, (), (mode))

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

    // HTTP client on the pool (see HeliosViewCore/Http.h): it keeps a pool of
    // keep-alive connections per origin and reuses them across requests. Must not
    // outlive `async`. cacert.pem (the CA bundle) sits next to the exe (copied at
    // configure time) so https certificates verify.
    helios::http::Client client{async, std::chrono::seconds(10), "cacert.pem"};

    // Frameless: a fully frameless window (no system title bar). The page's
    // title bar is the injected <helios-window-title-bar> web component - it
    // auto-registers as the drag region (built-in __hv.drag) and hosts
    // <helios-window-controls> for the caption buttons (built-in __hv.control /
    // __hv.state — the page draws them, so the state glyph stays in sync).
    //
    // Alternative: let WebView2 draw the caption buttons instead (Window
    // Controls Overlay). Opt in with setWindowControlsOverlay(true) + a
    // matching setWindowControlsBackgroundColor, and REMOVE
    // <helios-window-controls> from the page. Note: WebView2's overlay only
    // updates its maximize/restore glyph for state changes it initiates itself
    // (experimental API limitation) — externally-driven maximizes (Win+Up,
    // taskbar, snap) leave the glyph stale, which is why the page-drawn
    // buttons are the demo default.
    auto window = std::make_shared<helios::WebViewWindow>(
        900, 640, "HeliosView WebView Demo", helios::WindowStyle::Frameless);
    window->show();
    window->createWebView();
    // window->setWindowControlsOverlay(true); /* WebView2 draws the caption buttons (see comment above) */
    // window->setWindowControlsBackgroundColor(0x24, 0x24, 0x3A, 255);

    /* ---- auto-bound native functions (Boost.JSON deserializes arguments, serializes return values) ---- */

    // add({a, b}) -> a + b. The argument types are deduced from the handler, so no
    // explicit bindJson<AddReq> is needed (spelling them out still works, see `echo`).
    window->bindJson("add", [](AddReq req) -> std::execution::task<int> {
        const int result = req.a + req.b;
        std::println("[native] add({}, {}) -> {}", req.a, req.b, result);
        co_return result;
    });

    // echo(obj) -> returns the whole argument object back (round-trips any JSON).
    // Explicit form (identical here, shown for reference): bindJson<boost::json::value>.
    window->bindJson<boost::json::value>("echo", [](boost::json::value req) -> std::execution::task<boost::json::value> {
        std::println("[native] echo({}) -> {}", boost::json::serialize(req), boost::json::serialize(req));
        co_return req;
    });

    // greet({name}) -> {"msg":"hello, name"}
    window->bindJson("greet", [](GreetReq req) -> std::execution::task<boost::json::value> {
        const std::string msg = std::format("hello, {}", req.name);
        std::println("[native] greet(\"{}\") -> {{\"msg\":\"{}\"}}", req.name, msg);
        co_return boost::json::value{{"msg", msg}};
    });

    // fail() -> the Promise rejects with {"error":"nope"}
    window->bindJson("fail", [](boost::json::value) -> std::execution::task<helios::JsonError> {
        std::println("[native] fail() -> reject");
        co_return helios::JsonError{"error", "nope"};
    });

    // spin({a,b}) -> hops off the UI thread onto the asio worker pool, does some
    // busy work there, and resolves from the pool thread (resolve/reject is
    // thread-safe in the C bridge, so no marshalling back is needed).
    window->bindJson("spin", [&async](AddReq req) -> std::execution::task<boost::json::value> {
        co_await std::execution::schedule(async.get_scheduler()); // UI thread -> pool worker
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // simulated work
        const std::string msg = std::format("{} + {} = {} on worker thread {}",
                                            req.a, req.b, req.a + req.b, std::this_thread::get_id());
        std::println("[native] spin: computed on pool thread {}", std::this_thread::get_id());
        co_return boost::json::value{{"msg", msg}};
    });

    // HTTP client on the pool (see HeliosViewCore/Http.h): the response
    // (status/headers/body) is serialized back to JS, and the client reuses
    // keep-alive connections per origin. Network or timeout errors reject the
    // Promise with {"error": ...}.
    window->bindJson("fetch", [&client](FetchReq req) -> std::execution::task<boost::json::value> {
        const std::string url = req.url.empty() ? std::string("http://example.com/") : req.url;
        std::println("[native] fetch: GET {}", url);
        auto resp = co_await client.get(url);
        boost::json::object out;
        boost::json::array headers;
        for (const auto& [k, v] : resp.headers) {
            boost::json::object h;
            h[k] = v;
            headers.push_back(std::move(h));
        }
        out["status"] = resp.status;
        out["reason"] = resp.reason;
        out["headers"] = std::move(headers);
        out["body"] = resp.body;
        std::println("[native] fetch: -> {} {} ({} body bytes)", resp.status, resp.reason,
                     resp.body.size());
        co_return boost::json::value{{"resp", std::move(out)}};
    });

    // emit() -> resolves {"ok":true}, then pushes a native broadcast to the "status" channel
    // (raw pointer capture is safe: the binding lives exactly as long as the window)
    window->bindJson("emit", [win = window.get()](boost::json::value) -> std::execution::task<boost::json::value> {
        std::println("[native] emit() -> {{ok:true}}, broadcast 'status'");
        win->broadcast("status", R"({"from":"native","n":1})");
        co_return boost::json::value{{"ok", true}};
    });

    // Subscribe to the page's BroadcastChannel("status").postMessage (JS -> native).
    // The page's JS can send a broadcast on the same channel the native code pushes to;
    // the value is deserialized into MsgReq (deduced from the callback) and delivered on
    // the UI thread.
    window->subscribeJson("status", [](MsgReq req) {
        std::println("[native] received JS broadcast on 'status': from={} n={}", req.from, req.n);
    });

    /* ---- member-function overloads: bind a Service member instead of a lambda ---- */
    auto service = std::make_shared<Service>();
    // The parameter types come from the member function's signature (deduced);
    // bindJson<RepeatReq>(...) / subscribeJson<MsgReq>(...) also still work.
    window->bindJson("repeat", service.get(), &Service::repeat);
    window->subscribeJson("status", service.get(), &Service::onStatus);

    /* ---- right-click: one switch + one interception gate ---- */

    // The menu the gate shows; built once and reused (a Menu can be shown as often
    // as needed). Its items read the last request's link, so they can act on
    // whatever was under the cursor.
    auto contextMenu = std::make_shared<helios::Menu>();
    auto lastLink = std::make_shared<std::string>();
    contextMenu->addItem("Copy selection")->triggered.connect([window] {
        std::println("[native] context menu: copy selection");
        window->eval("document.execCommand('copy')");
    });
    contextMenu->addItem("Open link in the browser")->triggered.connect([lastLink] {
        std::println("[native] context menu: open '{}'", *lastLink);
        if (!lastLink->empty())
            helios::openUrl(*lastLink);
    });
    contextMenu->addSeparator();
    contextMenu->addItem("Log page URL")->triggered.connect([window] {
        window->eval("console.log('page URL:', location.href)");
        std::println("[native] context menu: logged the page URL");
    });

    // Consulted for every right click; true intercepts it (the engine's menu stays
    // closed). Everything else - what to show, and whether the engine's menu may
    // open at all (setContextMenuEnabled, switched by the page's buttons below) - is
    // the application's decision.
    // The demo's own policy, set from the page's buttons: "engine" lets the engine's
    // menu through, "page" draws it in the page (and suppresses the engine's), and
    // "native" intercepts every click except inside text fields.
    auto menuMode = std::make_shared<std::string>("engine");
    window->contextMenuGate = [window, contextMenu, lastLink, menuMode](const helios::ContextMenuInfo& info) {
        std::println("[native] right-click: target=0x{:X} [{}] at {},{} link='{}' selection='{}'",
                     helios::toUint(info.target),
                     info.has(helios::ContextMenuTarget::Link)       ? "link "
                     : info.has(helios::ContextMenuTarget::Editable) ? "editable"
                                                                     : "page",
                     info.x, info.y, info.linkUrl, info.selectionText);
        if (*menuMode != "native") {
            /* "page": the switch is off (the page draws its own menu, the engine
             * shows nothing) - do not intercept. "engine": let the engine's menu
             * open. Either way this gate stays out of the way. */
            return false;
        }
        if (info.has(helios::ContextMenuTarget::Editable)) {
            std::println("[native]   -> text field: not intercepted, the engine's menu opens");
            return false;
        }
        *lastLink = info.linkUrl;
        /* Intercept now, open our menu on the next UI turn: this callback runs inside
         * the engine's event dispatch and a popup menu runs a modal message loop. */
        if (auto* app = helios::App::instance())
            app->postTask([contextMenu, window] { contextMenu->show(window->nativeHandle()); });
        else
            contextMenu->show(window->nativeHandle());
        return true;
    };

    // The page picks the behaviour with window.helios.call('set_menu_mode', {mode}).
    // The library only needs the switch; what the gate does with a click is the
    // demo's own policy above.
    window->bindJson("set_menu_mode", [window, menuMode](MenuModeReq req) -> std::execution::task<boost::json::value> {
        *menuMode = req.mode;
        const int rc = window->setContextMenuEnabled(req.mode != "page");
        std::println("[native] right-click -> '{}': the engine's menu is {}", req.mode,
                     req.mode == "page" ? "suppressed" : "enabled");
        co_return boost::json::value{{"mode", req.mode}, {"rc", rc}};
    });

    /* ---- a page that uses the bridge ---- */

    window->navigateHtml(
        "<html><head><meta charset='utf-8'></head>"
        "<body style='font-family:system-ui;background:#1e1e2e;color:#cdd6f4;margin:0;"
        "height:100%;display:flex;flex-direction:column;overflow:hidden'>"
        // The title bar is the injected <helios-window-title-bar> web component:
        // it drags the window through WebView2's native app-region:drag (the
        // library enables IsNonClientRegionSupportEnabled - verified that this
        // is the active drag mechanism, not WM_NCHITTEST, which a full-bleed
        // WebView swallows). <helios-window-controls> renders the min/max/close
        // buttons in the page (it opts its own area out of the drag region with
        // app-region:no-drag, so clicking them does not start a drag). The
        // right padding keeps the title clear of the buttons.
        "<helios-window-title-bar style='padding:0 16px;padding-right:150px;"
        "box-sizing:border-box;background:#24243a;border-bottom:1px solid #3a3a55;"
        "color:#cdd6f4;white-space:nowrap;overflow:hidden;position:relative'>"
        "<span style='font-weight:600;font-size:14px'>HeliosView WebView Bridge Demo (Boost.JSON auto-binding)</span>"
        "<helios-window-controls/>"
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
        "<input id='url' value='https://example.com/' style='width:220px;background:#2a2a44;border:1px solid #3a3a55;"
        "color:#cdd6f4;border-radius:4px;padding:2px 6px;font-size:12px'>"
        "<button onclick=\"run('fetch', {url: document.getElementById('url').value})\">fetch() -&gt; HttpClient</button>"
        "<button onclick=\"bcSend()\">bc.postMessage -&gt; native</button>"
        "</div>"
        // Right-click context menu: pick who owns it, then right-click the box.
        "<div style='display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:8px'>"
        "<span style='opacity:.7'>Right-click menu:</span>"
        "<button onclick=\"setMenuMode('engine')\">engine's own</button>"
        "<button onclick=\"setMenuMode('page')\">drawn by the page</button>"
        "<button onclick=\"setMenuMode('native')\">native (C++ Menu)</button>"
        "<span id='ctxmode' style='opacity:.7'>engine</span>"
        "<div id='ctxbox' style='flex:1;min-width:180px;border:1px dashed #585b70;border-radius:6px;"
        "padding:4px 8px;font-size:12px'>right-click me (a <a href='https://example.com/from-menu' "
        "style='color:#89b4fa'>link</a>, <input value='text field' style='width:80px'>)</div>"
        "</div>"
        "</div>"
        "<div id='pagemenu' style='display:none;position:fixed;background:#313244;border:1px solid #585b70;"
        "border-radius:6px;padding:4px 0;font-size:12px;box-shadow:0 6px 18px #0008;min-width:150px'>"
        "<div style='padding:4px 12px;opacity:.7'>page-drawn menu</div>"
        "<div style='padding:4px 12px' onclick=\"pmenu('hello from the page menu')\">Hello</div>"
        "<div style='padding:4px 12px' onclick=\"pmenu('menu item 2')\">Another item</div>"
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
        // Right-click: switch what a right click does, and draw the page's own menu
        // when the page is the one handling it.
        "  let ctxmode = 'engine';"
        "  async function setMenuMode(m) {"
        "    ctxmode = m;"
        "    document.getElementById('ctxmode').textContent = m;"
        "    await run('set_menu_mode', {mode: m});"
        "  }"
        "  const pmenuBox = document.getElementById('pagemenu');"
        "  function pmenu(txt) { pmenuBox.style.display = 'none'; line('[page menu] ' + txt); }"
        "  addEventListener('contextmenu', e => {"
        "    if (ctxmode !== 'page') return;   // engine/native modes are handled outside the page"
        "    e.preventDefault();"
        "    pmenuBox.style.left = Math.min(e.clientX, innerWidth - 170) + 'px';"
        "    pmenuBox.style.top = e.clientY + 'px';"
        "    pmenuBox.style.display = 'block';"
        "    line('[page] contextmenu event -> drawing the page menu');"
        "  });"
        "  addEventListener('click', () => pmenuBox.style.display = 'none');"
        "</script>"
        "</body></html>");

    /* ---- run JS / broadcast (queued automatically until the WebView is ready) ---- */

    window->eval("console.log('hi from native eval');");
    window->evalAsync("1 + 1", [](int error, const char* result, void*) {
        std::println("[evalAsync] error={} result={}", error, result ? result : "(null)");
    });

    // close button does NOT auto-close; connect to closeRequested and call close()
    window->closeRequested.connect([window] {
        std::println("[main] close requested -> closing");
        window->close();
    });

    std::println("[main] the page is loading - click its buttons and watch this console:");
    std::println("[main]   add/greet/repeat = typed DTO arguments, echo = raw JSON,");
    std::println("[main]   fail = Promise reject, spin = Async pool hop, fetch = HttpClient,");
    std::println("[main]   emit / bc.postMessage = BroadcastChannel in both directions,");
    std::println("[main]   evalAsync prints below; close the window with its title-bar button");
    return app->exec();
}
