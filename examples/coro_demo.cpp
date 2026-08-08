// HeliosView.Core example: C++23 coroutines (std::execution::task).
// Write async chains with synchronous syntax: co_await a sender, errors are
// thrown at the co_await point (IoError).
// The namespace is std::execution (C++26 stdlib; on C++23 it falls back to
// stdexec via Execution.h).
#include <HeliosViewCore/HeliosView.h>

#include <exception>
#include <print>
#include <string>
#include <string_view>

// Coroutine functions return std::execution::task (itself a sender), usable with sync_wait / co_await
std::execution::task<void> runPipeline(helios::Async& async)
{
    const std::string path = "hv_io_test.bin";
    static char buf[256];

    // 1. Pool scheduler: after co_await we run on a worker thread
    co_await std::execution::schedule(async.get_scheduler());
    std::println("[coro] running on loop thread");

    // 2. File: open -> write -> read back (errors thrown at co_await)
    {
        helios::File f = co_await async.fileOpenAsync(path, true);
        std::println("[coro] file opened (write)");
        const char msg[] = "Hello, coroutines!\n";
        const uint32_t written = co_await async.fileWriteAsync(f, msg, sizeof(msg) - 1, 0);
        std::println("[coro] wrote {} bytes", written);
        async.fileClose(f);

        helios::File rf = co_await async.fileOpenAsync(path, false);
        const uint32_t got = co_await async.fileReadAsync(rf, buf, sizeof(buf), 0);
        std::println("[coro] read back {} bytes: {}", got, std::string_view(buf, got));
        async.fileClose(rf);
    }

    // 3. TCP: connect -> send -> single read
    {
        helios::TcpSocket sock = co_await async.tcpConnectAsync("example.com", 80);
        std::println("[coro] tcp connected");
        const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
        const uint32_t sent = co_await async.tcpWriteAsync(sock, req, sizeof(req) - 1);
        std::println("[coro] tcp sent {} bytes", sent);
        const uint32_t got = co_await async.tcpReadAsync(sock, buf, sizeof(buf));
        std::println("[coro] tcp got {} bytes:\n{}", got, std::string_view(buf, got));
        async.tcpClose(sock);
    }
}

int main()
{
    std::println("HeliosView {}", helios::version());
    helios::Async async;

    try {
        std::execution::sync_wait(runPipeline(async));
    } catch (const helios::IoError& e) {
        std::println("[main] io error: {}", e.code());
        return 1;
    }

    std::println("[main] done");
    return 0;
}
