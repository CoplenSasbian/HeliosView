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
#include <vector>

// Coroutine functions return std::execution::task (itself a sender), usable with sync_wait / co_await
std::execution::task<void> runPipeline(helios::Async& async)
{
    const std::string path = "hv_io_test.bin";
    static char buf[1024];

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

    // 3. TCP: connect -> send (Buffer variants) -> single read
    // The three writes concatenate into one valid HTTP/1.0 GET request.
    {
        helios::Socket sock = co_await async.connectAsync("example.com", 80);
        std::println("[coro] tcp connected");

        // Buffer::copy: allocate + copy from a std::string / any byte container.
        // Safe: the send owns its own copy, so the source may be freed immediately.
        const std::string head = "GET / HTTP/1.0\r\nHost: example.com\r\n";
        const uint32_t a = co_await async.writeAsync(sock, helios::Buffer::copy(head));
        std::println("[coro] tcp sent {} bytes (Buffer::copy)", a);

        // Buffer::ref: borrow a caller-owned buffer (zero-copy). The buffer MUST
        // outlive the writeAsync call — use it for long-lived members, not temporaries.
        std::vector<char> hdr = {'C', 'o', 'n', 'n', 'e', 'c', 't', 'i', 'o', 'n', ':', ' ', 'c', 'l', 'o', 's', 'e', '\r', '\n'};
        const uint32_t b = co_await async.writeAsync(sock, helios::Buffer::ref(hdr));
        std::println("[coro] tcp sent {} bytes (Buffer::ref, zero-copy)", b);

        // Buffer::alloc: allocate a writable buffer, fill it, then send (moved in).
        // This empty line ends the header block and makes the request valid.
        helios::Buffer tail = helios::Buffer::alloc(2);
        tail.data()[0] = '\r';
        tail.data()[1] = '\n';
        const uint32_t c = co_await async.writeAsync(sock, std::move(tail));
        std::println("[coro] tcp sent {} bytes (Buffer::alloc)", c);

        const uint32_t got = co_await async.readAsync(sock, buf, sizeof(buf));
        std::println("[coro] tcp got {} bytes:\n{}", got, std::string_view(buf, got));
        async.close(sock);
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
    async.stop();
    std::println("[main] done");
    return 0;
}
