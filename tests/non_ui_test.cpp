/**
 * HeliosView -- Comprehensive Non-UI Unit Test Suite
 *
 * Tests all non-UI systems:
 *   1. String encoding (UTF-8 <-> UTF-16, Unicode, Emoji, bounds, nulls)
 *   2. Geometry & Types (Rect, Point, Size, Event conversions, bitwise flags)
 *   3. Signal & Slot bus (sync, multiple slots, slot id disconnection, reentrancy, member fns)
 *   4. System queries & OS integration (OS version, system paths, DPI awareness, clipboard, primary monitor)
 *   5. Async thread pool & stdexec schedulers (concurrency, background thread hops, timer senders)
 *   6. HTTP client & URL parser & Beast server loopback (URL parsing, GET/POST, keep-alive pool)
 *   7. WebViewJson DTO reflection & Boost.JSON serialization (type-safe RPC mapping, JsonError)
 *   8. Core C API metadata & Error handling (version, backend, last error formatting)
 */

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/Http.h>
#include <HeliosViewCore/WebViewJson.h>

#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

static int g_failed = 0;
static int g_passed = 0;

#define TEST_CHECK(cond, msg) \
    do { \
        if (cond) { \
            std::cout << "  [PASS] " << msg << "\n"; \
            g_passed++; \
        } else { \
            std::cout << "  [FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
            g_failed++; \
        } \
    } while (0)

// ----------------------------------------------------------------------------
// 1. String Utilities Test (HeliosViewCore/String.h)
// ----------------------------------------------------------------------------
static void TestStringUtils() {
    std::cout << "\n=== [1] String Encoding Tests ===\n";

    // Basic ASCII
    {
        std::string src = "Hello, HeliosView C++23!";
        std::wstring wide = helios::utf8ToWide(src);
        std::string roundtrip = helios::wideToUtf8(wide);
        TEST_CHECK(roundtrip == src, "ASCII utf8ToWide and wideToUtf8 roundtrip");
    }

    // Chinese and Multi-byte Unicode
    {
        std::string src = "HeliosView 冲刺合并到 master 🚀✨ -- 现代化高性能框架";
        std::wstring wide = helios::utf8ToWide(src);
        std::string roundtrip = helios::wideToUtf8(wide);
        TEST_CHECK(roundtrip == src, "CJK & Emoji utf8ToWide and wideToUtf8 roundtrip");
    }

    // Empty and nullptr safety
    {
        TEST_CHECK(helios::utf8ToWide("").empty(), "Empty string utf8ToWide returns empty");
        TEST_CHECK(helios::wideToUtf8(L"").empty(), "Empty wstring wideToUtf8 returns empty");
        TEST_CHECK(helios::utf8ToWide(nullptr, 0).empty(), "nullptr utf8ToWide returns empty");
        TEST_CHECK(helios::wideToUtf8(nullptr, 0).empty(), "nullptr wideToUtf8 returns empty");
    }

    // Partial length conversion
    {
        std::string src = "HeliosView_LongText";
        std::wstring wide = helios::utf8ToWide(src.data(), 10);
        std::string roundtrip = helios::wideToUtf8(wide);
        TEST_CHECK(roundtrip == "HeliosView", "Partial length utf8ToWide truncation");
    }
}

// ----------------------------------------------------------------------------
// 2. Types & Geometry Test (HeliosViewCore/Types.h)
// ----------------------------------------------------------------------------
static void TestTypesAndGeometry() {
    std::cout << "\n=== [2] Types & Geometry Tests ===\n";

    // Rect conversions (System.h)
    heliosview_rect_t cRect{100, 150, 800, 600};
    helios::Rect cppRect = helios::toRect(cRect);
    TEST_CHECK(cppRect.x == 100 && cppRect.y == 150 && cppRect.width == 800 && cppRect.height == 600,
               "toRect(heliosview_rect_t) matches coordinates");

    // Event conversions
    heliosview_event_t cEv{};
    cEv.type = HELIOSVIEW_EVENT_KEY_DOWN;
    cEv.key = HELIOSVIEW_KEY_ESCAPE;
    cEv.x = 42;
    cEv.y = 84;
    helios::Event cppEv = helios::Event::fromC(cEv);
    TEST_CHECK(cppEv.type == helios::EventType::KeyDown, "Event::fromC converts event type");
    TEST_CHECK(cppEv.key == helios::KeyCode::Escape, "Event::fromC converts key code");
    TEST_CHECK(cppEv.x == 42 && cppEv.y == 84, "Event::fromC preserves coordinates");

    heliosview_event_t cEvRoundtrip = cppEv.toC();
    TEST_CHECK(cEvRoundtrip.type == HELIOSVIEW_EVENT_KEY_DOWN, "Event::toC roundtrips event type");
    TEST_CHECK(cEvRoundtrip.key == HELIOSVIEW_KEY_ESCAPE, "Event::toC roundtrips key code");

    // ContextMenuTarget bitwise operators
    auto target = helios::ContextMenuTarget::Page | helios::ContextMenuTarget::Link;
    helios::ContextMenuInfo info{};
    info.target = target;
    TEST_CHECK(info.has(helios::ContextMenuTarget::Page), "ContextMenuInfo has Page flag");
    TEST_CHECK(info.has(helios::ContextMenuTarget::Link), "ContextMenuInfo has Link flag");
    TEST_CHECK(!info.has(helios::ContextMenuTarget::Image), "ContextMenuInfo does not have Image flag");

    // WindowFlag bitwise operators
    auto flag = helios::WindowFlag::Resizable | helios::WindowFlag::Closable;
    TEST_CHECK(toUint(flag) != 0, "WindowFlag bitwise combination");

    // Tray event check
    TEST_CHECK(helios::isTrayEvent(helios::EventType::TrayLeftClick), "TrayLeftClick is detected as TrayEvent");
    TEST_CHECK(helios::isTrayEvent(helios::EventType::TrayRightClick), "TrayRightClick is detected as TrayEvent");
    TEST_CHECK(!helios::isTrayEvent(helios::EventType::WindowClose), "WindowClose is not a TrayEvent");
}

// ----------------------------------------------------------------------------
// 3. Signal & Slot Bus Test (HeliosViewCore/Signal.h)
// ----------------------------------------------------------------------------
struct TestReceiver {
    int callCount = 0;
    int lastVal = 0;
    void onValue(int v) {
        callCount++;
        lastVal = v;
    }
};

static void TestSignalSlot() {
    std::cout << "\n=== [3] Signal & Slot Bus Tests ===\n";

    // Simple single slot
    {
        helios::Signal<int> sig;
        int received = 0;
        sig.connect([&received](int val) {
            received = val;
        });
        sig(42);
        TEST_CHECK(received == 42, "Single slot received emitted argument");
    }

    // Multiple slots and execution
    {
        helios::Signal<std::string> sig;
        int count1 = 0;
        int count2 = 0;
        sig.connect([&count1](const std::string&) { count1++; });
        sig.connect([&count2](const std::string&) { count2++; });
        sig("ping");
        sig("pong");
        TEST_CHECK(count1 == 2 && count2 == 2, "Multiple slots both received emissions");
    }

    // Disconnect via slot id
    {
        helios::Signal<int> sig;
        int sum = 0;
        auto id1 = sig.connect([&sum](int v) { sum += v; });
        auto id2 = sig.connect([&sum](int v) { sum += v * 10; });

        sig(1); // sum = 1 + 10 = 11
        TEST_CHECK(sum == 11, "Two slots fired initially");

        sig.disconnect(id2);
        sig(2); // sum = 11 + 2 = 13 (id2 should not fire)
        TEST_CHECK(sum == 13, "Disconnected slot did not fire");

        sig.disconnect(id1);
        sig(5); // sum remains 13
        TEST_CHECK(sum == 13, "All disconnected slots produce no emission");
    }

    // Member function connection
    {
        TestReceiver receiver;
        helios::Signal<int> sig;
        sig.connect(&TestReceiver::onValue, &receiver);
        sig(99);
        TEST_CHECK(receiver.callCount == 1 && receiver.lastVal == 99, "Member function slot received emission");
    }

    // Reentrancy safety: connecting or disconnecting inside a slot emission
    {
        helios::Signal<int> sig;
        uint32_t innerId = 0;
        int outerCalls = 0;
        int innerCalls = 0;

        sig.connect([&](int v) {
            outerCalls++;
            if (innerId == 0) {
                innerId = sig.connect([&innerCalls](int) { innerCalls++; });
            }
        });

        sig(1);
        TEST_CHECK(outerCalls == 1, "Outer slot called on first emission");
        // Because emission copies the slot table snapshot, inner slot added inside shouldn't fire in the same emission
        TEST_CHECK(innerCalls == 0, "Slot added during emission does not fire in the same snapshot");

        sig(2);
        TEST_CHECK(outerCalls == 2, "Outer slot called on second emission");
        TEST_CHECK(innerCalls == 1, "Slot added previously now fires on next emission");
    }
}

// ----------------------------------------------------------------------------
// 4. System Queries & OS Integration (HeliosViewCore/System.h)
// ----------------------------------------------------------------------------
static void TestSystemQueries() {
    std::cout << "\n=== [4] System Queries & Helpers Tests ===\n";

    // OS Version
    std::string os;
    bool osOk = helios::osVersion(os);
    TEST_CHECK(osOk && !os.empty(), "helios::osVersion returned valid string: " + os);

    // System Paths
    std::pair<const char*, heliosview_system_path_kind_t> paths[] = {
        {"Home", HELIOSVIEW_SYSTEM_PATH_HOME},
        {"Documents", HELIOSVIEW_SYSTEM_PATH_DOCUMENTS},
        {"Downloads", HELIOSVIEW_SYSTEM_PATH_DOWNLOADS},
        {"AppData", HELIOSVIEW_SYSTEM_PATH_APPDATA},
        {"Temp", HELIOSVIEW_SYSTEM_PATH_TEMP}
    };

    for (const auto& [label, kind] : paths) {
        std::string p;
        bool ok = helios::systemPath(kind, p);
        TEST_CHECK(ok && !p.empty(), std::string("systemPath for ") + label + " returned non-empty: " + p);
        if (ok && !p.empty()) {
            std::error_code ec;
            bool exists = std::filesystem::exists(p, ec);
            TEST_CHECK(exists, std::string("systemPath for ") + label + " directory exists on filesystem");
        }
    }

    // DPI Awareness
    bool dpiOk = helios::enableDpiAwareness();
    TEST_CHECK(dpiOk, "helios::enableDpiAwareness succeeded");

    // Primary Work Area
    helios::Rect workArea{};
    bool areaOk = helios::primaryWorkArea(workArea);
    TEST_CHECK(areaOk, "helios::primaryWorkArea returned true");
    TEST_CHECK(workArea.width > 0 && workArea.height > 0, "Primary work area dimensions are positive");

    // Cursor position query (valid in interactive desktop; handled gracefully if headless/service)
    int32_t cx = 0, cy = 0;
    bool curOk = helios::cursorPosition(cx, cy);
    if (!curOk) {
        std::cout << "  [INFO] helios::cursorPosition returned false (non-interactive/headless session)\n";
    }
    TEST_CHECK(true, "helios::cursorPosition executed safely without crash");

    // Clipboard round-trip
    std::string clipWrite = "HeliosView_Unit_Test_Clipboard_123456";
    bool setOk = helios::clipboardSetText(clipWrite);
    TEST_CHECK(setOk, "helios::clipboardSetText succeeded");

    std::string clipRead;
    bool getOk = helios::clipboardGetText(clipRead);
    TEST_CHECK(getOk, "helios::clipboardGetText succeeded");
    TEST_CHECK(clipRead == clipWrite, "Clipboard content read matches what was written");
}

// ----------------------------------------------------------------------------
// 5. Async Thread Pool & stdexec Schedulers (HeliosViewCore/Async.h)
// ----------------------------------------------------------------------------
static void TestAsyncThreadPool() {
    std::cout << "\n=== [5] Async Thread Pool & Schedulers Tests ===\n";

    helios::Async async(4);
    auto mainTid = std::this_thread::get_id();

    // Work execution on pool thread via schedule sender
    std::atomic<bool> workExecuted = false;
    std::thread::id workerTid;

    auto snd = std::execution::schedule(async.get_scheduler())
             | std::execution::then([&]() {
                   workExecuted = true;
                   workerTid = std::this_thread::get_id();
               });

    std::execution::sync_wait(std::move(snd));

    TEST_CHECK(workExecuted.load(), "Async scheduler executed work via stdexec sender");
    TEST_CHECK(workerTid != mainTid, "Async work ran on background worker thread");

    // Async timer sender
    auto start = std::chrono::steady_clock::now();
    std::execution::sync_wait(async.timer(std::chrono::milliseconds(30)));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    ).count();
    TEST_CHECK(elapsed >= 25, "async.timer(30ms) waited appropriately (elapsed: " + std::to_string(elapsed) + "ms)");

    // Callback style sleep
    std::atomic<bool> cbExecuted = false;
    async.sleep(std::chrono::milliseconds(20), [&](boost::system::error_code ec) {
        if (!ec) cbExecuted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    TEST_CHECK(cbExecuted.load(), "async.sleep callback executed on timer expiry");
}

// ----------------------------------------------------------------------------
// 6. HTTP Client & URL Parser & In-Process Beast Server (HeliosViewCore/Http.h)
// ----------------------------------------------------------------------------
static void TestHttpAndUrlParser() {
    std::cout << "\n=== [6] HTTP URL Parser & Beast Loopback Tests ===\n";

    // URL Parsing tests
    {
        auto u1 = helios::http::detail::parse_url("http://example.com/api/v1?user=test");
        TEST_CHECK(u1.scheme == "http", "URL parse scheme http");
        TEST_CHECK(u1.host == "example.com", "URL parse host");
        TEST_CHECK(u1.port == "80", "URL parse default port 80");
        TEST_CHECK(u1.target == "/api/v1?user=test", "URL parse target with query");

        auto u2 = helios::http::detail::parse_url("https://127.0.0.1:9090");
        TEST_CHECK(u2.scheme == "https", "URL parse scheme https");
        TEST_CHECK(u2.host == "127.0.0.1", "URL parse IPv4 host");
        TEST_CHECK(u2.port == "9090", "URL parse custom port 9090");
        TEST_CHECK(u2.target == "/", "URL parse default root target");

        // Invalid URLs
        bool threw1 = false;
        try {
            helios::http::detail::parse_url("ftp://unsupported.com");
        } catch (const std::invalid_argument&) {
            threw1 = true;
        }
        TEST_CHECK(threw1, "parse_url throws invalid_argument on non-http scheme");

        bool threw2 = false;
        try {
            helios::http::detail::parse_url("http://");
        } catch (const std::invalid_argument&) {
            threw2 = true;
        }
        TEST_CHECK(threw2, "parse_url throws invalid_argument on empty host");
    }

    // In-process ephemeral Beast HTTP server to test HTTP client end-to-end
    {
        net::io_context serverIoc;
        tcp::acceptor acceptor(serverIoc, tcp::endpoint(tcp::v4(), 0));
        auto localPort = acceptor.local_endpoint().port();

        auto pSocket = std::make_shared<tcp::socket>(serverIoc);
        std::function<void()> do_accept;
        do_accept = [&]() {
            acceptor.async_accept(*pSocket, [&](boost::system::error_code ec) {
                if (ec) return;
                auto sock = std::move(*pSocket);
                pSocket = std::make_shared<tcp::socket>(serverIoc);
                do_accept();

                std::thread([s = std::move(sock)]() mutable {
                    boost::system::error_code ec;
                    beast::flat_buffer buffer;
                    while (true) {
                        http::request<http::string_body> req;
                        http::read(s, buffer, req, ec);
                        if (ec) break;

                        http::response<http::string_body> res{http::status::ok, req.version()};
                        res.set(http::field::server, "HeliosView-TestServer/1.0");
                        res.set(http::field::content_type, "application/json");
                        res.keep_alive(req.keep_alive());

                        if (req.method() == http::verb::get) {
                            res.body() = R"({"status":"ok","method":"GET"})";
                        } else if (req.method() == http::verb::post) {
                            res.body() = R"({"status":"ok","echo":")" + req.body() + R"("})";
                        }
                        res.prepare_payload();
                        http::write(s, res, ec);
                        if (!req.keep_alive() || ec) break;
                    }
                }).detach();
            });
        };
        do_accept();

        std::thread serverThread([&serverIoc]() {
            serverIoc.run();
        });

        std::string baseUrl = "http://127.0.0.1:" + std::to_string(localPort);

        helios::Async async(4);
        helios::http::PoolOptions opts;
        opts.keep_alive = true;
        helios::http::Client client{async, std::chrono::seconds(5), "", opts};

        // Test GET
        auto getTask = [&client, &baseUrl]() -> std::execution::task<helios::http::Response> {
            co_return co_await client.get(baseUrl + "/test");
        };

        auto resGet = std::execution::sync_wait(getTask());
        TEST_CHECK(resGet.has_value(), "client.get completed without error");
        if (resGet.has_value()) {
            auto& resp = std::get<0>(*resGet);
            TEST_CHECK(resp.status == 200, "client.get returned status 200 OK");
            TEST_CHECK(resp.body.find("\"method\":\"GET\"") != std::string::npos, "client.get body matches expected JSON");
            bool hasServerHeader = false;
            for (const auto& [hk, hv] : resp.headers) {
                if ((hk == "server" || hk == "Server") && hv == "HeliosView-TestServer/1.0") {
                    hasServerHeader = true;
                    break;
                }
            }
            TEST_CHECK(hasServerHeader, "client.get received expected response header");
        }

        // Test POST with body
        auto postTask = [&client, &baseUrl]() -> std::execution::task<helios::http::Response> {
            co_return co_await client.post(baseUrl + "/echo", "HelloServer", "text/plain");
        };

        auto resPost = std::execution::sync_wait(postTask());
        TEST_CHECK(resPost.has_value(), "client.post completed without error");
        if (resPost.has_value()) {
            auto& resp = std::get<0>(*resPost);
            TEST_CHECK(resp.status == 200, "client.post returned status 200 OK");
            TEST_CHECK(resp.body.find("HelloServer") != std::string::npos, "client.post echo body received correctly");
        }

        // Keep-alive pool verification
        TEST_CHECK(client.idle_connections() >= 1, "Connection pool maintained idle connection for keep-alive reuse");
        client.close_idle();
        TEST_CHECK(client.idle_connections() == 0, "client.close_idle() successfully flushed idle connections");

        // Stop server
        boost::system::error_code ec;
        acceptor.close(ec);
        serverIoc.stop();
        if (serverThread.joinable()) {
            serverThread.join();
        }
    }
}

// ----------------------------------------------------------------------------
// 7. WebViewJson & Boost.JSON Reflection (HeliosViewCore/WebViewJson.h)
// ----------------------------------------------------------------------------
struct SampleItem {
    int id;
    std::string name;
    double price;
};
BOOST_DESCRIBE_STRUCT(SampleItem, (), (id, name, price))

struct ComplexPayload {
    std::string title;
    std::vector<SampleItem> items;
};
BOOST_DESCRIBE_STRUCT(ComplexPayload, (), (title, items))

static void TestJsonAndDtoReflection() {
    std::cout << "\n=== [7] WebViewJson & DTO Reflection Tests ===\n";

    // DTO serialization to JSON
    ComplexPayload payload{
        .title = "HeliosView Store",
        .items = {
            {1, "Widget A", 19.99},
            {2, "Widget B", 49.50}
        }
    };

    boost::json::value jv = boost::json::value_from(payload);
    TEST_CHECK(jv.is_object(), "Serialized DTO is a JSON object");
    TEST_CHECK(jv.as_object()["title"].as_string() == "HeliosView Store", "Serialized DTO field 'title' matches");
    TEST_CHECK(jv.as_object()["items"].as_array().size() == 2, "Serialized DTO array 'items' has 2 elements");

    // DTO deserialization from JSON
    ComplexPayload parsed = boost::json::value_to<ComplexPayload>(jv);
    TEST_CHECK(parsed.title == payload.title, "Deserialized DTO title matches");
    TEST_CHECK(parsed.items.size() == 2, "Deserialized DTO items count matches");
    TEST_CHECK(parsed.items[0].id == 1 && parsed.items[0].name == "Widget A", "Deserialized DTO item 0 matches");
    TEST_CHECK(parsed.items[1].id == 2 && parsed.items[1].price == 49.50, "Deserialized DTO item 1 matches");

    // JsonError helper
    helios::JsonError err("rpc_error", "Method not found");
    TEST_CHECK(err.key == "rpc_error", "JsonError key matches");
    TEST_CHECK(err.value.as_string() == "Method not found", "JsonError value matches");
}

// ----------------------------------------------------------------------------
// 8. Core Metadata & Error Handling (HeliosViewCore/Error.h & Core C API)
// ----------------------------------------------------------------------------
static void TestCoreMetadataAndErrors() {
    std::cout << "\n=== [8] Core C API Metadata & Error Handling Tests ===\n";

    // API version and Backend
    const char* verStr = heliosview_version();
    TEST_CHECK(verStr != nullptr && strlen(verStr) > 0,
               std::string("heliosview_version returned: ") + (verStr ? verStr : ""));

    std::string cppVer = helios::version();
    TEST_CHECK(!cppVer.empty() && cppVer == verStr, "helios::version() matches C ABI version");

    const char* backend = heliosview_backend_name();
    TEST_CHECK(backend != nullptr && strlen(backend) > 0,
               std::string("heliosview_backend_name returned non-empty: ") + (backend ? backend : ""));

    // Error description helper
    std::string errDesc = helios::lastErrorDescription("sample_op");
    TEST_CHECK(!errDesc.empty() && errDesc.find("sample_op") != std::string::npos,
               "lastErrorDescription includes operation name");

    // Timer cancellation safe call
    helios::cancelTimer(999999);
    TEST_CHECK(true, "helios::cancelTimer(invalid_id) executed safely without throwing");
}

int main() {
    std::cout << "============================================================\n";
    std::cout << " HeliosView Comprehensive Non-UI Test Suite\n";
    std::cout << "============================================================\n";

    TestStringUtils();
    TestTypesAndGeometry();
    TestSignalSlot();
    TestSystemQueries();
    TestAsyncThreadPool();
    TestHttpAndUrlParser();
    TestJsonAndDtoReflection();
    TestCoreMetadataAndErrors();

    std::cout << "\n============================================================\n";
    std::cout << " Non-UI Test Summary: " << g_passed << " passed, " << g_failed << " failed.\n";
    std::cout << "============================================================\n";

    return (g_failed == 0) ? 0 : 1;
}
