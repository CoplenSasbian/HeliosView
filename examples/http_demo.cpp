// HeliosView.Core example: async HTTP client (HeliosViewCore/Http.h).
// A GET and a POST round-trip through the loop thread pool; HTTPS/TLS,
// redirects and the system proxy are handled by the platform HTTP stack.
#include <HeliosViewCore/HeliosView.h>

#include <print>
#include <string>

// Coroutine usage: co_await the httpRequestAsync sender. Transport failures
// throw helios::IoError at the co_await point; HTTP-level failures (4xx/5xx)
// are delivered as a normal response with resp.ok() == false.
std::execution::task<void> runHttp(helios::Async& async)
{
    // 1) GET
    {
        auto resp = co_await helios::httpRequestAsync(
            async, helios::HttpRequest("GET", "https://www.baidu.com/"));
        std::println("[http] GET status: {}", resp.status);
        std::println("[http] GET headers: {}", resp.headers.dump());
        std::println("[http] GET body size: {}", resp.body.size());
        std::println("[http] GET content-type: {}",
                     resp.header("Content-Type"));
    }

    // 2) POST with a JSON body
    {
        helios::HttpRequest req("POST", "https://httpbin.org/post");
        req.setJsonBody({{"hello", "world"}, {"n", 42}});
        auto resp = co_await helios::httpRequestAsync(async, std::move(req));
        std::println("[http] POST status: {}", resp.status);
        if (resp.ok()) {
            const auto j = resp.json();
            std::println("[http] POST body: {}", j.dump());
        }
    }

    // 3) callback API
    {
        helios::httpRequest(async, helios::HttpRequest("GET", "https://www.baidu.com/"),
                            [](helios::HttpResponse resp) {
                                std::println("[http] callback status: {} body size: {}",
                                             resp.status, resp.body.size());
                            });
    }
}

int main()
{
    helios::Async async;
    try {
        std::execution::sync_wait(runHttp(async));
    } catch (const helios::IoError& e) {
        std::println("[main] http error code: {}", e.code());
        return 1;
    }
    std::println("[main] done");
    return 0;
}
