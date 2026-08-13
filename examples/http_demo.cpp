// HeliosView.Core example: async HTTP client (HeliosViewCore/Http.h).
//
// A HttpClient issues requests over https:// (OpenSSL TLS + http-parser over the
// loop's async socket layer). This demo exercises JSON / XML responses,
// GET/POST/PUT/DELETE, query strings, custom headers, a 404, and both the sender
// (coroutine) and callback APIs against well-known public endpoints. Most
// requests use the convenience senders (get / post / put / del); [3] shows the
// full HttpRequest form. Each request is isolated, so a single unreachable/slow
// endpoint does not stop the rest.
#include <HeliosViewCore/HeliosView.h>

#include <future>
#include <print>
#include <string>
#include <utility>

// Co-await a request sender, translating a transport failure into a response with
// status 0 (and printing the error) so one bad endpoint doesn't abort the demo.
template <class Sender>
std::execution::task<helios::HttpResponse> safeAwait(std::string_view label, Sender&& s)
{
    try {
        co_return co_await std::forward<Sender>(s);
    } catch (const helios::IoError& e) {
        std::println("      ({}) transport error: {}", label, e.code());
        co_return helios::HttpResponse{};
    }
}

std::execution::task<void> runHttp(helios::Async& async)
{
    helios::HttpClient http(async);
    http.setTimeout(15000); // 15s per request

    // 1) JSON GET (convenience sender)
    {
        auto resp = co_await safeAwait("1", http.get("https://jsonplaceholder.typicode.com/todos/1"));
        std::println("[1] JSON GET    status: {}  content-type: {}",
                     resp.status, resp.header("Content-Type"));
        if (resp.ok())
            std::println("[1]              json: {}", resp.json().dump());
    }

    // 2) XML GET (body delivered raw)
    {
        auto resp = co_await safeAwait("2", http.get("https://www.w3schools.com/xml/note.xml"));
        std::println("[2] XML GET     status: {}  content-type: {}",
                     resp.status, resp.header("Content-Type"));
        if (resp.ok())
            std::println("[2]              xml:\n{}", resp.body);
    }

    // 3) JSON POST with a body + custom header (full HttpRequest form)
    {
        helios::HttpRequest req("POST", "https://postman-echo.com/post");
        req.setJsonBody({{"hello", "world"}, {"n", 42}});
        req.addHeader("X-Helios", "demo");
        auto resp = co_await safeAwait("3", http.requestAsync(std::move(req)));
        std::println("[3] JSON POST   status: {}", resp.status);
        if (resp.ok())
            std::println("[3]              echoed json: {}", resp.json()["json"].dump());
    }

    // 4) GET with a query string (echoed back)
    {
        auto resp = co_await safeAwait("4", http.get("https://postman-echo.com/get?name=helios&n=7"));
        std::println("[4] GET + query status: {}", resp.status);
        if (resp.ok())
            std::println("[4]              args: {}", resp.json()["args"].dump());
    }

    // 5) PUT with an XML body (convenience: body + Content-Type)
    {
        auto resp = co_await safeAwait("5", http.put(
            "https://postman-echo.com/put",
            "<note><to>helios</to><from>demo</from></note>", "application/xml"));
        std::println("[5] XML PUT     status: {}", resp.status);
        if (resp.ok())
            std::println("[5]              data: {}", resp.json()["data"].dump());
    }

    // 6) DELETE
    {
        auto resp = co_await safeAwait("6", http.del("https://postman-echo.com/delete"));
        std::println("[6] DELETE      status: {}", resp.status);
    }

    // 7) non-2xx handling (404)
    {
        auto resp = co_await safeAwait("7", http.get("https://postman-echo.com/status/404"));
        std::println("[7] 404         status: {} (ok = {})", resp.status, resp.ok());
    }

    // 8) callback API (waited for completion)
    {
        std::promise<helios::HttpResponse> done;
        auto fut = done.get_future();
        http.request(helios::HttpRequest("GET", "https://jsonplaceholder.typicode.com/posts/1"),
                     [&done](helios::HttpResponse resp) { done.set_value(std::move(resp)); });
        auto resp = fut.get();
        std::println("[8] callback    status: {}  title: {}",
                     resp.status, resp.json()["title"].dump());
    }
}

int main()
{
    helios::Async async;
    try {
        std::execution::sync_wait(runHttp(async));
    } catch (const std::exception& e) {
        std::println("[main] error: {}", e.what());
        return 1;
    }
    std::println("[main] done");
    return 0;
}
