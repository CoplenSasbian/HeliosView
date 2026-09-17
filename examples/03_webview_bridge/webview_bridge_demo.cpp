// ============================================================================
// HeliosView Example 03: Modern WebView & Bidirectional JS-Native Bridge
// ============================================================================
// A comprehensive master showcase of modern hybrid desktop development:
//   1. Hardware-Accelerated WebView2 with Frameless Custom Chrome
//   2. Type-Safe Bidirectional RPC Bridge (JS <-> C++ DTO via Boost.JSON)
//   3. Real-Time Telemetry & Event Streaming over BroadcastChannel
//   4. Native Window Shell Management (Size, Position, Opacity, State, Mica, Flash)
//   5. OS Integration (Dialogs, File/Folder Pickers, Explorer Reveal, Clipboard, Toast)
//   6. System Tray, Menus & Right-Click Context Menu Policy (Engine / DOM / Native)
//   7. Dynamic Multi-Window Management & Lifecycle
// ============================================================================

#include <HeliosViewCore/Dialogs.h>
#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/Notification.h>
#include <HeliosViewCore/System.h>
#include <HeliosViewCore/Tray.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
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

/* ---------- Request & Response DTOs for JS <-> Native Bridge ---------- */

struct SizeReq { int w; int h; };
BOOST_DESCRIBE_STRUCT(SizeReq, (), (w, h))

struct MoveReq { int x; int y; };
BOOST_DESCRIBE_STRUCT(MoveReq, (), (x, y))

struct OpacityReq { double v; }; /* 0.0 .. 1.0 */
BOOST_DESCRIBE_STRUCT(OpacityReq, (), (v))

struct TitleReq { std::string title; };
BOOST_DESCRIBE_STRUCT(TitleReq, (), (title))

struct ProgressReq { int v; }; /* 0 .. max */
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

struct MenuModeReq { std::string mode; };
BOOST_DESCRIBE_STRUCT(MenuModeReq, (), (mode))

struct BcMsg { std::string from; int n = 0; };
BOOST_DESCRIBE_STRUCT(BcMsg, (), (from, n))

/* ---------- Shared Demo State ---------- */

struct DemoState {
    std::shared_ptr<helios::Window> win;
    std::shared_ptr<helios::Tray> tray;
    std::shared_ptr<helios::Menu> menu;
    std::shared_ptr<helios::Menu> contextMenu;
    std::vector<std::shared_ptr<helios::Window>> extra;
    std::string page;
    std::string lastPicked;
    std::string lastLink;
    std::string menuMode = "engine"; /* engine | page | native */
    int extraCount = 0;
    bool topmost = false;
    bool resizable = true;
    bool insets = false;
};

/* Push a native event to the page's BroadcastChannel("events") */
void emit(DemoState& s, const char* ev, const json& payload = boost::json::object{})
{
    if (!s.win) return;
    json j = payload;
    j.as_object()["event"] = ev;
    s.win->broadcast("events", boost::json::serialize(j).c_str());
}

/* Window information inspector snapshot */
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

/* ---------- Embedded Master Showcase Web Page ---------- */

const char* kDemoPage = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HeliosView Master Showcase</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0f111a; color: #cdd6f4; overflow: hidden; }
  
  helios-window-title-bar {
    flex: 0 0 46px;
    height: 46px;
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 0 16px 0 20px;
    background: #181926;
    border-bottom: 1px solid #24273a;
    color: #cdd6f4;
    white-space: nowrap;
    user-select: none;
    -webkit-app-region: drag;
  }
  .title-brand { display: flex; align-items: center; gap: 8px; font-weight: 700; font-size: 14px; color: #cba6f7; }
  .title-desc { font-size: 12px; color: #7982a9; margin-left: 6px; }
  helios-window-controls { margin-left: auto; -webkit-app-region: no-drag; }

  .layout { display: flex; height: calc(100% - 46px); min-height: 0; }
  
  .sidebar {
    width: 410px;
    flex: 0 0 auto;
    overflow-y: auto;
    padding: 16px;
    background: #11121d;
    border-right: 1px solid #1f2335;
    user-select: none;
  }
  .sidebar::-webkit-scrollbar { width: 6px; }
  .sidebar::-webkit-scrollbar-thumb { background: #313244; border-radius: 3px; }

  .section-card {
    background: #181926;
    border: 1px solid #24273a;
    border-radius: 10px;
    padding: 12px 14px;
    margin-bottom: 14px;
  }
  .section-card h3 {
    margin-bottom: 10px;
    font-size: 11px;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.8px;
    color: #89b4fa;
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .row { display: flex; align-items: center; gap: 8px; margin: 6px 0; font-size: 12px; }
  .row .lab { flex: 0 0 26px; color: #a6adc8; font-size: 11px; font-weight: 600; }
  .row .val { flex: 0 0 42px; text-align: right; color: #89b4fa; font-size: 11px; font-family: ui-monospace, Consolas, monospace; }
  .row input[type=range] { flex: 1; accent-color: #89b4fa; cursor: pointer; height: 4px; }
  .row input[type=text] {
    flex: 1;
    min-width: 0;
    background: #0f111a;
    color: #cdd6f4;
    border: 1px solid #313244;
    border-radius: 6px;
    padding: 5px 8px;
    font-size: 12px;
  }
  .row input[type=text]:focus { outline: none; border-color: #89b4fa; }

  .btnrow { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; }
  button {
    flex: 0 0 auto;
    background: #24273a;
    color: #cdd6f4;
    border: 1px solid #313244;
    border-radius: 6px;
    padding: 5px 10px;
    font-size: 11.5px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }
  button:hover { background: #313244; border-color: #45475a; color: #fff; }
  button:active { background: #45475a; transform: translateY(1px); }
  button.primary { background: #89b4fa; color: #11111b; font-weight: 600; border-color: #89b4fa; }
  button.primary:hover { background: #b4befe; }

  .log-panel {
    flex: 1;
    display: flex;
    flex-direction: column;
    background: #0f111a;
    min-width: 0;
  }
  .log-header {
    flex: 0 0 40px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 16px;
    background: #141622;
    border-bottom: 1px solid #1f2335;
    font-size: 12px;
    font-weight: 600;
    color: #a6adc8;
  }
  #log {
    flex: 1;
    overflow-y: auto;
    padding: 14px 18px;
    font-family: ui-monospace, "Cascadia Code", Consolas, monospace;
    font-size: 12px;
    line-height: 1.65;
  }
  #log::-webkit-scrollbar { width: 6px; }
  #log::-webkit-scrollbar-thumb { background: #24273a; border-radius: 3px; }

  #log .call { color: #a6e3a1; }
  #log .ok   { color: #89b4fa; }
  #log .err  { color: #f38ba8; }
  #log .bc   { color: #cba6f7; }
  #log .dim  { color: #585b70; }

  #pagemenu {
    display: none;
    position: fixed;
    background: #24273a;
    border: 1px solid #45475a;
    border-radius: 8px;
    padding: 6px 0;
    font-size: 12px;
    box-shadow: 0 8px 24px rgba(0,0,0,0.5);
    min-width: 170px;
    z-index: 999;
  }
  #pagemenu .item {
    padding: 6px 14px;
    cursor: pointer;
    transition: background 0.1s;
  }
  #pagemenu .item:hover { background: #313244; color: #89b4fa; }
</style>
</head>
<body>

<helios-window-title-bar>
  <div class="title-brand">
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4"/></svg>
    <span>HeliosView Master Showcase</span>
  </div>
  <span class="title-desc">Modern Hybrid Bridge & OS Integration</span>
  <helios-window-controls></helios-window-controls>
</helios-window-title-bar>

<div class="layout">
  <div class="sidebar">
    <!-- 1. Window Shell Management -->
    <div class="section-card">
      <h3>1. Window Shell & Geometry</h3>
      <div class="row"><span class="lab">W</span><input type="range" id="sw" min="600" max="1920" value="1024"><span class="val" id="swv">1024</span></div>
      <div class="row"><span class="lab">H</span><input type="range" id="sh" min="450" max="1080" value="700"><span class="val" id="shv">700</span></div>
      <div class="btnrow"><button onclick="run('win_resize',{w:+sw.value,h:+sh.value})">Apply Size</button></div>
      <div class="row"><span class="lab">X</span><input type="range" id="sx" min="0" max="1600" value="100"><span class="val" id="sxv">100</span></div>
      <div class="row"><span class="lab">Y</span><input type="range" id="sy" min="0" max="1000" value="100"><span class="val" id="syv">100</span></div>
      <div class="btnrow"><button onclick="run('win_move',{x:+sx.value,y:+sy.value})">Apply Pos</button></div>
      <div class="row"><span class="lab">Op</span><input type="range" id="sop" min="20" max="100" value="100"><span class="val" id="sopv">100%</span></div>
      <div class="row"><input type="text" id="in_title" value="HeliosView Master Showcase"><button onclick="run('win_title',{title:in_title.value})">Set Title</button></div>
      <div class="btnrow">
        <button onclick="run('win_minimize',{})">Minimize</button>
        <button onclick="run('win_maximize',{})">Maximize</button>
        <button onclick="run('win_restore',{})">Restore</button>
        <button onclick="run('win_center',{})">Center</button>
        <button onclick="run('win_fullscreen',{})">Fullscreen</button>
        <button onclick="run('win_topmost',{})">Toggle Topmost</button>
        <button onclick="run('win_resizable',{})">Toggle Resizable</button>
        <button onclick="run('win_backdrop',{})">Mica + Dark</button>
        <button onclick="run('win_info',{})">Inspect Window Info</button>
      </div>
    </div>

    <!-- 2. Taskbar & Visual Effects -->
    <div class="section-card">
      <h3>2. Taskbar & Visual Effects</h3>
      <div class="row"><span class="lab">Pr</span><input type="range" id="spr" min="0" max="100" step="5" value="50"><span class="val" id="sprv">50%</span></div>
      <div class="btnrow">
        <button onclick="run('win_progress',{v:+spr.value})">Set Progress</button>
        <button onclick="run('win_progressIndeterminate',{})">Indeterminate</button>
        <button onclick="run('win_progressClear',{})">Clear Progress</button>
        <button onclick="run('win_flash',{})">Flash Window</button>
        <button onclick="run('win_flashUntilFocus',{})">Flash Until Focus</button>
      </div>
    </div>

    <!-- 3. Native Dialogs & System Queries -->
    <div class="section-card">
      <h3>3. Native Dialogs & System</h3>
      <div class="row"><input type="text" id="in_mbt" value="HeliosView Modal"><input type="text" id="in_mbm" value="Triggered from Web RPC!"></div>
      <div class="btnrow"><button onclick="run('dlg_message',{title:in_mbt.value,msg:in_mbm.value})">Message Box (Yes/No)</button></div>
      <div class="btnrow">
        <button onclick="run('dlg_folder',{})">Pick Folder</button>
        <button onclick="run('dlg_openFiles',{})">Open Files</button>
        <button onclick="run('dlg_saveFile',{})">Save File</button>
        <button onclick="run('dlg_showInFolder',{})">Reveal in Explorer</button>
        <button onclick="run('dlg_cursor',{})">Cursor Pos</button>
        <button onclick="run('dlg_workareas',{})">Work Areas</button>
      </div>
      <div class="row"><input type="text" id="in_clip" value="HeliosView Modern Clipboard Integration"><button onclick="run('dlg_clipboardSet',{text:in_clip.value})">Copy</button><button onclick="run('dlg_clipboardGet',{})">Paste/Read</button></div>
      <div class="row"><input type="text" id="in_url" value="https://github.com"><button onclick="run('dlg_openUrl',{url:in_url.value})">Open in Browser</button></div>
    </div>

    <!-- 4. WebView Bridge & RPC -->
    <div class="section-card">
      <h3>4. WebView Bridge & RPC</h3>
      <div class="row"><input type="text" id="in_nav" value="https://example.com"><button onclick="run('wv_navigate',{url:in_nav.value})">Navigate</button><button onclick="run('wv_home',{})">Back Home</button></div>
      <div class="row"><input type="text" id="in_eval" value="1 + 1 + Math.sqrt(16)"><button onclick="run('wv_eval',{script:in_eval.value})">Eval Async</button></div>
      <div class="row"><span class="lab">A</span><input type="range" id="sa" min="0" max="100" value="25"><span class="val" id="sav">25</span></div>
      <div class="row"><span class="lab">B</span><input type="range" id="sb" min="0" max="100" value="35"><span class="val" id="sbv">35</span></div>
      <div class="btnrow">
        <button class="primary" onclick="run('wv_add',{a:+sa.value,b:+sb.value})">Typed RPC Add</button>
        <button onclick="run('echo_obj',{timestamp:Date.now(),msg:'Hello C++'})">Raw JSON Echo</button>
        <button onclick="run('wv_fail',{})">Reject Demo</button>
      </div>
      <div class="row" style="margin-top:8px"><input type="text" id="in_bc" value="Greetings from C++ Backend"><button onclick="run('wv_broadcast',{msg:in_bc.value})">Native Broadcast</button><button onclick="bcSend()">Page Post</button></div>
      <div class="btnrow">
        <button onclick="run('wv_insets',{})">Toggle Top Insets</button>
        <button onclick="run('wv_mapLocal',{})">Map Local Folder</button>
      </div>
    </div>

    <!-- 5. System Tray & OS Notifications -->
    <div class="section-card">
      <h3>5. Tray, Menu & OS Toast</h3>
      <div class="btnrow">
        <button onclick="run('tray_create',{})">Create Tray</button>
        <button onclick="run('tray_notify',{})">Tray Balloon</button>
        <button onclick="run('tray_remove',{})">Remove Tray</button>
        <button onclick="run('menu_show',{})">Native Popup Menu</button>
        <button onclick="run('notify_show',{})">System Toast</button>
      </div>
    </div>

    <!-- 6. Right-Click Policy -->
    <div class="section-card">
      <h3>6. Right-Click Menu Policy</h3>
      <div class="btnrow">
        <button onclick="setMenuMode('engine')">Engine Default</button>
        <button onclick="setMenuMode('page')">Page DOM Menu</button>
        <button onclick="setMenuMode('native')">Native C++ Menu</button>
      </div>
      <div class="row" style="margin-top:6px"><span class="lab">Active:</span><span class="val" id="menumode" style="flex:1;text-align:left;color:#a6e3a1">engine</span></div>
      <div id="ctxbox" style="margin-top:8px;border:1px dashed #45475a;border-radius:6px;padding:8px 10px;font-size:12px;line-height:1.6;color:#a6adc8">
        Right-click in this box: test a <a href="https://example.com" style="color:#89b4fa">link</a>,
        selectable text, or this input: <input value="test input" style="width:75px;background:#0f111a;color:#cdd6f4;border:1px solid #313244;border-radius:4px;padding:2px 4px">
      </div>
    </div>

    <!-- 7. Multi-Window & Lifecycle -->
    <div class="section-card">
      <h3>7. Sub-Windows & App Lifecycle</h3>
      <div class="btnrow">
        <button onclick="run('win_new',{})">Spawn Sub-Window</button>
        <button onclick="run('win_closeAll',{})">Close All Sub-Windows</button>
        <button style="color:#f38ba8;border-color:#f38ba8" onclick="run('app_quit',{})">Quit Application</button>
      </div>
    </div>
  </div>

  <!-- Right Log Console -->
  <div class="log-panel">
    <div class="log-header">
      <span>Event Stream & RPC Execution Console</span>
      <button style="padding:2px 8px;font-size:11px" onclick="clearLog()">Clear</button>
    </div>
    <div id="log">
      <div class="dim">=== Ready. Every button invokes window.helios.call('&lt;method&gt;', payload). Live native signals stream over BroadcastChannel('events'). ===</div>
    </div>
  </div>
</div>

<div id="pagemenu">
  <div style="padding:4px 14px;opacity:0.6;font-size:11px;border-bottom:1px solid #313244;margin-bottom:4px">DOM Page Menu</div>
  <div class="item" onclick="pmenuItem('Custom Page Action 1')">Page Action 1</div>
  <div class="item" onclick="pmenuItem('Custom Page Action 2')">Page Action 2</div>
  <div class="item" onclick="pmenuItem('Copy Selected')">Copy Text</div>
</div>

<script>
  const log = document.getElementById('log');
  function line(txt, cls) {
    const d = document.createElement('div');
    if (cls) d.className = cls;
    d.textContent = txt;
    log.appendChild(d);
    log.scrollTop = log.scrollHeight;
  }
  function clearLog() {
    log.innerHTML = '<div class="dim">=== Log cleared ===</div>';
  }

  async function run(name, args) {
    line('> call: ' + name + (args && Object.keys(args).length ? ' ' + JSON.stringify(args) : ''), 'call');
    try {
      const r = await window.helios.call(name, args);
      line('< return: ' + JSON.stringify(r), 'ok');
    } catch(e) {
      line('! error: ' + (e ? (e.message || JSON.stringify(e)) : 'unknown error'), 'err');
    }
  }

  function bindSlider(id, out, suffix = '') {
    const el = document.getElementById(id), o = document.getElementById(out);
    const upd = () => o.textContent = el.value + suffix;
    el.addEventListener('input', upd);
    upd();
    return el;
  }

  const sw = bindSlider('sw','swv','px');
  const sh = bindSlider('sh','shv','px');
  const sx = bindSlider('sx','sxv','px');
  const sy = bindSlider('sy','syv','px');
  const sop = bindSlider('sop','sopv','%');
  sop.addEventListener('change', () => run('win_opacity', {v: +sop.value / 100}));
  const spr = bindSlider('spr','sprv','%');
  const sa = bindSlider('sa','sav');
  const sb = bindSlider('sb','sbv');

  // BroadcastChannel streaming
  const ev = new BroadcastChannel('events');
  ev.onmessage = e => line('[native event] ' + JSON.stringify(e.data), 'bc');

  const bc = new BroadcastChannel('status');
  function bcSend() {
    bc.postMessage({from: 'js-client', n: Math.floor(Math.random() * 1000)});
    line('[page] posted to native via BroadcastChannel("status")', 'bc');
  }

  let menuMode = 'engine';
  async function setMenuMode(m) {
    menuMode = m;
    document.getElementById('menumode').textContent = m;
    await run('menu_mode', {mode: m});
  }

  const pageMenu = document.getElementById('pagemenu');
  function pmenuItem(txt) {
    pageMenu.style.display = 'none';
    line('[dom menu] clicked: ' + txt, 'bc');
  }

  addEventListener('contextmenu', e => {
    if (menuMode !== 'page') return;
    e.preventDefault();
    pageMenu.style.left = Math.min(e.clientX, innerWidth - 180) + 'px';
    pageMenu.style.top = Math.min(e.clientY, innerHeight - 120) + 'px';
    pageMenu.style.display = 'block';
    line('[dom contextmenu] rendered page-drawn menu', 'bc');
  });
  addEventListener('click', () => pageMenu.style.display = 'none');
</script>
</body>
</html>)html";

} // namespace

int main()
{
    std::println("============================================================");
    std::println(" HeliosView Master Showcase (Example 03: Modern WebView)");
    std::println(" Version: {}", helios::version());
    std::println("============================================================");

    helios::enableDpiAwareness();
    helios::App::setAppId("com.heliosview.mastershowcase");
    std::println("[init] Notification subsystem: {}", helios::notificationInit() ? "active" : "standby");

    helios::App app;
    DemoState state;
    state.page = kDemoPage;
    state.win = std::make_shared<helios::Window>(1024, 700, "HeliosView Master Showcase",
                                                helios::WindowStyle::Frameless);
    auto* win = state.win.get();
    win->createWebView();
    win->show();

    // Stream native signals directly into the web page's BroadcastChannel("events")
    win->firstShown.connect([&state] { emit(state, "window-first-shown"); });
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

    // Native Context Menu & Interception Gate
    auto contextMenu = std::make_shared<helios::Menu>();
    contextMenu->addItem("Copy selection")->triggered.connect([&state] {
        emit(state, "context-menu-item", {{"item", "copy"}});
        state.win->eval("document.execCommand('copy')");
    });
    contextMenu->addItem("Open link in browser")->triggered.connect([&state] {
        emit(state, "context-menu-item", {{"item", "open-link"}, {"url", state.lastLink}});
        if (!state.lastLink.empty()) helios::openUrl(state.lastLink);
    });
    contextMenu->addSeparator();
    contextMenu->addItem("Inspect Window Info")->triggered.connect([&state] {
        emit(state, "context-menu-item", {{"item", "inspect-window"}});
        std::println("[menu] Window inspected");
    });
    state.contextMenu = contextMenu;

    win->contextMenuGate = [&state](const helios::ContextMenuInfo& info) {
        emit(state, "context-menu-request", {
            {"target", helios::toUint(info.target)},
            {"link", info.linkUrl},
            {"selection", info.selectionText},
            {"x", info.x},
            {"y", info.y}
        });
        if (state.menuMode != "native") return false;
        if (info.has(helios::ContextMenuTarget::Editable)) return false;

        state.lastLink = info.linkUrl;
        auto menu = state.contextMenu;
        auto* w = state.win.get();
        if (auto* a = helios::App::instance())
            a->postTask([menu, w] { menu->show(w->nativeHandle()); });
        else
            menu->show(w->nativeHandle());
        return true;
    };

    // Bridge registration function
    std::function<void(helios::Window*)> setupBridge;
    setupBridge = [&](helios::Window* w) {
        // Page -> Native status channel
        w->subscribeJson<BcMsg>("status", [&state](BcMsg msg) {
            emit(state, "native-echo", {{"received_from", msg.from}, {"n", msg.n}});
        });

        w->bindJson("menu_mode", [&state](MenuModeReq req) -> Task<json> {
            state.menuMode = req.mode;
            state.win->setContextMenuEnabled(req.mode != "page");
            co_return json{{"mode", req.mode}};
        });

        /* Window controls */
        w->bindJson("win_minimize", [&state]() -> Task<bool> { state.win->minimize(); co_return true; });
        w->bindJson("win_maximize", [&state]() -> Task<bool> { state.win->maximize(); co_return true; });
        w->bindJson("win_restore", [&state]() -> Task<bool> { state.win->restore(); co_return true; });
        w->bindJson("win_center", [&state]() -> Task<bool> { state.win->center(); co_return true; });
        w->bindJson("win_fullscreen", [&state]() -> Task<bool> {
            state.win->setFullscreen(!state.win->isFullscreen());
            co_return true;
        });
        w->bindJson("win_topmost", [&state]() -> Task<bool> {
            state.topmost = !state.topmost;
            state.win->setTopmost(state.topmost);
            co_return state.topmost;
        });
        w->bindJson("win_resizable", [&state]() -> Task<bool> {
            state.resizable = !state.resizable;
            state.win->setResizable(state.resizable);
            co_return state.resizable;
        });
        w->bindJson("win_progressIndeterminate", [&state]() -> Task<bool> {
            state.win->setProgressState(helios::ProgressState::Indeterminate);
            co_return true;
        });
        w->bindJson("win_progressClear", [&state]() -> Task<bool> { state.win->clearProgress(); co_return true; });
        w->bindJson("win_flash", [&state]() -> Task<bool> { state.win->flash(); co_return true; });
        w->bindJson("win_flashUntilFocus", [&state]() -> Task<bool> { state.win->flashUntilFocus(); co_return true; });
        w->bindJson("win_backdrop", [&state]() -> Task<bool> {
            state.win->setBackdrop(helios::Backdrop::Mica);
            state.win->setDarkMode(true);
            co_return true;
        });

        w->bindJson("win_resize", [&state](SizeReq req) -> Task<bool> {
            state.win->resize(req.w, req.h);
            co_return true;
        });
        w->bindJson("win_move", [&state](MoveReq req) -> Task<bool> {
            state.win->move(req.x, req.y);
            co_return true;
        });
        w->bindJson("win_opacity", [&state](OpacityReq req) -> Task<bool> {
            state.win->setOpacity(static_cast<float>(req.v));
            co_return true;
        });
        w->bindJson("win_title", [&state](TitleReq req) -> Task<bool> {
            state.win->setTitle(req.title.c_str());
            co_return true;
        });
        w->bindJson("win_progress", [&state](ProgressReq req) -> Task<bool> {
            state.win->setProgress(static_cast<uint32_t>(req.v), 100);
            co_return true;
        });
        w->bindJson("win_info", [&state]() -> Task<json> { co_return windowInfo(state); });

        /* Dialogs & OS integration */
        w->bindJson("dlg_message", [&state](MessageReq req) -> Task<json> {
            const auto r = helios::messageBox(state.win->nativeHandle(), helios::MessageBoxType::Question,
                                              helios::MessageBoxButtons::YesNo, req.title.c_str(), req.msg.c_str());
            co_return json{{"result", r == helios::MessageBoxResult::Yes ? "Yes" : "No"}};
        });
        w->bindJson("dlg_folder", [&state]() -> Task<json> {
            std::string path;
            if (!helios::selectFolder(state.win->nativeHandle(), "Select Folder", path))
                throw std::runtime_error("Folder selection cancelled");
            state.lastPicked = path;
            co_return json{{"path", path}};
        });
        w->bindJson("dlg_openFiles", [&state]() -> Task<json> {
            const auto files = helios::openFiles(state.win->nativeHandle(), "Select Files",
                                                 std::vector<helios::FileFilter>{{"All files", "*.*"}}, true);
            if (files.empty())
                throw std::runtime_error("File selection cancelled");
            state.lastPicked = files.front();
            co_return json{{"count", files.size()}, {"paths", files}};
        });
        w->bindJson("dlg_saveFile", [&state]() -> Task<json> {
            std::string path;
            if (!helios::saveFile(state.win->nativeHandle(), "Save File",
                                  std::vector<helios::FileFilter>{{"Text files", "txt"}, {"All files", "*.*"}},
                                  "untitled.txt", path))
                throw std::runtime_error("Save file cancelled");
            state.lastPicked = path;
            co_return json{{"path", path}};
        });
        w->bindJson("dlg_showInFolder", [&state]() -> Task<json> {
            if (state.lastPicked.empty())
                throw std::runtime_error("Pick a folder or file first");
            if (!helios::showInFolder(state.lastPicked))
                throw std::runtime_error("showInFolder failed");
            co_return json{{"path", state.lastPicked}};
        });
        w->bindJson("dlg_cursor", []() -> Task<json> {
            int32_t x = 0, y = 0;
            helios::cursorPosition(x, y);
            co_return json{{"x", x}, {"y", y}};
        });
        w->bindJson("dlg_workareas", []() -> Task<json> {
            helios::Rect primary{}, atCursor{};
            helios::primaryWorkArea(primary);
            int32_t x = 0, y = 0;
            helios::cursorPosition(x, y);
            helios::screenWorkArea(x, y, atCursor);
            co_return json{
                {"primary", {primary.x, primary.y, primary.width, primary.height}},
                {"atCursor", {atCursor.x, atCursor.y, atCursor.width, atCursor.height}}
            };
        });
        w->bindJson("dlg_openUrl", [](UrlReq req) -> Task<bool> {
            co_return helios::openUrl(req.url);
        });
        w->bindJson("dlg_clipboardSet", [](TextReq req) -> Task<bool> {
            co_return helios::clipboardSetText(req.text);
        });
        w->bindJson("dlg_clipboardGet", []() -> Task<json> {
            std::string text;
            if (!helios::clipboardGetText(text))
                throw std::runtime_error("Clipboard is empty or does not contain text");
            co_return json{{"text", text}};
        });

        /* WebView Bridge & RPC */
        w->bindJson("wv_navigate", [&state](UrlReq req) -> Task<json> {
            state.win->navigate(req.url.c_str());
            co_return json{{"url", req.url}};
        });
        w->bindJson("wv_home", [&state]() -> Task<bool> {
            state.win->navigateHtml(state.page.c_str());
            co_return true;
        });
        w->bindJson("wv_eval", [&state](ScriptReq req) -> Task<json> {
            state.win->evalAsync(req.script.c_str(), [](int error, const char* result, void* userdata) {
                auto& s = *static_cast<DemoState*>(userdata);
                emit(s, "eval-result", {{"error", error}, {"result", result ? result : ""}});
            }, &state);
            co_return json{{"status", "evalAsync dispatched to background execution"}};
        });
        w->bindJson("wv_insets", [&state]() -> Task<bool> {
            state.insets = !state.insets;
            state.win->setWebViewInsets(state.insets ? 56 : 0, 0, 0, 0);
            co_return state.insets;
        });
        w->bindJson("wv_mapLocal", [&state]() -> Task<json> {
            const std::string cwd = std::filesystem::current_path().string();
            const int rc = state.win->mapLocalFolder("assets.local", cwd.c_str());
            co_return json{{"rc", rc}, {"host", "https://assets.local"}, {"folder", cwd}};
        });
        w->bindJson("wv_broadcast", [&state](MsgReq req) -> Task<bool> {
            emit(state, "native-broadcast", {{"msg", req.msg}});
            co_return true;
        });
        w->bindJson("wv_add", [](AddReq req) -> Task<int> {
            co_return req.a + req.b;
        });
        w->bindJson("echo_obj", [](json arg) -> Task<json> {
            co_return json{{"echo", arg}, {"type", arg.is_object() ? "object" : "other"}};
        });
        w->bindJson("wv_fail", []() -> Task<helios::JsonError> {
            co_return helios::JsonError{"error", "This is a deliberate bridge reject demonstration."};
        });

        /* Tray & Notifications */
        w->bindJson("tray_create", [&state]() -> Task<bool> {
            if (!state.tray) {
                state.tray = std::make_shared<helios::Tray>("HeliosView Master Showcase");
                state.tray->leftClicked.connect([&state] { emit(state, "tray-left-click"); });
                state.tray->leftDoubleClicked.connect([&state] {
                    emit(state, "tray-double-click");
                    state.win->showNormal();
                });
                state.tray->rightClicked.connect([&state] {
                    emit(state, "tray-right-click");
                    state.win->showNormal();
                });
            }
            co_return true;
        });
        w->bindJson("tray_notify", [&state]() -> Task<bool> {
            if (!state.tray) throw std::runtime_error("Create the system tray icon first");
            co_return state.tray->notify("HeliosView Master Demo", "System tray balloon notification triggered!", helios::NotifyIcon::Info);
        });
        w->bindJson("tray_remove", [&state]() -> Task<bool> {
            state.tray.reset();
            co_return true;
        });
        w->bindJson("menu_show", [&state]() -> Task<bool> {
            if (!state.menu) {
                auto menu = std::make_shared<helios::Menu>();
                menu->addItem("Show / Restore")->triggered.connect([&state] { state.win->showNormal(); });
                menu->addItem("Maximize")->triggered.connect([&state] { state.win->maximize(); });
                auto* topmost = menu->addCheckItem("Toggle Topmost", state.topmost);
                topmost->triggered.connect([&state, topmost] {
                    state.topmost = !state.topmost;
                    state.win->setTopmost(state.topmost);
                    topmost->setChecked(state.topmost);
                });
                menu->addSeparator();
                menu->addItem("Open Documentation")->triggered.connect([] {
                    helios::openUrl("https://github.com");
                });
                menu->addSeparator();
                menu->addItem("Quit")->triggered.connect([] {
                    if (auto* a = helios::App::instance()) a->quit();
                });
                state.menu = std::move(menu);
            }
            state.menu->show(state.win->nativeHandle());
            co_return true;
        });
        w->bindJson("notify_show", []() -> Task<bool> {
            co_return helios::notificationShow("HeliosView Master Demo", "Windows Action Center notification triggered!");
        });

        /* Sub-Windows & Quit */
        w->bindJson("win_new", [&state]() -> Task<json> {
            const int n = ++state.extraCount;
            auto subWin = std::make_shared<helios::Window>(440, 320, std::format("HeliosView Sub-Window #{}", n).c_str());
            subWin->show();
            subWin->keyPressed.connect([&state, n](helios::KeyCode k) {
                emit(state, "subwindow-key", {{"n", n}, {"key", static_cast<int>(k)}});
            });
            subWin->closeRequested.connect([&state, n, raw = subWin.get()] {
                emit(state, "subwindow-closed", {{"n", n}});
                raw->close();
            });
            state.extra.push_back(std::move(subWin));
            co_return json{{"n", n}, {"open_windows", state.extra.size()}};
        });
        w->bindJson("win_closeAll", [&state]() -> Task<json> {
            const int count = static_cast<int>(state.extra.size());
            state.extra.clear();
            co_return json{{"closed", count}};
        });
        w->bindJson("app_quit", []() -> Task<bool> {
            if (auto* a = helios::App::instance()) a->quit();
            co_return true;
        });
    };

    setupBridge(win);

    win->closeRequested.connect([win] {
        std::println("[main] Window close requested -> closing application");
        win->close();
    });

    win->navigateHtml(state.page.c_str());
    return app.exec();
}
