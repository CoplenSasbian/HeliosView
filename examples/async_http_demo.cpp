// HeliosView.Core demo: Async (asio thread pool) + HttpClient.
//
// Console app (no WebView): exercises the pool — scheduler, timers (sender and
// callback styles), use_sender, loopback sockets — then the HTTP client:
// connection pooling against a local keep-alive server, http/https GET against
// example.com (TLS verified against cacert.pem, which sits next to this exe)
// and a POST against a local one-shot echo server.
//
// The example.com checks need internet; the rest runs locally.
#include <HeliosViewCore/Http.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace http = boost::beast::http;
using tcp = helios::asio::asio_impl::ip::tcp;

// Directory of the running exe (so "cacert.pem" resolves from any CWD).
static std::string exe_dir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const auto pos = p.find_last_of("\\/");
    return pos == std::string::npos ? "." : p.substr(0, pos);
#else
    return ".";
#endif
}

// One-shot HTTP echo server on the pool: accept, read a request, reply with
// the same body. Returns the received request body.
static std::execution::task<std::string> echo_server(helios::Async& async, tcp::acceptor& acc)
{
    namespace beast = boost::beast;
    beast::tcp_stream stream{async.get_executor()};
    co_await acc.async_accept(stream.socket(), helios::use_sender);

    beast::flat_buffer buffer;
    http::request<http::string_body> req;
    co_await http::async_read(stream, buffer, req, helios::use_sender);

    http::response<http::string_body> res;
    res.result(http::status::ok);
    res.version(req.version());
    res.set(http::field::content_type, "text/plain");
    res.body() = req.body();
    res.prepare_payload();
    co_await http::async_write(stream, res, helios::use_sender);
    co_return req.body();
}

// A keep-alive HTTP/1.1 server: serves `want` requests, reusing the connection
// for as many as the client sends on it. Returns the number of accepted
// connections (1 when the client pools).
static std::execution::task<int> keep_alive_server(helios::Async& async, tcp::acceptor& acc,
                                                   int want)
{
    namespace beast = boost::beast;
    int served = 0;
    int connections = 0;
    while (served < want) {
        beast::tcp_stream stream{async.get_executor()};
        co_await acc.async_accept(stream.socket(), helios::use_sender);
        ++connections;
        beast::flat_buffer buffer;
        while (served < want) {
            http::request<http::string_body> req;
            try {
                co_await http::async_read(stream, buffer, req, helios::use_sender);
            } catch (...) {
                break; // client closed: accept another connection
            }
            ++served;
            http::response<http::string_body> res;
            res.result(http::status::ok);
            res.version(req.version());
            res.set(http::field::content_type, "text/plain");
            res.body() = "reply-" + std::to_string(served);
            res.keep_alive(true);
            res.prepare_payload();
            co_await http::async_write(stream, res, helios::use_sender);
        }
    }
    co_return connections;
}

// Three sequential GETs with one Client: the connection pool should serve them
// all on a single connection.
static std::execution::task<int> three_gets(helios::http::Client client, std::string url)
{
    int status_sum = 0;
    for (int i = 0; i < 3; ++i)
        status_sum += (co_await client.get(url)).status;
    co_return status_sum;
}

int main()
{
    std::println("HeliosView Async + HTTP demo\n");

    helios::Async async{4};
    std::println("[pool] {} worker threads", async.available_parallelism());

    /* ---------- Async: scheduler + timers ---------- */

    (void)std::execution::sync_wait(std::execution::schedule(async.get_scheduler())
                              | std::execution::then([] {
                                    std::println("[schedule] ran on pool thread {}",
                                                 std::this_thread::get_id());
                                }));

    auto t0 = std::chrono::steady_clock::now();
    (void)std::execution::sync_wait(async.timer(std::chrono::milliseconds(200))).value();
    std::println("[timer] co_await async.timer(200ms) -> {} ms elapsed",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count());

    t0 = std::chrono::steady_clock::now();
    (void)std::execution::sync_wait(async.sleepAsync(std::chrono::milliseconds(100))).value();
    std::println("[timer] sleepAsync(100ms) -> {} ms elapsed",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count());

    bool fired = false;
    helios::asio::error_code ec;
    async.sleep(std::chrono::milliseconds(50),
                [&](helios::asio::error_code e) { ec = e; fired = true; });
    while (!fired)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::println("[timer] sleep(50ms, callback) fired, ec={}", ec ? ec.message() : "none");

    helios::asio::asio_impl::steady_timer t{async.get_executor(), std::chrono::milliseconds(30)};
    (void)std::execution::sync_wait(async.waitAsync(t)).value();
    std::println("[timer] waitAsync(user steady_timer + use_sender) ok");

    /* ---------- Async: loopback socket echo ---------- */

    tcp::acceptor acc{async.get_executor()};
    tcp::endpoint ep(helios::asio::asio_impl::ip::address_v4::loopback(), 0);
    acc.open(ep.protocol());
    acc.bind(ep);
    acc.listen(1);
    const auto local = acc.local_endpoint();

    tcp::socket client_sock{async.get_executor()};
    tcp::socket server_sock{async.get_executor()};
    auto accepted = async.acceptAsync(acc, server_sock);
    auto connected = async.connectAsync(client_sock, local);
    (void)std::execution::sync_wait(
        std::execution::when_all(std::move(accepted), std::move(connected)))
        .value();

    const std::string payload(4096, 'q');
    char buf[4096]{};
    auto w = async.writeAsync(client_sock, helios::asio::asio_impl::buffer(payload));
    auto r = async.readAsync(server_sock, helios::asio::asio_impl::buffer(buf, payload.size()));
    auto [nw, nr] =
        std::execution::sync_wait(std::execution::when_all(std::move(w), std::move(r))).value();
    std::println("[socket] loopback echo: {} bytes written, {} read, match={}", nw, nr,
                 std::memcmp(buf, payload.data(), payload.size()) == 0);

    /* ---------- HttpClient ---------- */

    helios::http::Client client{async, std::chrono::seconds(10), exe_dir() + "/cacert.pem"};
    std::println("\n[client] cacert.pem = {}/cacert.pem", exe_dir());

    /* Connection pool: three sequential GETs against a keep-alive server. */
    tcp::acceptor pool_acc{async.get_executor()};
    tcp::endpoint pool_ep(helios::asio::asio_impl::ip::address_v4::loopback(), 0);
    pool_acc.open(pool_ep.protocol());
    pool_acc.bind(pool_ep);
    pool_acc.listen(4);
    const std::string pool_url =
        "http://127.0.0.1:" + std::to_string(pool_acc.local_endpoint().port()) + "/pool";
    auto [connections, status_sum] =
        std::execution::sync_wait(std::execution::when_all(
                                      keep_alive_server(async, pool_acc, 3),
                                      three_gets(client, pool_url)))
            .value();
    std::println("[client] 3 GET {} -> status sum {}, {} connection(s), {} idle pooled", pool_url,
                 status_sum, connections, client.idle_connections());
    client.close_idle();
    std::println("[client] after close_idle(): {} idle", client.idle_connections());

    // POST against a local echo server (deterministic).
    tcp::acceptor post_acc{async.get_executor()};
    tcp::endpoint pep(helios::asio::asio_impl::ip::address_v4::loopback(), 0);
    post_acc.open(pep.protocol());
    post_acc.bind(pep);
    post_acc.listen(1);
    const auto post_local = post_acc.local_endpoint();
    const std::string post_url =
        "http://127.0.0.1:" + std::to_string(post_local.port()) + "/echo";
    auto server_task = echo_server(async, post_acc);
    auto post_task = client.post(post_url, "hello-echo", "text/plain");
    auto [echoed, post_resp] = std::execution::sync_wait(
        std::execution::when_all(std::move(server_task), std::move(post_task)))
                                   .value();
    std::println("[client] POST {} -> {} {}, echoed={}", post_url, post_resp.status,
                 post_resp.reason, post_resp.body);

    // http GET (internet)
    try {
        auto [r] = std::execution::sync_wait(client.get("http://example.com/")).value();
        std::println("[client] GET http://example.com/ -> {} {} ({} body bytes)", r.status,
                     r.reason, r.body.size());
    } catch (const std::exception& e) {
        std::println("[client] GET http failed: {}", e.what());
    }

    // https GET (internet, TLS verified)
    try {
        auto [r] = std::execution::sync_wait(client.get("https://example.com/")).value();
        std::println("[client] GET https://example.com/ -> {} {} ({} body bytes)", r.status,
                     r.reason, r.body.size());
        for (const auto& [k, v] : r.headers)
            if (k == "Content-Type" || k == "Server")
                std::println("          {}: {}", k, v);
    } catch (const std::exception& e) {
        std::println("[client] GET https failed: {}", e.what());
    }

    std::println("\ndone");
    return 0;
}
