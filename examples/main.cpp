// HeliosView master demo: a WebView window whose page drives every feature of the
// library through the JS <-> native bridge.
//
// What it demonstrates (this is the "read me first" demo):
//   - One frameless window embedding a WebView; the page's title bar is the injected
//     <helios-window-title-bar> / <helios-window-controls> components (drag +
//     min/max/close), so the app draws its own chrome.
//   - bindJson(name, handler): a native function callable from JS as
//     window.helios.call(name, args) -> Promise. The handler's parameter type is
//     deduced from its own signature and deserialized from the JSON argument
//     (Boost.JSON + Boost.Describe DTOs); what the task returns resolves the Promise,
//     a thrown exception / helios::JsonError rejects it. See the request DTOs below.
//   - broadcast() / subscribeJson(): native -> page and page -> native messages over
//     a BroadcastChannel. The native events (window, WebView, tray, menu, eval) are
//     pushed to the page's "events" channel and logged live.
//   - eval / evalAsync, navigation, local folder mapping, WebView insets.
//   - Right-click: one switch for the engine's own menu (setContextMenuEnabled) plus
//     a native interception gate (contextMenuGate) - the page and the gate can each
//     intercept, and what to show (page menu / native Menu / nothing) is the app's.
//   - Window, dialog, tray, menu, notification and multi-window APIs.
//
// The page on the right logs every call and its result; the sidebar drives the
// features with sliders (numbers) and text inputs (strings).
//
// Bridge names must be C identifiers ([A-Za-z_][A-Za-z0-9_]*): the bridge rejects
// dots (reserved for the library's internal __hv.* names), hence win_minimize,
// dlg_message, ...
#include <HeliosViewCore/HeliosView.h>

#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/describe.hpp>
#include <boost/json.hpp>

namespace {
using json = boost::json::value;
template <class T>
using Task = std::execution::task<T>;

/* ---------- request DTOs: the JS argument object -> a C++ struct ----------
 *
 * BOOST_DESCRIBE_STRUCT maps each member to the JSON key of the same name, so the
 * page's {w: 1024, h: 700} lands in SizeReq with no hand-written parsing. bindJson
 * deduces the parameter type from the handler's signature:
 *
 *     win->bindJson("win_resize", [](SizeReq req) -> Task<bool> { ... });
 *
 * Each DTO is one bridge function's argument; a handler that takes no arguments
 * (the button-only actions) ignores whatever the page sends. */

struct SizeReq { int w; int h; };
BOOST_DESCRIBE_STRUCT(SizeReq, (), (w, h))

struct MoveReq { int x; int y; };
BOOST_DESCRIBE_STRUCT(MoveReq, (), (x, y))

struct OpacityReq { double v; };   /* 0.0 .. 1.0 */
BOOST_DESCRIBE_STRUCT(OpacityReq, (), (v))

struct TitleReq { std::string title; };
BOOST_DESCRIBE_STRUCT(TitleReq, (), (title))

struct ProgressReq { int v; };     /* 0 .. max */
BOOST_DESCRIBE_STRUCT(ProgressReq, (), (v))

struct MessageReq { std::string title; std::string msg; };
BOOST_DESCRIBE_STRUCT(MessageReq, (), (title, msg))

struct UrlReq { std::string url; };
BOOST_DESCRIBE_STRUCT(UrlReq, (), (url))

struct TextReq { std::string text; };
BOOST_DESCRIBE_STRUCT(TextReq, (), (text))

struct ScriptReq { std::string script; };
BOOST_DESCRIBE_STRUCT(ScriptReq, (), (script))

struct AddReq { int a; int b; };
BOOST_DESCRIBE_STRUCT(AddReq, (), (a, b))

struct MsgReq { std::string msg; };
BOOST_DESCRIBE_STRUCT(MsgReq, (), (msg))

/* The page's right-click-menu mode request: "engine" | "page" | "native". */
struct MenuModeReq { std::string mode; };
BOOST_DESCRIBE_STRUCT(MenuModeReq, (), (mode))

/* A BroadcastChannel("status") postMessage from the page (page -> native). */
struct BcMsg { std::string from; int n = 0; };
BOOST_DESCRIBE_STRUCT(BcMsg, (), (from, n))

/* ---------- shared demo state ---------- */

struct DemoState {
    std::shared_ptr<helios::WebViewWindow> win;         /* the main window */
    std::shared_ptr<helios::Tray> tray;                 /* created on demand */
    std::shared_ptr<helios::Menu> menu;                 /* created on demand */
    std::shared_ptr<helios::Menu> contextMenu;          /* the right-click menu (Native mode) */
    std::vector<std::shared_ptr<helios::Window>> extra; /* sub-windows */
    std::string page;                                   /* the demo HTML (wv_home) */
    std::string lastPicked;                             /* last dialog path (dlg_showInFolder) */
    std::string lastLink;                               /* link under the last right-click */
    std::string menuMode = "engine";                    /* this demo's policy: engine|page|native */
    int extraCount = 0;
    bool topmost = false;
    bool resizable = true;
    bool insets = false;
};

/* Push a native event to the page's BroadcastChannel("events"). */
void emit(DemoState& s, const char* ev, const json& payload = boost::json::object{})
{
    if (!s.win)
        return;
    json j = payload;
    j.as_object()["event"] = ev;
    s.win->broadcast("events", boost::json::serialize(j).c_str());
}

/* The main window's current state, as a JSON payload (the win_info button). */
json windowInfo(DemoState& s)
{
    auto* w = s.win.get();
    int32_t x = 0, y = 0, ww = 0, hh = 0;
    w->position(x, y);
    w->size(ww, hh);
    helios::Rect wa{};
    w->workArea(wa);
    return {
        {"windowId", w->id()},
        {"pos", {x, y}},
        {"size", {ww, hh}},
        {"showState", static_cast<int>(w->state())},
        {"visible", w->isVisible()},
        {"enabled", w->isEnabled()},
        {"fullscreen", w->isFullscreen()},
        {"topmost", s.topmost},
        {"resizable", s.resizable},
        {"dpi", w->dpi()},
        {"titleBarHeight", w->titleBarHeight()},
        {"windowCount", heliosview_window_count()},
        {"workArea", {wa.x, wa.y, wa.width, wa.height}},
    };
}

/* ---------- the demo page ---------- */

const char* kDemoPage = R"html(<html><head><meta charset='utf-8'><style>
:root{color-scheme:dark}
*{box-sizing:border-box}
html,body{height:100%;margin:0}
body{font-family:system-ui,'Segoe UI',sans-serif;background:#1e1e2e;color:#cdd6f4;display:flex;flex-direction:column;overflow:hidden}
helios-window-title-bar{flex:0 0 48px;display:flex;align-items:center;gap:8px;padding:0 16px;padding-right:150px;background:#24243a;border-bottom:1px solid #3a3a55;color:#cdd6f4;white-space:nowrap;overflow:hidden}
#title{font-weight:600;font-size:14px}
.layout{flex:1;display:flex;min-height:0}
.sidebar{width:340px;flex:0 0 auto;overflow-y:auto;padding:8px 12px;background:#181825;border-right:1px solid #313244}
.sidebar h3{margin:14px 0 6px;font-size:11px;text-transform:uppercase;letter-spacing:.1em;color:#89b4fa}
.row{display:flex;align-items:center;gap:6px;margin:4px 0;font-size:12px}
.row .lab{flex:0 0 22px;color:#a6adc8}
.row .val{flex:0 0 36px;text-align:right;color:#89b4fa;font-size:11px;font-family:ui-monospace,Consolas,monospace}
.row input[type=range]{flex:1;accent-color:#89b4fa}
.row input[type=text]{flex:1;min-width:0;background:#11111b;color:#cdd6f4;border:1px solid #45475a;border-radius:5px;padding:4px 7px;font-size:12px}
.btnrow{display:flex;flex-wrap:wrap;gap:6px}
button{flex:0 0 auto;background:#313244;color:#cdd6f4;border:1px solid #45475a;border-radius:6px;padding:5px 10px;font-size:12px;cursor:pointer}
button:hover{background:#45475a}
button:active{background:#585b70}
#log{flex:1;overflow-y:auto;padding:10px 14px;font-family:ui-monospace,Consolas,monospace;font-size:12.5px;line-height:1.6}
#log .call{color:#a6e3a1}
#log .ok{color:#89b4fa}
#log .err{color:#f38ba8}
#log .bc{color:#cba6f7}
#log .dim{color:#6c7086}
</style></head><body>
<helios-window-title-bar>
  <span id='title'>HeliosView master demo - every control calls a native function (drag the title bar)</span>
  <helios-window-controls></helios-window-controls>
</helios-window-title-bar>
<div class='layout'>
<div class='sidebar'>
  <h3>Window</h3>
  <div class='row'><span class='lab'>W</span><input type='range' id='sw' min='400' max='1600' step='10' value='1024'><span class='val' id='swv'>1024</span></div>
  <div class='row'><span class='lab'>H</span><input type='range' id='sh' min='300' max='1200' step='10' value='700'><span class='val' id='shv'>700</span></div>
  <div class='btnrow'><button onclick="run('win_resize',{w:+sw.value,h:+sh.value})">Apply Size</button></div>
  <div class='row'><span class='lab'>X</span><input type='range' id='sx' min='0' max='3000' step='10' value='120'><span class='val' id='sxv'>120</span></div>
  <div class='row'><span class='lab'>Y</span><input type='range' id='sy' min='0' max='2000' step='10' value='80'><span class='val' id='syv'>80</span></div>
  <div class='btnrow'><button onclick="run('win_move',{x:+sx.value,y:+sy.value})">Move</button></div>
  <div class='row'><span class='lab'>Op</span><input type='range' id='sop' min='0' max='100' step='5' value='100'><span class='val' id='sopv'>100</span></div>
  <div class='btnrow'>
    <button onclick="run('win_recreate_webview',{})">Restart webview</button>
    <button onclick="run('win_minimize',{})">Minimize</button>
    <button onclick="run('win_maximize',{})">Maximize</button>
    <button onclick="run('win_restore',{})">Restore</button>
    <button onclick="run('win_toggleMaximize',{})">Toggle Max</button>
    <button onclick="run('win_fullscreen',{})">Fullscreen</button>
    <button onclick="run('win_hide',{})">Hide</button>
    <button onclick="run('win_show',{})">Show</button>
    <button onclick="run('win_center',{})">Center</button>
    <button onclick="run('win_info',{})">Info</button>
    <button onclick="run('win_focus',{})">Focus</button>
    <button onclick="run('win_workarea',{})">Work Area</button>
    <button onclick="run('win_topmost',{})">Topmost</button>
    <button onclick="run('win_resizable',{})">Resizable</button>
    <button onclick="run('win_minsize',{})">Min Size</button>
    <button onclick="run('win_maxsize',{})">Max Size</button>
  </div>
  <div class='row'><input type='text' id='in_title' value='HeliosView Master Demo'><button onclick="run('win_title',{title:in_title.value})">Rename</button></div>
  <div class='row'><span class='lab'>Pr</span><input type='range' id='spr' min='0' max='100' step='5' value='50'><span class='val' id='sprv'>50</span></div>
  <div class='btnrow'>
    <button onclick="run('win_progress',{v:+spr.value})">Set Progress</button>
    <button onclick="run('win_progressIndeterminate',{})">Indeterminate</button>
    <button onclick="run('win_progressClear',{})">Clear</button>
    <button onclick="run('win_flash',{})">Flash</button>
    <button onclick="run('win_flashUntilFocus',{})">Flash Until Focus</button>
    <button onclick="run('win_backdrop',{})">Mica + Dark</button>
  </div>
  <h3>Dialogs &amp; System</h3>
  <div class='row'><input type='text' id='in_mbt' value='HeliosView'><input type='text' id='in_mbm' value='This is a message box demo'></div>
  <div class='btnrow'><button onclick="run('dlg_message',{title:in_mbt.value,msg:in_mbm.value})">Message Box</button></div>
  <div class='btnrow'>
    <button onclick="run('dlg_folder',{})">Pick Folder</button>
    <button onclick="run('dlg_openFiles',{})">Open Files</button>
    <button onclick="run('dlg_saveFile',{})">Save File</button>
    <button onclick="run('dlg_cursor',{})">Cursor</button>
    <button onclick="run('dlg_showInFolder',{})">Show in Folder</button>
    <button onclick="run('dlg_workareas',{})">Work Areas</button>
  </div>
  <div class='row'><input type='text' id='in_url' value='https://example.com'><button onclick="run('dlg_openUrl',{url:in_url.value})">Open URL</button></div>
  <div class='row'><input type='text' id='in_clip' value='Text from the HeliosView master demo'><button onclick="run('dlg_clipboardSet',{text:in_clip.value})">Copy</button><button onclick="run('dlg_clipboardGet',{})">Read</button></div>
  <h3>WebView Bridge</h3>
  <div class='row'><input type='text' id='in_nav' value='https://example.com'><button onclick="run('wv_navigate',{url:in_nav.value})">Navigate</button><button onclick="run('wv_home',{})">Back</button></div>
  <div class='row'><input type='text' id='in_eval' value='1 + 1'><button onclick="run('wv_eval',{script:in_eval.value})">Eval</button></div>
  <div class='row'><span class='lab'>A</span><input type='range' id='sa' min='0' max='100' step='1' value='20'><span class='val' id='sav'>20</span></div>
  <div class='row'><span class='lab'>B</span><input type='range' id='sb' min='0' max='100' step='1' value='22'><span class='val' id='sbv'>22</span></div>
  <div class='btnrow'>
    <button onclick="run('wv_add',{a:+sa.value,b:+sb.value})">Bridge add (typed args)</button>
    <button onclick="run('echo_obj',{x:1,y:'hi'})">Bridge echo (raw JSON)</button>
    <button onclick="run('wv_fail',{})">Bridge reject</button>
  </div>
  <div class='row'><input type='text' id='in_bc' value='hello from native'><button onclick="run('wv_broadcast',{msg:in_bc.value})">Native -&gt; Page</button><button onclick="bcSend()">Page -&gt; Native</button></div>
  <div class='btnrow'>
    <button onclick="run('wv_insets',{})">Top Inset</button>
    <button onclick="run('wv_mapLocal',{})">Map Local Folder</button>
  </div>
  <h3>Tray, Menu &amp; Notifications</h3>
  <div class='btnrow'>
    <button onclick="run('tray_create',{})">Create Tray</button>
    <button onclick="run('tray_notify',{})">Balloon</button>
    <button onclick="run('tray_remove',{})">Remove</button>
    <button onclick="run('menu_show',{})">Popup Menu</button>
    <button onclick="run('notify_show',{})">System Toast</button>
  </div>
  <h3>Right-click Menu</h3>
  <div class='btnrow'>
    <button onclick="setMenuMode('engine')">engine's own</button>
    <button onclick="setMenuMode('page')">drawn by the page</button>
    <button onclick="setMenuMode('native')">native Menu</button>
  </div>
  <div class='row'><span class='lab'>now</span><span class='val' id='menumode' style='flex:1;text-align:left'>engine</span></div>
  <div id='ctxbox' style='border:1px dashed #585b70;border-radius:6px;padding:6px 8px;font-size:12px;line-height:1.7'>
    right-click me:<br>a <a href='https://example.com/from-menu' style='color:#89b4fa'>link</a>,
    some selectable text, and an <input value='input' style='width:70px;background:#11111b;color:#cdd6f4;border:1px solid #45475a;border-radius:4px'>
  </div>
  <h3>Windows &amp; Quit</h3>
  <div class='btnrow'>
    <button onclick="run('win_new',{})">New Sub-window</button>
    <button onclick="run('win_closeAll',{})">Close All</button>
    <button onclick="run('app_quit',{})">Quit App</button>
  </div>
</div>
<div id='pagemenu' style='display:none;position:fixed;background:#313244;border:1px solid #585b70;border-radius:6px;padding:4px 0;font-size:12px;box-shadow:0 6px 18px #0008;min-width:150px;z-index:9'>
  <div style='padding:4px 12px;opacity:.7'>page-drawn menu</div>
  <div style='padding:4px 12px;cursor:pointer' onclick="pmenuItem('Hello from the page menu')">Hello</div>
  <div style='padding:4px 12px;cursor:pointer' onclick="pmenuItem('Another page item')">Another item</div>
</div>
<div id='log'><div class='dim'>Every button calls window.helios.call('&lt;name&gt;', args) - a native bindJson handler. It resolves with the returned JSON (logged as "return") or rejects with an error (logged as "error"). Slider/input values are sent as the argument object; broadcast() and native events arrive on the BroadcastChannel('events') and appear here as "[native] ...".</div></div>
</div>
<script>
  const log = document.getElementById('log');
  function line(txt, cls){
    const d = document.createElement('div');
    if(cls) d.className = cls;
    d.textContent = txt;
    log.appendChild(d);
    log.scrollTop = log.scrollHeight;
  }
  async function run(name, args){
    line('call: ' + name, 'call');
    try {
      const r = await window.helios.call(name, args);
      line('return: ' + JSON.stringify(r), 'ok');
    } catch(e){
      line('error: ' + e.message, 'err');
    }
  }
  // Wire each slider to its value label; opacity applies on release.
  function slider(id, out){
    const el = document.getElementById(id), o = document.getElementById(out);
    const upd = () => o.textContent = el.value;
    el.addEventListener('input', upd);
    upd();
    return el;
  }
  const sw = slider('sw','swv'), sh = slider('sh','shv');
  const sx = slider('sx','sxv'), sy = slider('sy','syv');
  const sop = slider('sop','sopv');
  sop.addEventListener('change', () => run('win_opacity', {v: +sop.value/100}));
  const spr = slider('spr','sprv');
  const sa = slider('sa','sav'), sb = slider('sb','sbv');
  const ev = new BroadcastChannel('events');
  ev.onmessage = e => line('[native] ' + JSON.stringify(e.data), 'bc');
  const bc = new BroadcastChannel('status');
  function bcSend(){
    bc.postMessage({from:'js', n: Math.floor(Math.random()*100)});
    line('[page] posted to native via BroadcastChannel("status")', 'bc');
  }
  // Right-click: pick what happens, and draw the page's own menu when the page is
  // the one handling it (the native side then leaves the click alone).
  let menuMode = 'engine';
  async function setMenuMode(m){
    menuMode = m;
    document.getElementById('menumode').textContent = m;
    await run('menu_mode', {mode: m});
  }
  const pageMenu = document.getElementById('pagemenu');
  function pmenuItem(txt){ pageMenu.style.display = 'none'; line('[page menu] ' + txt, 'bc'); }
  addEventListener('contextmenu', e => {
    if (menuMode !== 'page') return;   // the engine / the native handler owns the others
    e.preventDefault();
    pageMenu.style.left = Math.min(e.clientX, innerWidth - 170) + 'px';
    pageMenu.style.top  = Math.min(e.clientY, innerHeight - 90) + 'px';
    pageMenu.style.display = 'block';
    line('[page] contextmenu event -> drawing the page menu', 'bc');
  });
  addEventListener('click', () => pageMenu.style.display = 'none');
</script>
</body></html>)html";

} // namespace

int main()
{
    std::println("HeliosView {} - master demo", helios::version());
    /* DPI awareness is initialized automatically with the first window; the explicit
     * call is only needed for early initialization (e.g. before App). */
    helios::enableDpiAwareness();
    helios::App::setAppId("com.example.heliosview.masterdemo"); /* Windows AUMID / macOS bundle id / Linux app id */
    std::println("[init] app id: {}", helios::App::appId());
    std::println("[init] notification backend: {}", helios::notificationInit() ? "ok" : "failed (unpackaged app)");
    helios::notificationRequestPermission([](helios::NotificationPermission permission) {
        std::println("[init] notification permission: {}", static_cast<int>(permission));
    });

    /* Order matters: app first, then state - Tray/Menu use App::instance(), and
     * state (with the window/WebView) is destroyed before the App. */
    helios::App app;
    DemoState state;
    state.page = kDemoPage;
    state.win = std::make_shared<helios::WebViewWindow>(1024, 700, "HeliosView Master Demo",
                                                        helios::WindowStyle::Frameless);
    auto* win = state.win.get();
    win->createWebView();
    win->firstShown.connect([&state] { emit(state, "window-first-shown"); std::println("window first shown"); });
    win->show();

    /* ---- native events -> page (BroadcastChannel "events") ----
     * Signals are the native side of the same conversation: connect a lambda and
     * forward whatever the page should see. Every callback runs on the UI thread. */

    win->resized.connect([&state](int32_t w, int32_t h) { emit(state, "window-resized", {{"w", w}, {"h", h}}); });
    win->moved.connect([&state](int32_t x, int32_t y) { emit(state, "window-moved", {{"x", x}, {"y", y}}); });
    win->focused.connect([&state] { emit(state, "window-focused"); });
    win->blurred.connect([&state] { emit(state, "window-blurred"); });
    win->keyPressed.connect([&state](helios::KeyCode k) { emit(state, "key-pressed", {{"key", static_cast<int>(k)}}); });
    win->mouseButtonPressed.connect([&state](int32_t x, int32_t y, helios::MouseButton b) {
        emit(state, "mouse-pressed", {{"x", x}, {"y", y}, {"button", static_cast<int>(b)}});
    });
    win->navigationCompleted.connect([&state](int error) { emit(state, "webview-navigation-completed", {{"error", error}}); });
    win->titleChanged.connect([&state](std::string t) { emit(state, "webview-title-changed", {{"title", t}}); });
    win->urlChanged.connect([&state](std::string u, bool newDoc) {
        emit(state, "webview-url-changed", {{"url", u}, {"newDocument", newDoc}});
    });

    /* ---- right-click: the switch (engine menu on/off) + the native gate ----
     * The gate is consulted for every right click and returns true to intercept it
     * (the engine's menu stays closed). What to show is this app's business: here a
     * helios::Menu popped at the cursor. */
    auto contextMenu = std::make_shared<helios::Menu>();
    contextMenu->addItem("Copy selection")->triggered.connect([&state] {
        std::println("[native] context menu: copy selection");
        emit(state, "context-menu-item", {{"item", "copy"}});
        state.win->eval("document.execCommand('copy')");
    });
    contextMenu->addItem("Open link in the browser")->triggered.connect([&state] {
        std::println("[native] context menu: open '{}'", state.lastLink);
        emit(state, "context-menu-item", {{"item", "open-link"}, {"url", state.lastLink}});
        if (!state.lastLink.empty())
            helios::openUrl(state.lastLink);
    });
    contextMenu->addSeparator();
    contextMenu->addItem("Log the page URL")->triggered.connect([&state] {
        emit(state, "context-menu-item", {{"item", "page-url"}});
        state.win->eval("console.log('page URL:', location.href)");
        std::println("[native] context menu: logged the page URL");
    });
    state.contextMenu = contextMenu;

    win->contextMenuGate = [&state](const helios::ContextMenuInfo& info) {
        emit(state, "context-menu-request",
             {{"target", helios::toUint(info.target)},
              {"link", info.linkUrl},
              {"selection", info.selectionText},
              {"x", info.x},
              {"y", info.y}});
        if (state.menuMode != "native") {
            /* "engine": the page does not intercept, the engine's menu shows.
             * "page": the switch is off, so neither the engine nor this gate opens
             * anything - the page draws its own menu from its 'contextmenu' event. */
            return false;
        }
        if (info.has(helios::ContextMenuTarget::Editable)) {
            /* Inside a text field the engine's own items (paste, spell check) are
             * more useful: do not intercept, and the switch (still enabled) opens
             * the engine's menu for this click. */
            std::println("[native] right-click in a text field -> engine menu");
            return false;
        }
        if (!info.linkUrl.empty())
            std::println("[native] right-click on '{}' -> native menu", info.linkUrl);
        state.lastLink = info.linkUrl;
        /* Suppress the engine's menu now, open ours on the next UI turn: this
         * callback runs inside the engine's event dispatch and a popup menu runs a
         * modal message loop, so showing it here would re-enter the engine. */
        auto menu = state.contextMenu;
        auto* win = state.win.get();
        if (auto* app = helios::App::instance())
            app->postTask([menu, win] { menu->show(win->nativeHandle()); });
        else
            menu->show(win->nativeHandle());
        return true; /* intercepted: keep the engine's menu closed */
    };

    /* All page <-> native registrations (bindJson / subscribeJson) are made through
     * this lambda so they can be re-applied after a WebView recreate: the bindings
     * live on the C-layer webview instance and are dropped when destroyWebView()
     * tears it down - the C++ wrapper does not keep a copy. setupBridge runs after
     * the initial createWebView() and again inside the "Restart webview" handler.
     *
     * std::function instead of auto: the Restart handler re-registers the bridge,
     * so the name must be declared before the lambda that calls it. */
    std::function<void(helios::WebViewWindow*)> setupBridge;
    setupBridge = [&](helios::WebViewWindow* win) {

    /* ---- page -> native: BroadcastChannel("status") posts ----
     * subscribeJson deduces the DTO from the callback and runs it on the UI thread. */
    win->subscribeJson("status", [&state](BcMsg m) {
        emit(state, "js-broadcast", {{"from", m.from}, {"n", m.n}});
    });

    /* ---- the page switches what a right click does ----
     * The library only needs the switch (may the engine's menu open?); what the gate
     * above does with the click is this demo's own policy. The switch lives on the
     * C-layer webview instance, so it is re-applied below whenever setupBridge runs
     * (the Restart button builds a fresh WebView). */
    win->bindJson("menu_mode", [&state](MenuModeReq req) -> Task<json> {
        state.menuMode = req.mode;                                   /* engine|page|native */
        const bool engineMenu = req.mode != "page";
        const int rc = state.win->setContextMenuEnabled(engineMenu);
        std::println("[native] right-click -> mode '{}': engine menu {} (rc={})", req.mode,
                     engineMenu ? "enabled" : "suppressed", rc);
        emit(state, "context-menu-mode", {{"mode", req.mode}, {"rc", rc}});
        co_return json{{"mode", req.mode}, {"rc", rc}};
    });

    /* ================= Window =================
     * The button-only actions: a zero-argument handler (the page still sends {}).
     * Resolving true tells the page the call succeeded. */
    win->bindJson("win_minimize", [&state]() -> Task<bool> { state.win->minimize(); co_return true; });
    win->bindJson("win_maximize", [&state]() -> Task<bool> { state.win->maximize(); co_return true; });
    win->bindJson("win_restore", [&state]() -> Task<bool> { state.win->restore(); co_return true; });
    win->bindJson("win_toggleMaximize", [&state]() -> Task<bool> { state.win->toggleMaximize(); co_return true; });
    win->bindJson("win_fullscreen", [&state]() -> Task<bool> {
        state.win->setFullscreen(!state.win->isFullscreen());
        co_return true;
    });
    win->bindJson("win_hide", [&state]() -> Task<bool> { state.win->hide(); co_return true; });
    win->bindJson("win_show", [&state]() -> Task<bool> { state.win->showNormal(); co_return true; });
    win->bindJson("win_center", [&state]() -> Task<bool> { state.win->center(); co_return true; });
    win->bindJson("win_focus", [&state]() -> Task<bool> { state.win->focus(); co_return true; });
    win->bindJson("win_topmost", [&state]() -> Task<bool> {
        state.topmost = !state.topmost;
        state.win->setTopmost(state.topmost);
        co_return true;
    });
    win->bindJson("win_resizable", [&state]() -> Task<bool> {
        state.resizable = !state.resizable;
        state.win->setResizable(state.resizable);
        co_return true;
    });
    win->bindJson("win_minsize", [&state]() -> Task<bool> { state.win->setMinimumSize(420, 320); co_return true; });
    win->bindJson("win_maxsize", [&state]() -> Task<bool> { state.win->setMaximumSize(1600, 1200); co_return true; });
    win->bindJson("win_progressIndeterminate", [&state]() -> Task<bool> {
        state.win->setProgressState(helios::ProgressState::Indeterminate);
        co_return true;
    });
    win->bindJson("win_progressClear", [&state]() -> Task<bool> { state.win->clearProgress(); co_return true; });
    win->bindJson("win_flash", [&state]() -> Task<bool> { state.win->flash(); co_return true; });
    win->bindJson("win_flashUntilFocus", [&state]() -> Task<bool> { state.win->flashUntilFocus(); co_return true; });
    win->bindJson("win_backdrop", [&state]() -> Task<bool> {
        state.win->setBackdrop(helios::Backdrop::Mica);
        state.win->setDarkMode(true);
        co_return true;
    });

    /* The Restart button rebuilds the whole WebView. destroyWebView() must not run
     * from inside its own JS bridge call (the C layer requires destroying a WebView
     * only when no asynchronous call is in flight), so it is deferred to the next
     * UI idle turn with App::postTask. The recreated WebView comes back empty:
     * setupBridge re-registers everything and the page is loaded again. */
    win->bindJson("win_recreate_webview", [&state, &setupBridge]() -> Task<bool> {
        auto recreate = [&] {
            state.win->destroyWebView();
            state.win->createWebView();
            setupBridge(state.win.get()); /* re-register the page <-> native bridge */
            state.win->navigateHtml(state.page.c_str());
        };
        if (auto* app = helios::App::instance())
            app->postTask(recreate);
        else
            recreate();
        co_return true;
    });

    /* Typed argument: SizeReq { int w; int h; } <- {w: 1024, h: 700} */
    win->bindJson("win_resize", [&state](SizeReq req) -> Task<bool> {
        state.win->resize(req.w, req.h);
        co_return true;
    });
    win->bindJson("win_move", [&state](MoveReq req) -> Task<bool> {
        state.win->move(req.x, req.y);
        co_return true;
    });
    win->bindJson("win_opacity", [&state](OpacityReq req) -> Task<bool> {
        state.win->setOpacity(static_cast<float>(req.v));
        co_return true;
    });
    win->bindJson("win_title", [&state](TitleReq req) -> Task<bool> {
        state.win->setTitle(req.title.c_str());
        co_return true;
    });
    win->bindJson("win_progress", [&state](ProgressReq req) -> Task<bool> {
        state.win->setProgress(static_cast<uint32_t>(req.v), 100);
        co_return true;
    });

    /* Returning JSON: whatever the task returns is serialized back to the Promise.
     * Returning boost::json::value builds the payload directly. */
    win->bindJson("win_info", [&state]() -> Task<json> { co_return windowInfo(state); });
    win->bindJson("win_workarea", [&state]() -> Task<json> {
        helios::Rect r{};
        state.win->workArea(r);
        co_return json{{"x", r.x}, {"y", r.y}, {"w", r.width}, {"h", r.height}};
    });

    /* ================= Dialogs & System ================= */

    win->bindJson("dlg_message", [&state](MessageReq req) -> Task<json> {
        const auto r = helios::messageBox(state.win->nativeHandle(), helios::MessageBoxType::Question,
                                          helios::MessageBoxButtons::YesNo, req.title.c_str(),
                                          req.msg.c_str());
        co_return json{{"result", r == helios::MessageBoxResult::Yes ? "Yes" : "No"}};
    });
    win->bindJson("dlg_folder", [&state]() -> Task<json> {
        std::string path;
        if (!helios::selectFolder(state.win->nativeHandle(), "Pick a folder", path))
            throw std::runtime_error("cancelled"); /* the Promise rejects with {"error": ...} */
        state.lastPicked = path;
        co_return json{{"path", path}};
    });
    win->bindJson("dlg_openFiles", [&state]() -> Task<json> {
        const auto files = helios::openFiles(state.win->nativeHandle(), "Pick files",
                                             std::vector<helios::FileFilter>{{"All files", "*.*"}}, true);
        if (files.empty())
            throw std::runtime_error("cancelled");
        state.lastPicked = files.front();
        co_return json{{"count", files.size()}, {"paths", files}}; /* the full list */
    });
    win->bindJson("dlg_saveFile", [&state]() -> Task<json> {
        std::string path;
        if (!helios::saveFile(state.win->nativeHandle(), "Save file",
                              std::vector<helios::FileFilter>{{"Text files", "txt"}},
                              "untitled.txt", path))
            throw std::runtime_error("cancelled");
        state.lastPicked = path;
        co_return json{{"path", path}};
    });
    win->bindJson("dlg_openUrl", [](UrlReq req) -> Task<bool> {
        co_return helios::openUrl(req.url);
    });
    win->bindJson("dlg_clipboardSet", [](TextReq req) -> Task<bool> {
        co_return helios::clipboardSetText(req.text);
    });
    win->bindJson("dlg_clipboardGet", []() -> Task<json> {
        std::string text;
        if (!helios::clipboardGetText(text))
            throw std::runtime_error("clipboard has no text");
        co_return json{{"text", text}};
    });
    win->bindJson("dlg_cursor", []() -> Task<json> {
        int32_t x = 0, y = 0;
        helios::cursorPosition(x, y);
        co_return json{{"x", x}, {"y", y}};
    });
    win->bindJson("dlg_showInFolder", [&state]() -> Task<json> {
        if (state.lastPicked.empty())
            throw std::runtime_error("pick a folder or file first");
        if (!helios::showInFolder(state.lastPicked))
            throw std::runtime_error("showInFolder failed");
        co_return json{{"path", state.lastPicked}};
    });
    win->bindJson("dlg_workareas", []() -> Task<json> {
        helios::Rect primary{}, atCursor{};
        helios::primaryWorkArea(primary);
        int32_t x = 0, y = 0;
        helios::cursorPosition(x, y);
        helios::screenWorkArea(x, y, atCursor);
        co_return json{{"primary", {primary.x, primary.y, primary.width, primary.height}},
                       {"atCursor", {atCursor.x, atCursor.y, atCursor.width, atCursor.height}}};
    });

    /* ================= WebView Bridge ================= */

    win->bindJson("wv_navigate", [&state](UrlReq req) -> Task<json> {
        state.win->navigate(req.url.c_str());
        co_return json{{"url", req.url}};
    });
    win->bindJson("wv_home", [&state]() -> Task<bool> {
        state.win->navigateHtml(state.page.c_str());
        co_return true;
    });
    win->bindJson("wv_eval", [&state](ScriptReq req) -> Task<json> {
        state.win->evalAsync(req.script.c_str(), [](int error, const char* result, void* userdata) {
            auto& s = *static_cast<DemoState*>(userdata);
            emit(s, "eval-result", {{"error", error}, {"result", result ? result : ""}});
        }, &state);
        co_return json{{"note", "evalAsync dispatched; the result comes back on the events channel"}};
    });
    win->bindJson("wv_insets", [&state]() -> Task<bool> {
        state.insets = !state.insets;
        state.win->setWebViewInsets(state.insets ? 56 : 0, 0, 0, 0);
        co_return state.insets;
    });
    win->bindJson("wv_mapLocal", [&state]() -> Task<json> {
        const std::string cwd = std::filesystem::current_path().string();
        const int rc = state.win->mapLocalFolder("assets.local", cwd.c_str());
        co_return json{{"rc", rc}, {"host", "https://assets.local"}, {"folder", cwd}};
    });
    win->bindJson("wv_broadcast", [&state](MsgReq req) -> Task<bool> {
        emit(state, "native-broadcast", {{"msg", req.msg}});
        co_return true;
    });
    /* Typed arguments: {a: 20, b: 22} -> AddReq, the result resolves as a number. */
    win->bindJson("wv_add", [](AddReq req) -> Task<int> {
        co_return req.a + req.b;
    });
    /* Raw JSON: boost::json::value is a valid parameter type too - use it for
     * payloads whose shape is decided by the page. */
    win->bindJson("echo_obj", [](json arg) -> Task<json> {
        co_return json{{"echo", arg}, {"type", arg.is_object() ? "object" : "other"}};
    });
    /* Rejecting: helios::JsonError rejects the Promise with a custom payload
     * (throwing rejects with {"error": what()}). */
    win->bindJson("wv_fail", []() -> Task<helios::JsonError> {
        co_return helios::JsonError{"error", "this is a deliberate reject demo"};
    });

    /* ================= Tray / Menu / Notifications ================= */

    win->bindJson("tray_create", [&state]() -> Task<bool> {
        if (!state.tray) {
            state.tray = std::make_shared<helios::Tray>("HeliosView Master Demo");
            if (!state.tray->valid()) {
                state.tray.reset();
                throw std::runtime_error("tray creation failed");
            }
            state.tray->leftClicked.connect([&state] { emit(state, "tray-left-click"); });
            state.tray->leftDoubleClicked.connect([&state] {
                emit(state, "tray-left-double-click");
                state.win->showNormal();
            });
            state.tray->rightClicked.connect([&state] {
                emit(state, "tray-right-click");
                state.win->showNormal();
            });
        }
        co_return true;
    });
    win->bindJson("tray_notify", [&state]() -> Task<bool> {
        if (!state.tray)
            throw std::runtime_error("create the tray first");
        co_return state.tray->notify("HeliosView", "Tray balloon notification", helios::NotifyIcon::Info);
    });
    win->bindJson("tray_remove", [&state]() -> Task<bool> {
        state.tray.reset();
        co_return true;
    });
    win->bindJson("menu_show", [&state]() -> Task<bool> {
        if (!state.menu) {
            auto menu = std::make_shared<helios::Menu>();
            if (!menu->valid())
                throw std::runtime_error("menu creation failed");
            menu->addItem("Show / Restore")->triggered.connect([&state] {
                state.win->showNormal();
                emit(state, "menu-item", {{"item", "show/restore"}});
            });
            menu->addItem("Maximize")->triggered.connect([&state] {
                state.win->maximize();
                emit(state, "menu-item", {{"item", "maximize"}});
            });
            auto* topmost = menu->addCheckItem("Toggle Topmost", state.topmost);
            topmost->triggered.connect([&state, topmost] {
                state.topmost = !state.topmost;
                state.win->setTopmost(state.topmost);
                topmost->setChecked(state.topmost); /* keep the checkmark in sync */
                emit(state, "menu-item", {{"item", "topmost"}});
            });
            menu->addSeparator();
            menu->addItem("Open URL")->triggered.connect([&state] {
                helios::openUrl("https://example.com");
                emit(state, "menu-item", {{"item", "open-url"}});
            });
            menu->addSeparator();
            menu->addItem("Quit")->triggered.connect([&state] {
                emit(state, "menu-item", {{"item", "quit"}});
                if (auto* a = helios::App::instance())
                    a->quit();
            });
            state.menu = std::move(menu);
        }
        state.menu->show(state.win->nativeHandle());
        co_return true;
    });
    win->bindJson("notify_show", []() -> Task<bool> {
        /* OS toasts are the one API that is safe from any thread (the handler may
         * already be running off it after co_await schedule(...)). */
        co_return helios::notificationShow("HeliosView", "System toast notification");
    });

    /* ================= Multiple Windows & Quit ================= */

    win->bindJson("win_new", [&state]() -> Task<json> {
        const int n = ++state.extraCount;
        auto w = std::make_shared<helios::Window>(420, 300, std::format("Sub-window #{}", n).c_str());
        w->show();
        w->keyPressed.connect([&state, n](helios::KeyCode k) {
            emit(state, "subwindow-key", {{"n", n}, {"key", static_cast<int>(k)}});
        });
        w->closeRequested.connect([&state, n, raw = w.get()] {
            emit(state, "subwindow-closed", {{"n", n}});
            raw->close();  /* close button does NOT auto-close; call close() here */
        });
        state.extra.push_back(std::move(w));
        co_return json{{"n", n}, {"count", state.extra.size()}};
    });
    win->bindJson("win_closeAll", [&state]() -> Task<json> {
        const int n = static_cast<int>(state.extra.size());
        state.extra.clear(); /* the dtors close the matching native windows */
        co_return json{{"closed", n}};
    });
    win->bindJson("app_quit", []() -> Task<bool> {
        if (auto* a = helios::App::instance())
            a->quit();
        co_return true;
    });

    /* Re-apply the right-click switch: it lives on the C-layer webview instance,
     * which destroyWebView()/createWebView() (the Restart button) replaced. */
    state.win->setContextMenuEnabled(state.menuMode != "page");
    }; /* setupBridge */

    /* register all page <-> native bindings on the initial webview */
    setupBridge(win);

    // close button does NOT auto-close; connect to closeRequested and call close()
    win->closeRequested.connect([win] {
        std::println("[main] close requested -> closing");
        win->close();
    });

    /* ---- load the demo page ---- */
    win->navigateHtml(state.page.c_str());

    std::println("[main] entering UI loop - drive every feature from the page controls");
    return app.exec();
}
