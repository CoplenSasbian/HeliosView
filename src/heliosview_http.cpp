// HeliosView.dll -- async HTTP client core (non-blocking state machine).
//
// The cross-platform half of the HTTP client: URL parsing, request wire
// building, http-parser-driven response parsing, reference counting and the
// connect -> (TLS handshake) -> send request -> read response driver.
//
// The driver talks to the loop's async socket/timer layer only through the
// public heliosview_socket_* / heliosview_timer_* interfaces, so nothing in
// this file is platform specific. The one platform-dependent piece -- TLS -- is
// behind the hv_tls_ops interface (see heliosview_http_internal.h): each
// platform implements heliosview_tls_ops() (SChannel on Windows, OpenSSL on
// Linux, ...) and the core drives its memory-BIO-style session through the
// request's encrypted tls_in / plaintext out buffers -- no blocking anywhere.
//
// Every socket operation holds a reference on the request and fires exactly one
// terminal callback (the io layer guarantees a cancel callback on read_stop/close),
// so the request is reference-counted and freed only after the last callback.
// Request timeouts are enforced by the loop's one-shot timer service
// (heliosview_timer_create): a single timer thread tracks all deadlines in a
// min-heap, so the watchdog occupies no worker thread while waiting.
#include <HeliosView/heliosview.h>
#include "heliosview_internal.h"
#include "heliosview_http_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

/* forward declarations (defined below) */
void onWrite(int error, uint32_t bytes, void* userdata);
void onRead(int error, const char* data, uint32_t len, void* userdata);
void onConnect(int error, heliosview_socket_t* tcp, void* userdata);
void finishHeader(heliosview_http_request* r);
bool pump(heliosview_http_request* r);
bool finalizeIfComplete(heliosview_http_request* r);
void dispatchResponse(heliosview_http_request* r);

/* ---------- small string helpers ---------- */

bool ciEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (char)((unsigned char)a[i] >= 'A' && (unsigned char)a[i] <= 'Z'
                                   ? (unsigned char)a[i] + ('a' - 'A')
                                   : (unsigned char)a[i]);
        const char cb = (char)((unsigned char)b[i] >= 'A' && (unsigned char)b[i] <= 'Z'
                                   ? (unsigned char)b[i] + ('a' - 'A')
                                   : (unsigned char)b[i]);
        if (ca != cb)
            return false;
    }
    return true;
}

void trim(std::string& s)
{
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t'))
        ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        --e;
    s = s.substr(b, e - b);
}

/* Parse "scheme://authority[/path]" into host / port / path. */
bool parseUrl(heliosview_http_request* r)
{
    const std::string& u = r->url;
    const size_t p = u.find("://");
    if (p == std::string::npos)
        return false;
    const std::string scheme = u.substr(0, p);
    if (scheme == "https")
        r->https = true;
    else if (scheme == "http")
        r->https = false;
    else
        return false;

    const size_t start = p + 3;
    const size_t slash = u.find('/', start);
    std::string authority;
    if (slash == std::string::npos) {
        authority = u.substr(start);
        r->path = "/";
    } else {
        authority = u.substr(start, slash - start);
        r->path = u.substr(slash);
    }
    if (authority.empty())
        return false;

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find_first_not_of(':') < colon) {
        r->host = authority.substr(0, colon);
        const std::string portStr = authority.substr(colon + 1);
        if (portStr.empty()
            || !std::all_of(portStr.begin(), portStr.end(),
                            [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        const unsigned long portVal = std::strtoul(portStr.c_str(), nullptr, 10);
        if (portVal == 0 || portVal > 65535)
            return false;
        r->port = (uint16_t)portVal;
    } else {
        r->host = authority;
        r->port = r->https ? 443 : 80;
    }
    if (r->host.empty())
        return false;
    return true;
}

/* Build the plaintext HTTP/1.1 request. */
void buildWire(heliosview_http_request* r)
{
    std::string& w = r->request_wire;
    w = r->method + " " + r->path + " HTTP/1.1\r\n";
    w += "Host: " + r->host;
    if (r->port != (r->https ? 443 : 80))
        w += ":" + std::to_string(r->port);
    w += "\r\n";
    for (const auto& h : r->req_headers)
        w += h.name + ": " + h.value + "\r\n";
    if (!r->body.empty())
        w += "Content-Length: " + std::to_string(r->body.size()) + "\r\n";
    w += "Connection: close\r\n\r\n";
    w += r->body;
}

/* ---------- header collection (last value wins, case-insensitive) ---------- */

void headerSet(std::vector<Header>& items, std::string name, std::string value)
{
    auto it = std::find_if(items.begin(), items.end(),
                           [&](const Header& h) { return ciEqual(h.name, name); });
    if (it != items.end())
        it->value = std::move(value);
    else
        items.push_back({ std::move(name), std::move(value) });
}

/* ---------- http-parser callbacks ---------- */

int onHeaderField(http_parser* p, const char* at, size_t len)
{
    auto* r = static_cast<heliosview_http_request*>(p->data);
    if (r->hstate == HState::Value)
        finishHeader(r);
    r->hstate = HState::Field;
    r->cur_hdr_name.append(at, len);
    return 0;
}

int onHeaderValue(http_parser* p, const char* at, size_t len)
{
    auto* r = static_cast<heliosview_http_request*>(p->data);
    r->hstate = HState::Value;
    r->cur_hdr_value.append(at, len);
    return 0;
}

int onHeadersComplete(http_parser* p)
{
    auto* r = static_cast<heliosview_http_request*>(p->data);
    finishHeader(r);
    r->hstate = HState::None;
    r->status = (int)p->status_code;
    return 0;
}

int onBody(http_parser* p, const char* at, size_t len)
{
    auto* r = static_cast<heliosview_http_request*>(p->data);
    r->resp_body.append(at, len);
    return 0;
}

int onMessageComplete(http_parser* p)
{
    auto* r = static_cast<heliosview_http_request*>(p->data);
    r->message_complete = true; /* finalized by finalizeIfComplete() in the driver */
    return 0;
}

void finishHeader(heliosview_http_request* r)
{
    if (r->cur_hdr_name.empty() && r->cur_hdr_value.empty())
        return;
    trim(r->cur_hdr_name);
    trim(r->cur_hdr_value);
    if (!r->cur_hdr_name.empty())
        headerSet(r->resp_headers.items, std::move(r->cur_hdr_name), std::move(r->cur_hdr_value));
    r->cur_hdr_name.clear();
    r->cur_hdr_value.clear();
}

/* ---------- reference counting ---------- */

void freeRequest(heliosview_http_request* r)
{
    if (r->tls && r->client && r->client->tls_ops)
        r->client->tls_ops->session_destroy(r->tls);
    hv::hv_dealloc(r);
}

void addRef(heliosview_http_request* r)
{
    r->refs.fetch_add(1, std::memory_order_relaxed);
}

void release(heliosview_http_request* r)
{
    if (r->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        freeRequest(r);
}

/* Timeout watchdog: a one-shot timer (heliosview_timer_create) fires this on a
 * loop worker thread; it forces a HELIOSVIEW_HTTP_TIMEOUT completion when the
 * request outlives its budget. The loop's timer service tracks all deadlines in
 * a single timer thread, so no worker is occupied while waiting. */
void timeoutTramp(int, void* userdata)
{
    auto* r = static_cast<heliosview_http_request*>(userdata);
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(r->m);
        dispatch = hv_http_finalize(r, HELIOSVIEW_HTTP_TIMEOUT);
    }
    if (dispatch)
        dispatchResponse(r);
    /* the timeout ref is released by finalize() when it wins; if it lost, the
     * other winning finalize() already released it */
}

/* Delivers the response callback. Call with the lock released: it runs arbitrary
 * user code (which may even destroy the request / client). The caller holds a ref
 * on the request for the whole call, so r stays alive until after it returns. */
void dispatchResponse(heliosview_http_request* r)
{
    heliosview_http_response resp;
    resp.status_code = (r->final_error == 0) ? r->status : 0;
    resp.headers = &r->resp_headers;
    resp.body = r->resp_body.data();
    resp.body_len = r->resp_body.size();
    r->cb(r, r->final_error, (r->final_error == 0) ? &resp : nullptr, r->userdata);
}

/* After the parser ran, finalize if the response is fully parsed (or failed).
 * Returns true when it finalized (the caller must dispatch). */
bool finalizeIfComplete(heliosview_http_request* r)
{
    if (r->parse_error)
        return hv_http_finalize(r, -1);
    if (r->message_complete)
        return hv_http_finalize(r, 0);
    return false;
}

/* ---------- transport helpers ---------- */

bool startRead(heliosview_http_request* r)
{
    r->read_started = true;
    addRef(r);
    const int rc = heliosview_socket_read_start(r->tcp, &onRead, r);
    if (rc != 0) {
        r->read_started = false;
        release(r);
        return hv_http_finalize(r, rc);
    }
    return false;
}

void onRead(int error, const char* data, uint32_t len, void* userdata)
{
    auto* r = static_cast<heliosview_http_request*>(userdata);
    bool terminal = false;
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(r->m);
        if (error != 0) {
            /* terminal: real error or HELIOSVIEW_IO_CANCELLED. Route through
             * finalize() (which no-ops when done is already set) so the
             * connection is closed and the watchdog dropped — a direct callback
             * here would leak the socket (only heliosview_socket_close releases
             * it; the io layer's error path leaves it open). */
            dispatch = hv_http_finalize(r, error);
            terminal = true;
        } else if (len == 0) {
            /* peer closed (EOF): finalize a connection-close-delimited body */
            if (r->https && r->stage == Stage::Reading && !r->tls_in.empty()) {
                /* Flush any complete TLS records still buffered; if the last
                 * record is incomplete the stream was truncated mid-record. */
                const bool tlsDone = r->client->tls_ops->read(r, r->tls);
                if (tlsDone) {
                    dispatch = true;
                } else if (!r->done.load()) {
                    if (!r->message_complete)
                        hv_http_parse_feed(r, "", 0);
                    dispatch = hv_http_finalize(r, (r->message_complete && !r->parse_error) ? 0 : -1);
                }
            } else {
                if (r->stage == Stage::Reading && !r->message_complete)
                    hv_http_parse_feed(r, "", 0);
                dispatch = hv_http_finalize(r, (r->message_complete && !r->parse_error) ? 0 : -1);
            }
            terminal = true;
        } else {
            /* data chunk */
            if (r->https) {
                r->tls_in.insert(r->tls_in.end(), data, data + len);
                dispatch = pump(r); /* tls read -> parseFeed -> finalize */
            } else if (r->stage == Stage::Reading) {
                hv_http_parse_feed(r, data, len);
                dispatch = finalizeIfComplete(r);
                if (!dispatch)
                    dispatch = pump(r); /* resubmit any leftover writes, then continue */
            } else {
                dispatch = pump(r);
            }
            /* read continues; its terminal callback releases the read ref */
        }
    }
    if (dispatch)
        dispatchResponse(r);
    if (terminal)
        release(r); /* read ref */
}

void onWrite(int error, uint32_t bytes, void* userdata)
{
    auto* r = static_cast<heliosview_http_request*>(userdata);
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(r->m);
        r->write_in_flight = false;
        if (r->done.load()) {
            /* already completed: just release the write ref below */
        } else if (error) {
            dispatch = hv_http_finalize(r, error);
        } else {
            if (bytes < r->in_flight.size()) {
                std::string rem = r->in_flight.substr(bytes);
                r->in_flight.clear();
                rem += std::move(r->out);
                r->out = std::move(rem);
            } else {
                r->in_flight.clear();
            }
            dispatch = pump(r);
        }
    }
    if (dispatch)
        dispatchResponse(r);
    release(r); /* write ref */
}

/* ---------- the driver: advance the exchange as far as it can ---------- */

/* Returns true when the request finalized while advancing (caller must dispatch). */
bool pump(heliosview_http_request* r)
{
    if (r->done.load())
        return false;

    /* Resubmit bytes left over from a partial socket write (WSASend may complete
     * with fewer bytes than requested, e.g. for bodies larger than SO_SNDBUF).
     * The Handshake/Sending stages drive writes themselves, so a non-empty
     * remainder only reaches this point while idle in Reading. */
    if (!r->out.empty() && !r->write_in_flight) {
        if (hv_http_start_write(r))
            return true;
    }
    if (r->done.load())
        return false; /* startWrite failed and finalized — the caller of pump dispatches */

    if (r->https) {
        const hv_tls_ops* ops = r->client->tls_ops;
        void* sess = r->tls;

        /* 1) TLS handshake */
        if (r->stage == Stage::Handshake) {
            const int rc = ops->handshake(r, sess);
            if (rc == 1) {
                r->stage = Stage::Sending;
            } else if (rc == 0) {
                if (!r->out.empty() && !r->write_in_flight) {
                    if (hv_http_start_write(r))
                        return true;
                }
                return false; /* send the queued token(s), then wait for more server data */
            } else {
                return hv_http_finalize(r, rc);
            }
        }

        /* 2) send the request (encrypt everything, then flush once) */
        if (r->stage == Stage::Sending) {
            while (r->request_off < r->request_wire.size()) {
                const int n = ops->write(r, sess, r->request_wire.data() + r->request_off,
                                         r->request_wire.size() - r->request_off);
                if (n < 0)
                    return hv_http_finalize(r, n);
                r->request_off += (size_t)n;
            }
            if (!r->out.empty() && !r->write_in_flight) {
                if (hv_http_start_write(r))
                    return true;
            }
            if (r->done.load())
                return false;
            r->stage = Stage::Reading;
        }

        /* 3) read the response (decrypt buffered records) */
        if (r->stage == Stage::Reading)
            return ops->read(r, sess);
        return false;
    }

    /* plain http */
    if (r->stage == Stage::Sending) {
        r->out.append(r->request_wire.data() + r->request_off,
                      r->request_wire.size() - r->request_off);
        r->request_off = r->request_wire.size();
        if (hv_http_start_write(r))
            return true;
        r->stage = Stage::Reading;
    }

    if (r->stage == Stage::Reading) {
        /* nothing left to read here: the socket read callback feeds parseFeed */
    }
    return false;
}

/* ---------- async connect callback ---------- */

void onConnect(int error, heliosview_socket_t* tcp, void* userdata)
{
    auto* r = static_cast<heliosview_http_request*>(userdata);
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(r->m);
        if (r->done.load()) {
            /* already completed (e.g. timed out) while connecting: close the socket */
            if (tcp)
                heliosview_socket_close(tcp);
        } else if (error || !tcp) {
            dispatch = hv_http_finalize(r, error ? error : -1000);
        } else {
            r->tcp = tcp;
            if (r->https) {
                r->tls = r->client->tls_ops->session_create(r->client, r->host.c_str());
                if (!r->tls) {
                    dispatch = hv_http_finalize(r, -1001); /* TLS session could not be created */
                } else {
                    r->stage = Stage::Handshake;
                }
            } else {
                r->stage = Stage::Sending;
            }
            if (!dispatch && r->tcp) { /* still live (not completed above) */
                dispatch = startRead(r);
                if (!dispatch)
                    dispatch = pump(r);
            }
        }
    }
    if (dispatch)
        dispatchResponse(r);
    release(r); /* connect ref */
}

} // namespace

/* ================= Core helpers called by the TLS backend ================= */

void hv_http_parse_feed(heliosview_http_request* r, const char* data, size_t len)
{
    http_parser_execute(&r->parser, &r->settings, data, len);
    if (r->parser.http_errno != HPE_OK)
        r->parse_error = true;
}

bool hv_http_finalize(heliosview_http_request* r, int error)
{
    if (r->done.exchange(true))
        return false;
    r->final_error = error;
    if (r->timer) {
        /* Drop the watchdog. The timeout ref (addRef before timer_create) is
         * released here unconditionally: if the timer was still pending it will
         * never fire, and if it already fired its tramp's finalize() will lose
         * the race above and thus must NOT release it. This also covers the
         * case where the timer fired but its tramp could not be posted (loop
         * shutting down), which would otherwise leak the ref. */
        heliosview_timer_destroy(r->timer);
        r->timer = nullptr;
        release(r); /* timeout ref */
    }
    if (r->tcp) {
        heliosview_socket_close(r->tcp); /* cancels the read (terminal CANCELLED cb) + pending writes */
        r->tcp = nullptr;
    }
    return true;
}

bool hv_http_start_write(heliosview_http_request* r)
{
    if (r->write_in_flight || r->out.empty())
        return false;
    r->in_flight = std::move(r->out);
    r->write_in_flight = true;
    addRef(r);
    const int rc = heliosview_socket_write(r->tcp, r->in_flight.data(),
                                           (uint32_t)r->in_flight.size(),
                                           &onWrite, r);
    if (rc != 0) {
        r->write_in_flight = false;
        r->in_flight.clear();
        release(r);
        return hv_http_finalize(r, rc);
    }
    return false;
}

/* ================= Public API ================= */

heliosview_http_client_t* heliosview_http_client_create(heliosview_loop_t* loop)
{
    if (!loop)
        return nullptr;

    auto* client = hv::hv_alloc<heliosview_http_client>();
    client->loop = loop;

    /* Acquire the platform TLS credential for the client (SChannel on Windows,
     * OpenSSL on Linux, ...); every request derives its TLS session from it.
     * A platform with no TLS backend (heliosview_tls_ops() == nullptr) still
     * yields a working client for plain http. */
    client->tls_ops = heliosview_tls_ops();
    if (client->tls_ops) {
        client->tls_ctx = client->tls_ops->ctx_create(client);
        if (!client->tls_ctx) {
            hv::hv_dealloc(client);
            return nullptr;
        }
    }
    return client;
}

void heliosview_http_client_destroy(heliosview_http_client_t* client)
{
    if (!client)
        return;
    if (client->tls_ops && client->tls_ctx)
        client->tls_ops->ctx_destroy(client->tls_ctx);
    hv::hv_dealloc(client);
}

void heliosview_http_client_set_timeout(heliosview_http_client_t* client, uint32_t timeout_ms)
{
    if (client)
        client->timeout_ms.store(timeout_ms, std::memory_order_relaxed);
}

heliosview_http_request_t* heliosview_http_client_request(
    heliosview_http_client_t* client, const char* method, const char* url,
    const heliosview_http_headers_t* headers,
    const void* body, size_t body_len,
    heliosview_http_response_cb on_response, void* userdata)
{
    if (!client || !method || !url || !on_response)
        return nullptr;

    auto* r = hv::hv_alloc<heliosview_http_request>();
    r->client = client;
    r->method = method;
    r->url = url;
    r->cb = on_response;
    r->userdata = userdata;
    r->timeout_ms = client->timeout_ms.load(std::memory_order_relaxed);

    if (!parseUrl(r)) {
        hv::hv_dealloc(r);
        return nullptr;
    }

    /* HTTPS needs a TLS backend with a usable credential. */
    if (r->https && (!client->tls_ops || !client->tls_ctx)) {
        hv::hv_dealloc(r);
        return nullptr;
    }

    if (headers) {
        r->req_headers.reserve(headers->items.size());
        r->req_headers = headers->items;
    }
    if (body && body_len)
        r->body.assign(static_cast<const char*>(body), body_len);

    buildWire(r);

    http_parser_init(&r->parser, HTTP_RESPONSE);
    r->parser.data = r;
    r->settings.on_header_field = &onHeaderField;
    r->settings.on_header_value = &onHeaderValue;
    r->settings.on_headers_complete = &onHeadersComplete;
    r->settings.on_body = &onBody;
    r->settings.on_message_complete = &onMessageComplete;

    addRef(r); /* connect ref */
    if (heliosview_socket_connect(client->loop, r->host.c_str(), r->port, &onConnect, r) != 0) {
        hv::hv_dealloc(r);
        return nullptr;
    }
    if (r->timeout_ms > 0) {
        addRef(r); /* timeout ref (released when the watchdog fires or is destroyed) */
        r->timer = heliosview_timer_create(client->loop, r->timeout_ms, &timeoutTramp, r);
        if (!r->timer)
            release(r); /* failed to create the watchdog: drop its ref */
    }
    return r;
}

int heliosview_http_request_cancel(heliosview_http_request_t* request)
{
    if (!request)
        return -1;
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(request->m);
        if (request->done.load())
            return -1; /* already completed */
        /* Complete inline: closes the socket, which unwinds any pending read/
         * connect with a terminal callback, so the response callback still
         * fires exactly once (on this thread, before cancel returns). */
        dispatch = hv_http_finalize(request, HELIOSVIEW_HTTP_CANCELLED);
    }
    if (dispatch)
        dispatchResponse(request);
    return 0;
}

void heliosview_http_request_destroy(heliosview_http_request_t* request)
{
    release(request); /* drops the caller ref; freed after the last socket callback */
}

/* ================= Header collection ================= */

heliosview_http_headers_t* heliosview_http_headers_create(void)
{
    return hv::hv_alloc<heliosview_http_headers>();
}

void heliosview_http_headers_destroy(heliosview_http_headers_t* headers)
{
    hv::hv_dealloc(headers);
}

int heliosview_http_headers_add(heliosview_http_headers_t* headers, const char* name, const char* value)
{
    if (!headers || !name)
        return -1;
    headers->items.push_back({ name, value ? value : "" });
    return 0;
}

int heliosview_http_headers_set(heliosview_http_headers_t* headers, const char* name, const char* value)
{
    if (!headers || !name)
        return -1;
    headerSet(headers->items, name, value ? value : "");
    return 0;
}

int heliosview_http_headers_remove(heliosview_http_headers_t* headers, const char* name)
{
    if (!headers || !name)
        return -1;
    const std::string n = name;
    const size_t before = headers->items.size();
    std::erase_if(headers->items, [&](const Header& h) { return ciEqual(h.name, n); });
    return (int)(before - headers->items.size());
}

void heliosview_http_headers_clear(heliosview_http_headers_t* headers)
{
    if (headers)
        headers->items.clear();
}

size_t heliosview_http_headers_count(const heliosview_http_headers_t* headers)
{
    return headers ? headers->items.size() : 0;
}

int heliosview_http_headers_get(const heliosview_http_headers_t* headers, size_t index,
                                const char** out_name, const char** out_value)
{
    if (!headers || index >= headers->items.size())
        return -1;
    if (out_name)
        *out_name = headers->items[index].name.c_str();
    if (out_value)
        *out_value = headers->items[index].value.c_str();
    return 0;
}

const char* heliosview_http_headers_find(const heliosview_http_headers_t* headers, const char* name)
{
    if (!headers || !name)
        return nullptr;
    const std::string n = name;
    auto it = std::find_if(headers->items.begin(), headers->items.end(),
                           [&](const Header& h) { return ciEqual(h.name, n); });
    return it == headers->items.end() ? nullptr : it->value.c_str();
}
