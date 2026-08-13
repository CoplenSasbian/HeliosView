// HeliosView.Core example: WebView events, local resource mapping, folder dialog.
// Demonstrates the features added for UI work on top of the bridge:
//   - navigationStarting (with navigationStartingGate veto) / urlChanged /
//     titleChanged / navigationCompleted: WebView navigation events
//   - mapLocalFolder: maps a local folder to a virtual https://assets.local/ host
//     so the page can load images/files that are not part of the frontend
//   - helios::selectFolder: native folder-picker dialog, callable from the page
//     through a bindJson handler
#include <HeliosViewCore/HeliosView.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <string>

struct BrowseReq { std::string title; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BrowseReq, title)

int main()
{
    std::println("HeliosView {}", helios::version());

    auto app    = std::make_shared<helios::App>();
    auto window = std::make_shared<helios::WebViewWindow>(900, 640, L"HeliosView WebView Events Demo");
    window->show();
    window->createWebView();

    /* ---- navigation events ---- */

    // navigationStarting: fires before a navigation begins. Here a sync slot logs
    // the target URI.
    window->navigationStarting.connect([](std::string uri, bool redirect, bool user) {
        std::println("[nav-start] {} uri='{}' (redirect={}, user={})",
                     redirect ? "redirect" : "new-doc", uri, redirect, user);
    });

    // navigationStartingGate (optional): return true to cancel a navigation.
    // This example spies without cancelling (always returns false = allow).
    window->navigationStartingGate = [](const std::string& uri, bool redirect, bool user) {
        std::println("[gate] would-allow '{}' (redirect={}, user={})", uri, redirect, user);
        if (uri.starts_with("https://external.example/"))
            std::println("[gate]  -> blocked external link");
        return uri.starts_with("https://external.example/");
    };

    // urlChanged: the WebView's current URL changed.
    window->urlChanged.connect([](std::string uri, bool newDoc) {
        std::println("[url] '{}' (new-document={})", uri, newDoc);
    });

    // titleChanged: the page's <title> changed.
    window->titleChanged.connect([](std::string title) {
        std::println("[title] '{}'", title);
    });

    // navigationCompleted: know when the page is ready (and when it failed).
    window->navigationCompleted.connect([](int error) {
        if (error == 0)
            std::println("[navigation] page loaded OK (ready for eval)");
        else
            std::println("[navigation] page load FAILED, error={}", error);
    });

    /* ---- local resource mapping: serve a temp dir over https://assets.local/ ---- */

    const auto assetsDir = std::filesystem::temp_directory_path() / "heliosview_assets";
    std::filesystem::create_directories(assetsDir);
    {
        std::ofstream svg(assetsDir / "logo.svg");
        svg << "<svg xmlns='http://www.w3.org/2000/svg' width='96' height='96'>"
               "<rect width='96' height='96' rx='16' fill='#89b4fa'/>"
               "<text x='48' y='60' font-size='40' text-anchor='middle' fill='#1e1e2e'>HV</text></svg>";
    }
    window->mapLocalFolder("assets.local", assetsDir.string().c_str());
    std::println("[map] {} mapped to https://assets.local/", assetsDir.string());

    /* ---- native folder dialog exposed to the page through the bridge ---- */

    window->bindJson<BrowseReq>("browseFolder",
                                [win = window.get()](BrowseReq req) -> std::execution::task<helios::JsonResp<std::string>> {
                                    std::string path;
                                    const bool ok = helios::selectFolder(win->nativeHandle(),
                                                                          req.title.c_str(), path);
                                    std::println("[native] browseFolder(\"{}\") -> {} '{}'",
                                                 req.title, ok ? "OK" : "cancel", path);
                                    co_return helios::JsonResp<std::string>{ok ? "path" : "cancelled", ok ? path : ""};
                                });

    /* ---- a page that uses all of the above ---- */

    window->navigateHtml(
        "<html><head><meta charset='utf-8'><title>HeliosView 事件</title></head>"
        "<body style='font-family:system-ui;background:#1e1e2e;color:#cdd6f4;margin:0;"
        "height:100%;display:flex;flex-direction:column'>"
        "<div style='padding:12px'>"
        "<h2 style='margin:0 0 8px'>WebView 事件 + 本地资源 + 文件夹对话框</h2>"
        "<div style='display:flex;align-items:center;gap:8px;flex-wrap:wrap'>"
        "<img id='logo' src='https://assets.local/logo.svg' width='48' height='48'"
        " style='background:#111;border-radius:8px' onerror=\"this.style.visibility='hidden'\">"
        "<span>本地资源: https://assets.local/logo.svg</span>"
        "<button onclick=\"browse()\">浏览文件夹…</button>"
        "<button onclick=\"document.title += ' #' + (++window.__n||1)\">改标题</button>"
        "<span id='picked' style='opacity:.6'></span>"
        "</div></div>"
        "<div id='log' style='flex:1;overflow:auto;padding:0 12px 12px;"
        "font-family:ui-monospace,Consolas,monospace;font-size:13px;line-height:1.5'>"
        "<div style='opacity:.6'>页面加载后, 右侧日志记录 navigationStarting / urlChanged / titleChanged / navigationCompleted</div></div>"
        "<script>"
        "  const log = document.getElementById('log');"
        "  function line(txt) {"
        "    const d = document.createElement('div');"
        "    d.textContent = txt;"
        "    log.appendChild(d);"
        "    log.scrollTop = log.scrollHeight;"
        "  }"
        "  async function browse() {"
        "    line('call: browseFolder({\"title\":\"Select a folder\"})');"
        "    const r = await window.helios.call('browseFolder', {title: 'Select a folder'});"
        "    if (r.cancelled) line('result: cancelled');"
        "    else { line('result: ' + r.path); document.getElementById('picked').textContent = r.path; }"
        "  }"
        "</script>"
        "</body></html>");

    std::println("[main] entering UI loop...");
    return app->exec();
}
