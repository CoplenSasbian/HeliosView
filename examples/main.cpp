// HeliosView master demo: a WebView window whose page drives every feature of
// the library through the JS <-> native bridge.
//
//   - One Frameless window embedding a WebView (WebView2); the page's title bar
//     is the injected <helios-window-title-bar> / <helios-window-controls>
//     components (drag + min/max/close).
//   - Numeric parameters are set with sliders (size, position, opacity,
//     progress, add operands); text parameters with input boxes (title, URL,
//     eval script, message box, clipboard, broadcast payload). Every control
//     calls window.helios.call(name, args) -> Promise; the native handler
//     performs the feature and resolves/rejects with a result the page logs.
//   - Native events (window resize/move/focus/keys, WebView navigation, tray
//     clicks, menu items, eval results, sub-window activity) are pushed back to
//     the page on a BroadcastChannel("events") and logged live.
//   - Covers: window ops, taskbar progress/flash, backdrop, dialogs, system
//     helpers, WebView bridge (bind/subscribe/broadcast/eval/insets/local
//     folder), tray, popup menu, toast notifications, sub-windows, quit.
//
// Binding names must be C identifiers ([A-Za-z_][A-Za-z0-9_]*): the bridge
// rejects dots (they are reserved for the library's internal __hv.* names), so
// the demo uses underscores (win_minimize, dlg_message, ...).
#include <HeliosViewCore/HeliosView.h>

#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <boost/describe.hpp>
#include <boost/json.hpp>

namespace {
using json = boost::json::value;
template <class T>
using Task = std::execution::task<T>;

// boost::json::value has no jget(j, "key", default) accessor (nlohmann::json did);
// jget provides the equivalent: value_to<T>(j["key"]) or `fallback` when missing.
template <class T>
T jget(const json& j, std::string_view key, T fallback)
{
    if (const auto* obj = j.if_object())
        if (const auto* it = obj->if_contains(key))
            return boost::json::value_to<T>(*it);
    return fallback;
}

/* ---------- shared demo state ---------- */

struct DemoState {
    std::shared_ptr<helios::WebViewWindow> win;      /* the main window */
    std::shared_ptr<helios::Tray> tray;              /* created on demand */
    std::shared_ptr<helios::Menu> menu;              /* created on demand */
    std::vector<std::shared_ptr<helios::Window>> extra; /* sub-windows */
    std::string page;                                /* the demo HTML (wv_home) */
    std::string lastPicked;                          /* last dialog path (dlg_showInFolder) */
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

/* Bind a no-argument action: JS run('name', {}) resolves true. */
void bindAction(helios::WebViewWindow* win, const char* name, std::function<void()> fn)
{
    win->bindJson<json>(name, [fn](json) -> Task<bool> {
        fn();
        co_return true;
    });
}

/* The main window's current state, as a JSON payload (win_info button). */
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

/* A BroadcastChannel("status") postMessage DTO (JS -> native subscribe demo). */
struct BcMsg {
    std::string from;
    int n = 0;
};
BOOST_DESCRIBE_STRUCT(BcMsg, (), (from, n))

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
  <span id='title'>HeliosView Master Demo - sliders for numbers, inputs for text (drag the title bar)</span>
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
  <div class='btnrow'><button onclick="run('wv_add',{a:+sa.value,b:+sb.value})">Bridge add</button><button onclick="run('wv_fail',{})">Bridge fail</button></div>
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
  <h3>Windows &amp; Quit</h3>
  <div class='btnrow'>
    <button onclick="run('win_new',{})">New Sub-window</button>
    <button onclick="run('win_closeAll',{})">Close All</button>
    <button onclick="run('app_quit',{})">Quit App</button>
  </div>
</div>
<div id='log'><div class='dim'>Click the controls to call native features; window events, tray, menus, broadcasts and eval results appear here live.</div></div>
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
</script>
</body></html>)html";

} // namespace

int main()
{
    std::println("HeliosView {} - master demo", helios::version());
    helios::enableDpiAwareness();
    std::println("[init] notification backend: {}", helios::notificationInit() ? "ok" : "failed (unpackaged app)");

    /* Order matters: app first, then state - Tray/Menu use App::instance(), and
     * state (with the window/WebView) is destroyed before the App. */
    helios::App app;
    DemoState state;
    state.page = kDemoPage;
    state.win = std::make_shared<helios::WebViewWindow>(1024, 700, "HeliosView Master Demo",
                                                        helios::WindowStyle::Frameless);
    auto* win = state.win.get();
    win->createWebView();
    win->ready.connect([&state] { emit(state, "window-ready"); std::println("window ready"); });
    win->show();

    /* ---- native events -> page (BroadcastChannel "events") ---- */

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


    /* ---- page -> native: BroadcastChannel("status") posts ---- */
    win->subscribeJson<BcMsg>("status", [&state](BcMsg m) {
        emit(state, "js-broadcast", {{"from", m.from}, {"n", m.n}});
    });

    /* ================= Window ================= */

    bindAction(win, "win_minimize", [&state] { state.win->minimize(); });
    bindAction(win, "win_maximize", [&state] { state.win->maximize(); });
    bindAction(win, "win_restore", [&state] { state.win->restore(); });
    bindAction(win, "win_toggleMaximize", [&state] { state.win->toggleMaximize(); });
    bindAction(win, "win_fullscreen", [&state] { state.win->setFullscreen(!state.win->isFullscreen()); });
    bindAction(win, "win_hide", [&state] { state.win->hide(); });
    bindAction(win, "win_show", [&state] { state.win->showNormal(); });
    bindAction(win, "win_center", [&state] { state.win->center(); });
    bindAction(win, "win_topmost", [&state] {
        state.topmost = !state.topmost;
        state.win->setTopmost(state.topmost);
    });
    bindAction(win, "win_resizable", [&state] {
        state.resizable = !state.resizable;
        state.win->setResizable(state.resizable);
    });
    bindAction(win, "win_minsize", [&state] { state.win->setMinimumSize(420, 320); });
    bindAction(win, "win_maxsize", [&state] { state.win->setMaximumSize(1600, 1200); });
    bindAction(win, "win_progressIndeterminate", [&state] {
        state.win->setProgressState(helios::ProgressState::Indeterminate);
    });
    bindAction(win, "win_progressClear", [&state] { state.win->clearProgress(); });
    bindAction(win, "win_flash", [&state] { state.win->flash(); });
    bindAction(win, "win_flashUntilFocus", [&state] { state.win->flashUntilFocus(); });
    bindAction(win, "win_backdrop", [&state] {
        state.win->setBackdrop(helios::Backdrop::Mica);
        state.win->setDarkMode(true);
    });
    bindAction(win, "win_focus", [&state] { state.win->focus(); });

    win->bindJson<json>("win_move", [&state](json j) -> Task<bool> {
        state.win->move(jget(j, "x", 0), jget(j, "y", 0));
        co_return true;
    });
    win->bindJson<json>("win_resize", [&state](json j) -> Task<bool> {
        state.win->resize(jget(j, "w", 800), jget(j, "h", 600));
        co_return true;
    });
    win->bindJson<json>("win_opacity", [&state](json j) -> Task<bool> {
        state.win->setOpacity(jget(j, "v", 1.0f));
        co_return true;
    });
    win->bindJson<json>("win_title", [&state](json j) -> Task<bool> {
        state.win->setTitle(jget(j, "title", std::string("HeliosView Master Demo")));
        co_return true;
    });
    win->bindJson<json>("win_progress", [&state](json j) -> Task<bool> {
        state.win->setProgress(jget(j, "v", 0), 100);
        co_return true;
    });
    win->bindJson<json>("win_info", [&state](json) -> Task<json> { co_return windowInfo(state); });
    win->bindJson<json>("win_workarea", [&state](json) -> Task<json> {
        helios::Rect r{};
        state.win->workArea(r);
        co_return json{{"x", r.x}, {"y", r.y}, {"w", r.width}, {"h", r.height}};
    });

    /* ================= Dialogs & System ================= */

    win->bindJson<json>("dlg_message", [&state](json j) -> Task<boost::json::value> {
        const auto r = helios::messageBox(state.win->nativeHandle(), helios::MessageBoxType::Question,
                                          helios::MessageBoxButtons::YesNo,
                                          jget(j, "title", std::string("HeliosView")).c_str(),
                                          jget(j, "msg", std::string("")).c_str());
        co_return boost::json::value{{"result", r == helios::MessageBoxResult::Yes ? "Yes" : "No"}};
    });
    win->bindJson<json>("dlg_folder", [&state](json) -> Task<boost::json::value> {
        std::string path;
        if (!helios::selectFolder(state.win->nativeHandle(), "Pick a folder", path))
            throw std::runtime_error("cancelled");
        state.lastPicked = path;
        co_return boost::json::value{{"path", path}};
    });
    win->bindJson<json>("dlg_openFiles", [&state](json) -> Task<json> {
        const auto files = helios::openFiles(state.win->nativeHandle(), "Pick files",
                                             "All files (*.*)|*.*", true);
        if (files.empty())
            throw std::runtime_error("cancelled");
        state.lastPicked = files.front();
        co_return json{{"count", files.size()}, {"paths", files}}; /* the full list */
    });
    win->bindJson<json>("dlg_saveFile", [&state](json) -> Task<boost::json::value> {
        std::string path;
        if (!helios::saveFile(state.win->nativeHandle(), "Save file", "Text files (*.txt)|*.txt",
                              "untitled.txt", path))
            throw std::runtime_error("cancelled");
        state.lastPicked = path;
        co_return boost::json::value{{"path", path}};
    });
    win->bindJson<json>("dlg_openUrl", [](json j) -> Task<bool> {
        const bool ok = helios::openUrl(jget(j, "url", std::string("https://example.com")));
        co_return ok;
    });
    win->bindJson<json>("dlg_clipboardSet", [](json j) -> Task<bool> {
        const bool ok = helios::clipboardSetText(
            jget(j, "text", std::string("Text from the HeliosView master demo")));
        co_return ok;
    });
    win->bindJson<json>("dlg_clipboardGet", [](json) -> Task<boost::json::value> {
        std::string text;
        if (!helios::clipboardGetText(text))
            throw std::runtime_error("clipboard has no text");
        co_return boost::json::value{{"text", text}};
    });
    win->bindJson<json>("dlg_cursor", [](json) -> Task<json> {
        int32_t x = 0, y = 0;
        helios::cursorPosition(x, y);
        co_return json{{"x", x}, {"y", y}};
    });
    win->bindJson<json>("dlg_showInFolder", [&state](json) -> Task<boost::json::value> {
        if (state.lastPicked.empty())
            throw std::runtime_error("pick a folder or file first");
        if (!helios::showInFolder(state.lastPicked))
            throw std::runtime_error("showInFolder failed");
        co_return boost::json::value{{"path", state.lastPicked}};
    });
    win->bindJson<json>("dlg_workareas", [](json) -> Task<json> {
        helios::Rect primary{}, atCursor{};
        helios::primaryWorkArea(primary);
        int32_t x = 0, y = 0;
        helios::cursorPosition(x, y);
        helios::screenWorkArea(x, y, atCursor);
        co_return json{{"primary", {primary.x, primary.y, primary.width, primary.height}},
                       {"atCursor", {atCursor.x, atCursor.y, atCursor.width, atCursor.height}}};
    });

    /* ================= WebView Bridge ================= */

    win->bindJson<json>("wv_navigate", [&state](json j) -> Task<boost::json::value> {
        const std::string url = jget(j, "url", std::string("https://example.com"));
        state.win->navigate(url.c_str());
        co_return boost::json::value{{"url", url}};
    });
    win->bindJson<json>("wv_home", [&state](json) -> Task<bool> {
        state.win->navigateHtml(state.page.c_str());
        co_return true;
    });
    win->bindJson<json>("wv_eval", [&state](json j) -> Task<boost::json::value> {
        const std::string script = jget(j, "script", std::string("1 + 1"));
        state.win->evalAsync(script.c_str(), [](int error, const char* result, void* userdata) {
            auto& s = *static_cast<DemoState*>(userdata);
            emit(s, "eval-result", {{"error", error}, {"result", result ? result : ""}});
        }, &state);
        co_return boost::json::value{{"note", "evalAsync dispatched; the result comes back on the events channel"}};
    });
    win->bindJson<json>("wv_insets", [&state](json) -> Task<bool> {
        state.insets = !state.insets;
        state.win->setWebViewInsets(state.insets ? 56 : 0, 0, 0, 0);
        co_return state.insets;
    });
    win->bindJson<json>("wv_mapLocal", [&state](json) -> Task<json> {
        const std::string cwd = std::filesystem::current_path().string();
        const int rc = state.win->mapLocalFolder("assets.local", cwd.c_str());
        co_return json{{"rc", rc}, {"host", "https://assets.local"}, {"folder", cwd}};
    });
    win->bindJson<json>("wv_broadcast", [&state](json j) -> Task<bool> {
        emit(state, "native-broadcast", {{"msg", jget(j, "msg", std::string("hello from native"))}});
        co_return true;
    });
    win->bindJson<json>("wv_add", [](json j) -> Task<int> {
        co_return jget(j, "a", 0) + jget(j, "b", 0);
    });
    win->bindJson<json>("wv_fail", [](json) -> Task<helios::JsonError> {
        co_return helios::JsonError{"error", "this is a deliberate reject demo"};
    });

    /* ================= Tray / Menu / Notifications ================= */

    win->bindJson<json>("tray_create", [&state](json) -> Task<bool> {
        if (!state.tray) {
            state.tray = std::make_shared<helios::Tray>(state.win->nativeHandle(), "HeliosView Master Demo");
            if (!state.tray->valid()) {
                state.tray.reset();
                throw std::runtime_error("tray creation failed (window not shown?)");
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
    win->bindJson<json>("tray_notify", [&state](json) -> Task<bool> {
        if (!state.tray)
            throw std::runtime_error("create the tray first");
        const bool ok = state.tray->notify("HeliosView", "Tray balloon notification", helios::NotifyIcon::Info);
        co_return ok;
    });
    win->bindJson<json>("tray_remove", [&state](json) -> Task<bool> {
        state.tray.reset();
        co_return true;
    });
    win->bindJson<json>("menu_show", [&state](json) -> Task<bool> {
        if (!state.menu) {
            auto menu = std::make_shared<helios::Menu>(state.win->nativeHandle());
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
                topmost->setChecked(state.topmost); // keep the checkmark in sync
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
    win->bindJson<json>("notify_show", [](json) -> Task<bool> {
        const bool ok = helios::notificationShow("HeliosView", "System toast notification (safe from any thread)");
        co_return ok;
    });

    /* ================= Multiple Windows & Quit ================= */

    win->bindJson<json>("win_new", [&state](json) -> Task<json> {
        const int n = ++state.extraCount;
        auto w = std::make_shared<helios::Window>(420, 300, std::format("Sub-window #{}", n).c_str());
        w->show();
        w->keyPressed.connect([&state, n](helios::KeyCode k) {
            emit(state, "subwindow-key", {{"n", n}, {"key", static_cast<int>(k)}});
        });
        w->closeRequested.connect([&state, n, raw = w.get()] {
            emit(state, "subwindow-closed", {{"n", n}});
            raw->close();  // close button does NOT auto-close; call close() here
        });
        state.extra.push_back(std::move(w));
        co_return json{{"n", n}, {"count", state.extra.size()}};
    });
    win->bindJson<json>("win_closeAll", [&state](json) -> Task<boost::json::value> {
        const int n = static_cast<int>(state.extra.size());
        state.extra.clear(); /* dtors close the matching native windows */
        co_return boost::json::value{{"closed", n}};
    });
    win->bindJson<json>("app_quit", [](json) -> Task<bool> {
        if (auto* a = helios::App::instance())
            a->quit();
        co_return true;
    });

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
