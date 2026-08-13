// HeliosView.dll -- Windows async HTTP client (WinHTTP).
//
// The request runs on a background worker thread of the loop's pool (posted via
// heliosview_loop_post), using the synchronous WinHTTP API. HTTPS/TLS, redirects
// and system proxy settings are handled by WinHTTP itself. The response callback
// fires exactly once, from the worker thread.
#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

/* Removed in the newer SDKs: the "header by value" sentinels are still (DWORD)-1,
 * used as the LPCWSTR header-name argument of WinHttpQueryHeaders */
#ifndef WINHTTP_HEADER_NAME_BY_VALUE
#define WINHTTP_HEADER_NAME_BY_VALUE ((LPCWSTR)(DWORD_PTR)-1)
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

/* The opaque handle: caller-owned; destroyed with heliosview_http_destroy.
 * The worker thread fills the result fields, then invokes the callback. */
struct heliosview_http_request {
    // Request inputs (owned by this object).
    heliosview_loop_t* loop;
    std::string method;
    std::string url;
    std::string headers_json;
    std::string body;

    // Response callback (owned by the caller via userdata ownership contract).
    heliosview_http_response_cb cb;
    void* userdata;

    // Results (filled by the worker before invoking the callback).
    int error = 0;
    int status_code = 0;
    std::string resp_headers_json = "{}";
    std::string resp_body;

    // Completion bookkeeping.
    std::atomic<bool> done{false};      // set right before the callback fires
    std::atomic<bool> cancelled{false}; // requested by heliosview_http_cancel
};

namespace {

/* Parse an RFC-style "Name: value" header block into a JSON object.
 * Duplicate names: the last value wins. */
nlohmann::json parseHeadersJson(const char* raw, size_t len)
{
    nlohmann::json out = nlohmann::json::object();
    if (!raw || len == 0)
        return out;

    std::string_view text(raw, len);
    size_t start = 0;
    while (start < text.size()) {
        size_t eol = text.find("\r\n", start);
        std::string_view line = (eol == std::string_view::npos)
                                    ? text.substr(start)
                                    : text.substr(start, eol - start);
        if (eol == std::string_view::npos)
            start = text.size();
        else
            start = eol + 2;

        if (line.empty())
            continue;

        const size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;

        std::string name(line.substr(0, colon));
        std::string value(line.substr(colon + 1));

        // Trim name.
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
            name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        // Trim value.
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();

        if (!name.empty())
            out[name] = value;
    }
    return out;
}

/* Build the "Name: value\r\n" request-header block from a JSON object of
 * string values. Returns false if the JSON is not a valid header map. */
bool buildRequestHeaders(const char* headers_json, std::string& out)
{
    out.clear();
    if (!headers_json || !*headers_json)
        return true;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(headers_json);
    } catch (...) {
        return false;
    }
    if (!j.is_object())
        return false;

    for (auto& [name, value] : j.items()) {
        if (!value.is_string())
            return false;
        out += name;
        out += ": ";
        out += value.get<std::string>();
        out += "\r\n";
    }
    return true;
}

/* Convert a UTF-8 string to UTF-16 (WinHTTP wide-char API). */
std::wstring utf8ToWide(std::string_view s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out;
    out.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

/* The worker: runs the synchronous WinHTTP request on a loop worker thread. */
void httpWorker(int, void* userdata)
{
    auto* req = static_cast<struct heliosview_http_request*>(userdata);

    if (req->cancelled.load()) {
        req->error = -1; /* cancelled before it started */
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    // Parse the URL: scheme, host, port, path + query.
    const std::wstring urlW = utf8ToWide(req->url);
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(urlW.c_str(), (DWORD)urlW.size(), 0, &uc)) {
        req->error = -2;
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    const std::wstring extra = uc.dwExtraInfoLength ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"";
    /* 0 = use the scheme default port (WinHTTP then selects 80/443 itself) */
    const INTERNET_PORT port = static_cast<INTERNET_PORT>(uc.nPort);

    // Request headers (a JSON object -> "Name: value" lines).
    std::string headerBlock;
    if (!buildRequestHeaders(req->headers_json.c_str(), headerBlock)) {
        req->error = -3;
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    HINTERNET hSession = WinHttpOpen(L"HeliosView/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        req->error = -(int)GetLastError();
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    // 30 s timeouts (resolve/connect/send/receive).
    WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 30000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        req->error = -(int)GetLastError();
        WinHttpCloseHandle(hSession);
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    const DWORD flags = WINHTTP_FLAG_REFRESH |
                        (https ? WINHTTP_FLAG_SECURE : 0);
    const std::wstring method = utf8ToWide(req->method);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(),
                                            (path + extra).c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        req->error = -(int)GetLastError();
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    // Follow redirects automatically.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!headerBlock.empty()) {
        const std::wstring headerBlockW = utf8ToWide(headerBlock);
        WinHttpAddRequestHeaders(hRequest, headerBlockW.c_str(), (DWORD)headerBlockW.size(),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    const void* body = req->body.empty() ? WINHTTP_NO_REQUEST_DATA : req->body.data();
    const DWORD bodyLen = (DWORD)req->body.size();
    if (!WinHttpSendRequest(hRequest, NULL, 0,
                            const_cast<void*>(body), bodyLen, bodyLen, 0)) {
        req->error = -(int)GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        req->error = -(int)GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        req->done.store(true);
        req->cb(req, req->error, 0, "{}", "", 0, req->userdata);
        return;
    }

    // Status code.
    DWORD statusLen = sizeof(req->status_code);
    DWORD status = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_VALUE, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
    req->status_code = (int)status;

    // Raw response headers ("Name: value\r\n" ...) -> JSON object.
    DWORD rawLen = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_VALUE, WINHTTP_NO_OUTPUT_BUFFER, &rawLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (rawLen > 0) {
        std::wstring rawW;
        rawW.resize(rawLen / sizeof(wchar_t));
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_VALUE, rawW.data(), &rawLen,
                                WINHTTP_NO_HEADER_INDEX)) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, rawW.c_str(), (int)rawW.size(),
                                              nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string raw;
                raw.resize(n);
                WideCharToMultiByte(CP_UTF8, 0, rawW.c_str(), (int)rawW.size(),
                                    raw.data(), n, nullptr, nullptr);
                req->resp_headers_json = parseHeadersJson(raw.c_str(), raw.size()).dump();
            }
        }
    }

    // Body.
    std::string bodyBuf;
    char chunk[64 * 1024];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, chunk, sizeof(chunk), &got))
            break;
        if (got == 0)
            break;
        bodyBuf.append(chunk, got);
    }
    req->resp_body = std::move(bodyBuf);

    req->error = 0;
    req->done.store(true);
    req->cb(req, req->error, req->status_code, req->resp_headers_json.c_str(),
            req->resp_body.data(), req->resp_body.size(), req->userdata);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

} // namespace

/* ================= Public API ================= */

heliosview_http_request_t* heliosview_http_request(heliosview_loop_t* loop, const char* method,
                                                   const char* url, const char* headers_json,
                                                   const char* body, size_t body_len,
                                                   heliosview_http_response_cb on_response,
                                                   void* userdata)
{
    if (!loop || !method || !url || !on_response)
        return nullptr;

    auto* req = hv::hv_alloc<struct heliosview_http_request>();
    req->loop = loop;
    req->method = method;
    req->url = url;
    req->headers_json = headers_json ? headers_json : "";
    if (body && body_len)
        req->body.assign(body, body_len);
    req->cb = on_response;
    req->userdata = userdata;

    if (heliosview_loop_post(loop, &httpWorker, req) != 0) {
        hv::hv_dealloc(req);
        return nullptr;
    }
    return req;
}

int heliosview_http_cancel(heliosview_http_request_t* request)
{
    if (!request)
        return -1;
    if (request->done.load())
        return -1; /* already completed */
    request->cancelled.store(true);
    return 0;
}

void heliosview_http_destroy(heliosview_http_request_t* request)
{
    hv::hv_dealloc(request);
}
