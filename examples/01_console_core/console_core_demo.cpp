// ============================================================================
// HeliosView Example 01: Console Core & Async Services Demo
// ============================================================================
// Demonstrates the headless core infrastructure of HeliosView:
//   1. System Information & Environment Metrics (OS, paths, monitors)
//   2. Thread-Safe Signal & Slot Bus (HeliosViewCore/Signal.h)
//   3. High-Performance Thread Pool & Async Schedulers (HeliosViewCore/Async.h)
//   4. Coroutine Tasks (C++20/23 std::execution / P2300 senders)
//   5. Asynchronous HTTP/HTTPS Client & Keep-Alive Pooling (HeliosViewCore/Http.h)
// ============================================================================

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/Http.h>

#include <chrono>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace http = boost::beast::http;
using tcp = helios::asio::asio_impl::ip::tcp;

// ----------------------------------------------------------------------------
// 1. System Services Query
// ----------------------------------------------------------------------------
static void DemoSystemQueries() {
    std::cout << "\n==================================================\n";
    std::cout << " [1] System Information & Path Queries\n";
    std::cout << "==================================================\n";

    std::string os;
    if (helios::osVersion(os)) {
        std::cout << "  OS Version:     " << os << "\n";
    }
    std::cout << "  Engine Backend: " << heliosview_backend_name() << "\n";

    const std::pair<const char*, heliosview_system_path_kind_t> paths[] = {
        {"Home Directory", HELIOSVIEW_SYSTEM_PATH_HOME},
        {"Documents",      HELIOSVIEW_SYSTEM_PATH_DOCUMENTS},
        {"Downloads",      HELIOSVIEW_SYSTEM_PATH_DOWNLOADS},
        {"AppData",        HELIOSVIEW_SYSTEM_PATH_APPDATA},
        {"Temp",           HELIOSVIEW_SYSTEM_PATH_TEMP}
    };

    for (const auto& [label, kind] : paths) {
        std::string p;
        if (helios::systemPath(kind, p)) {
            std::cout << std::format("  {:<16} {}\n", label, p);
        }
    }

    helios::Rect workArea{};
    if (helios::primaryWorkArea(workArea)) {
        std::cout << std::format("  Primary Screen:  {}x{} at ({}, {})\n",
            workArea.width, workArea.height, workArea.x, workArea.y);
    }

    int32_t cx = 0, cy = 0;
    if (helios::cursorPosition(cx, cy)) {
        std::cout << std::format("  Cursor Position: ({}, {})\n", cx, cy);
    }
}

// ----------------------------------------------------------------------------
// 2. Signals & Slots (Event Bus)
// ----------------------------------------------------------------------------
static void DemoSignalSlot() {
    std::cout << "\n==================================================\n";
    std::cout << " [2] Type-Safe Signal & Slot Event Dispatching\n";
    std::cout << "==================================================\n";

    helios::Signal<std::string, int> telemetrySignal;

    // Slot 1: Simple lambda listener
    auto conn1 = telemetrySignal.connect([](const std::string& metric, int value) {
        std::cout << std::format("  [Slot 1 - Logger] Metric '{}' updated to {}\n", metric, value);
    });

    // Slot 2: Alert threshold monitor
    auto conn2 = telemetrySignal.connect([](const std::string& metric, int value) {
        if (value > 80) {
            std::cout << std::format("  [Slot 2 - Alert ] WARNING: Metric '{}' exceeded threshold! ({})\n", metric, value);
        }
    });

    std::cout << "  Emitting telemetry: CPU = 45\n";
    telemetrySignal("CPU", 45);

    std::cout << "  Emitting telemetry: Memory = 92\n";
    telemetrySignal("Memory", 92);

    // Disconnect Slot 2
    telemetrySignal.disconnect(conn2);
    std::cout << "  (Slot 2 disconnected)\n";

    std::cout << "  Emitting telemetry: GPU = 95 (Alert should NOT fire)\n";
    telemetrySignal("GPU", 95);
}

// ----------------------------------------------------------------------------
// 3. Thread Pool & Coroutine Task Execution
// ----------------------------------------------------------------------------
static std::execution::task<int> ComputeFibonacciAsync(helios::Async& async, int n) {
    // Hop from caller thread onto the worker thread pool
    co_await std::execution::schedule(async.get_scheduler());

    std::cout << std::format("  -> Worker thread [{}] computing Fibonacci({})\n",
        std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000, n);

    auto fib = [](auto self, int val) -> int {
        if (val <= 1) return val;
        return self(self, val - 1) + self(self, val - 2);
    };

    co_return fib(fib, n);
}

static void DemoAsyncThreadPool() {
    std::cout << "\n==================================================\n";
    std::cout << " [3] Async ThreadPool & Coroutine Scheduling\n";
    std::cout << "==================================================\n";

    helios::Async async(4); // 4-worker thread pool
    std::cout << "  Created thread pool with 4 workers.\n";

    std::cout << std::format("  Main thread [{}] dispatching async tasks...\n",
        std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000);

    // Run parallel coroutine tasks
    auto t1 = ComputeFibonacciAsync(async, 25);
    auto t2 = ComputeFibonacciAsync(async, 28);

    auto [res1] = std::this_thread::sync_wait(std::move(t1)).value();
    auto [res2] = std::this_thread::sync_wait(std::move(t2)).value();

    std::cout << std::format("  Result: Fib(25) = {}, Fib(28) = {}\n", res1, res2);

    // Delayed execution timer
    std::cout << "  Scheduling 200ms timer on thread pool...\n";
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sync_wait(
        async.timer(std::chrono::milliseconds(200))
    );
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << std::format("  Timer woke up after {}ms!\n", elapsed);
}

// ----------------------------------------------------------------------------
// 4. Asynchronous HTTP Client & Connection Pooling
// ----------------------------------------------------------------------------
static std::execution::task<std::string> RunEchoServer(helios::Async& async, tcp::acceptor& acc) {
    namespace beast = boost::beast;
    beast::tcp_stream stream{async.get_executor()};
    co_await acc.async_accept(stream.socket(), helios::use_sender);

    beast::flat_buffer buffer;
    http::request<http::string_body> req;
    co_await http::async_read(stream, buffer, req, helios::use_sender);

    http::response<http::string_body> res;
    res.result(http::status::ok);
    res.version(req.version());
    res.set(http::field::content_type, "application/json");
    res.body() = R"({"status":"ok","echo":")" + req.body() + R"("})";
    res.prepare_payload();
    co_await http::async_write(stream, res, helios::use_sender);

    co_return req.body();
}

static void DemoHttpClient() {
    std::cout << "\n==================================================\n";
    std::cout << " [4] Async HTTP Client & Local Service Loop\n";
    std::cout << "==================================================\n";

    helios::Async async(2);

    // Setup a temporary in-memory loopback HTTP server
    tcp::acceptor acceptor{async.get_executor()};
    tcp::endpoint ep(helios::asio::asio_impl::ip::address_v4::loopback(), 0);
    acceptor.open(ep.protocol());
    acceptor.bind(ep);
    acceptor.listen(1);
    const unsigned short port = acceptor.local_endpoint().port();
    const std::string echoUrl = std::format("http://127.0.0.1:{}/api/echo", port);

    std::cout << std::format("  Started loopback echo server on port {}\n", port);

    // Start server task and client task concurrently via when_all
    helios::http::Client client(async, std::chrono::seconds(5));
    std::cout << "  Client sending POST request with payload...\n";

    auto serverTask = RunEchoServer(async, acceptor);
    auto postTask = client.post(echoUrl, "HeliosView Core Packet 2026", "text/plain");

    auto [serverReceived, clientResp] = std::this_thread::sync_wait(
        std::execution::when_all(std::move(serverTask), std::move(postTask))
    ).value();

    std::cout << std::format("  Server processed payload: '{}'\n", serverReceived);
    std::cout << std::format("  Client received response: '{}' (HTTP {})\n", clientResp.body, clientResp.status);
}

// ----------------------------------------------------------------------------
// Main Entry
// ----------------------------------------------------------------------------
int main() {
    std::cout << "===========================================================\n";
    std::cout << " HeliosView " << helios::version() << " - Core & Async Services Showcase\n";
    std::cout << "===========================================================\n";

    try {
        DemoSystemQueries();
        DemoSignalSlot();
        DemoAsyncThreadPool();
        DemoHttpClient();

        std::cout << "\n===================================================\n";
        std::cout << " All Core Services executed successfully!\n";
        std::cout << "===================================================\n";
    } catch (const std::exception& e) {
        std::cerr << "[Error] Exception occurred: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
