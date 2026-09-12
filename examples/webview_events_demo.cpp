// HeliosView.Core example: WebView navigation events, local resources, native dialogs.
//
// The parts of a real app that surround the JS bridge:
//   - navigation events: navigationStarting (with the navigationStartingGate veto),
//     urlChanged, titleChanged, navigationCompleted - all on the UI thread
//   - mapLocalFolder + localUrl: serve a folder outside the frontend over a virtual
//     host, so the page can load images/files the bundle does not contain
//   - selectFolder: a native dialog exposed to the page through a bindJson handler
//   - setTitle: the page's <title> drives the native window title
//
// Try it: click "Browse folder..." (native dialog, result logged on the page), click
// "Rename" (the page's <title> retitles the window and comes back as a titleChanged
// event), click the external link (navigationStartingGate blocks it).
#include <HeliosViewCore/HeliosView.h>

#include <boost/describe.hpp>
#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string>

/* The page's folder-dialog request: {title: "..."} -> BrowseReq */
struct BrowseReq { std::string title; };
BOOST_DESCRIBE_STRUCT(BrowseReq, (), (title))

int main()
{
    std::println("HeliosView {} - WebView events / local resources demo", helios::version());

    auto app    = std::make_shared<helios::App>();
    auto window = std::make_shared<helios::WebViewWindow>(900, 640, "HeliosView WebView Events Demo");
    window->show();
    window->createWebView();

    /* ---- navigation events ---- */

    // navigationStarting: fires before a navigation begins (a sync slot logs the URI).
    window->navigationStarting.connect([](std::string uri, bool redirect, bool user) {
        std::println("[nav-start] {} uri='{}' (redirect={}, user={})",
                     redirect ? "redirect" : "new-doc", uri, redirect, user);
    });

    // navigationStartingGate: return true to CANCEL the navigation. Note that it is
    // consulted for the library's own page loads too, so it must only veto what the
    // app really wants to block (here: a demo "external" host).
    window->navigationStartingGate = [](const std::string& uri, bool redirect, bool user) {
        const bool blocked = uri.starts_with("https://external.example/");
        std::println("[gate] {} '{}' (redirect={}, user={})", blocked ? "BLOCK" : "allow", uri,
                     redirect, user);
        return blocked;
    };

    // urlChanged: the WebView's current URL changed (newDoc = a different document).
    window->urlChanged.connect([](std::string uri, bool newDoc) {
        std::println("[url] '{}' (new-document={})", uri, newDoc);
    });

    // titleChanged: the page's <title> changed - mirror it into the window title.
    window->titleChanged.connect([window](std::string title) {
        std::println("[title] '{}'", title);
        window->setTitle(title.c_str());
    });

    // navigationCompleted: the page is ready (error == 0) or failed.
    window->navigationCompleted.connect([](int error) {
        if (error == 0)
            std::println("[navigation] page loaded OK (ready for eval)");
        else
            std::println("[navigation] page load FAILED, error={}", error);
    });

    /* ---- local resource mapping: serve a folder over a virtual host ---- */

    const auto assetsDir = std::filesystem::temp_directory_path() / "heliosview_assets";
    std::filesystem::create_directories(assetsDir);
    {
        std::ofstream svg(assetsDir / "logo.svg");
        svg << "<svg xmlns='http://www.w3.org/2000/svg' width='96' height='96'>"
               "<rect width='96' height='96' rx='16' fill='#89b4fa'/>"
               "<text x='48' y='60' font-size='40' text-anchor='middle' fill='#1e1e2e'>HV</text></svg>";
    }
    /* mapLocalFolder(host, folder) makes <folder> reachable from the page; build the
     * URL with localUrl() - the engine's URL shape differs per platform
     * (https://<host>/... on Windows, a custom scheme elsewhere), so never hard-code it. */
    const int rc = window->mapLocalFolder("assets.local", assetsDir.string().c_str());
    const std::string logoUrl = window->localUrl("assets.local", "logo.svg");
    std::println("[map] rc={} {} -> {}", rc, assetsDir.string(), logoUrl);

    /* ---- a native dialog exposed to the page through the bridge ---- */

    // bindJson deduces BrowseReq from the handler; the folder picker is modal, so it
    // runs on the UI thread inside the (single-threaded) handler.
    window->bindJson("browseFolder", [win = window.get()](BrowseReq req) -> std::execution::task<boost::json::value> {
        std::string path;
        const bool ok = helios::selectFolder(win->nativeHandle(), req.title.c_str(), path);
        std::println("[native] browseFolder(\"{}\") -> {} '{}'", req.title, ok ? "OK" : "cancel", path);
        co_return boost::json::value{{ok ? "path" : "cancelled", ok ? path : ""}};
    });

    /* ---- a page that uses all of the above ---- */

    const std::string page =
        std::string("<html><head><meta charset='utf-8'><title>HeliosView Events</title></head>")
        + "<body style='font-family:system-ui;background:#1e1e2e;color:#cdd6f4;margin:0;"
        "height:100%;display:flex;flex-direction:column'>"
        "<div style='padding:12px'>"
        "<h2 style='margin:0 0 8px'>Navigation events + local resources + native dialog</h2>"
        "<div style='display:flex;align-items:center;gap:8px;flex-wrap:wrap'>"
        "<img id='logo' src='" + logoUrl + "' width='48' height='48'"
        " style='background:#111;border-radius:8px' onerror=\"this.style.visibility='hidden'\">"
        "<span style='opacity:.7'>mapped file: " + logoUrl + "</span>"
        "<button onclick=\"browse()\">Browse folder...</button>"
        "<button onclick=\"document.title += ' #' + (++window.__n||1)\">Rename (page title)</button>"
        "<a href='https://external.example/blocked' style='color:#f38ba8'>external link (blocked by the gate)</a>"
        "<span id='picked' style='opacity:.6'></span>"
        "</div></div>"
        "<div id='log' style='flex:1;overflow:auto;padding:0 12px 12px;"
        "font-family:ui-monospace,Consolas,monospace;font-size:13px;line-height:1.5'>"
        "<div style='opacity:.6'>navigationStarting / urlChanged / titleChanged / navigationCompleted "
        "are logged here as the page loads, retitles and navigates.</div></div>"
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
        "</body></html>";
    window->navigateHtml(page.c_str());

    /* The close button does NOT auto-close; the app decides. */
    window->closeRequested.connect([window] {
        std::println("[main] close requested -> closing");
        window->close();
    });

    std::println("[main] the page is loading; watch this console for the navigation events");
    return app->exec();
}
