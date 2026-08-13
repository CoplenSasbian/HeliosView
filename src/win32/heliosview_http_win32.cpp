// HeliosView.dll -- async HTTP client (non-blocking state machine).
//
// A client-style async HTTP/1.1 client (GET/POST/...). A heliosview_http_client_t
// acquires an SChannel credential and is bound to a loop; requests are issued
// from it.
//
// Transport: plain http:// and https://. HTTPS is implemented with Windows
// SChannel (SSPI) driven by the loop's async socket layer
// (heliosview_socket_connect / write / read_start) -- no blocking anywhere:
// handshake tokens and record data flow through per-request input/output
// buffers, mirroring the memory-BIO design. The whole exchange is a
// callback-driven state machine:
//   connect -> TLS handshake -> send request -> read response (http-parser).
//
// TLS needs no third-party dependency: SChannel is part of the Windows SDK, and
// server certificates are validated against the Windows system store
// (root + CA) with an RFC 6125-style hostname check.
//
// Every socket operation holds a reference on the request and fires exactly one
// terminal callback (the io layer guarantees a cancel callback on read_stop/close),
// so the request is reference-counted and freed only after the last callback.
// Request timeouts are enforced by the loop's one-shot timer service
// (heliosview_timer_create): a single timer thread tracks all deadlines in a
// min-heap, so the watchdog occupies no worker thread while waiting.
#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Win10: SCH_CREDENTIALS, TLS 1.3 protocol flags */
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define SECURITY_WIN32 /* expose the Secur32.dll (client) SSPI API */
#define SCHANNEL_USE_BLACKLISTS /* expose the current SCH_CREDENTIALS + TLS_PARAMETERS (SDK gates them) */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX /* keep std::min/std::max (windows.h defines min/max macros) */
#include <windows.h>
#include <winternl.h> /* UNICODE_STRING / PUNICODE_STRING (needed by SCH_CREDENTIALS block) */
#include <wincrypt.h>
#include <sspi.h>
#include <schannel.h>

#include <http_parser.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/* Parser header-field/value state machine. */
enum class HState { None, Field, Value };

/* Request lifecycle stages. */
enum class Stage { Connecting, Handshake, Sending, Reading };

/* Internal HTTP header (owned strings). The public C boundary type is the opaque
 * heliosview_http_headers_t handle; internally we just use C++ containers. */
struct Header {
    std::string name;
    std::string value;
};

/* The header collection (opaque in the public API). */
struct heliosview_http_headers {
    std::vector<Header> items;
};

/* SChannel (SSPI) TLS state for one request. SChannel is driven with buffered
 * input/output (SECBUFFER_TOKEN / SECBUFFER_DATA), which maps 1:1 onto the
 * memory-BIO design the OpenSSL code used: nothing blocks, the handshake and
 * record I/O are advanced by the same pump() state machine. */
struct TlsState {
    CtxtHandle ctx{};                  /* security context (per request) */
    bool have_ctx = false;             /* ctx has been initialized */
    SecPkgContext_StreamSizes sizes{}; /* header/trailer/max-message sizes */
    ULONG attrs = 0;                   /* context attributes (ISC output) */
    TimeStamp expiry{};                /* context expiry (ISC output) */
    std::wstring host_w;               /* UTF-16 server name (SNI + cert check) */
    std::vector<char> in;              /* pending encrypted inbound bytes */
};

/* The opaque client: owns an SChannel credential + the loop it issues requests on. */
struct heliosview_http_client {
    heliosview_loop_t* loop;
    CredHandle cred{};        /* SChannel credential (acquired once) */
    bool have_cred = false;
    std::atomic<uint32_t> timeout_ms{0}; /* 0 = no timeout (read at submission) */
};

/* The opaque request handle. */
struct heliosview_http_request {
    std::atomic<int> refs{1}; /* caller ref + one per in-flight socket op */
    std::mutex m;             /* serializes the per-request state machine (read/write/timeout) */
    heliosview_http_client* client;
    uint32_t timeout_ms = 0;  /* snapshot of the client timeout at submission */

    /* Request inputs (copied at submission time). */
    std::string method;
    std::string url;
    std::string body;
    std::vector<Header> req_headers;
    heliosview_http_response_cb cb;
    void* userdata;

    /* Parsed URL. */
    std::string host;
    std::string path;
    uint16_t port = 0;
    bool https = false;

    /* The plaintext HTTP request bytes (built once before sending). */
    std::string request_wire;
    size_t request_off = 0; /* plaintext bytes already handed to the transport */

    /* Connection state. */
    heliosview_socket_t* tcp = nullptr;
    TlsState tls; /* SChannel TLS state (https only) */
    Stage stage = Stage::Connecting;
    bool read_started = false;

    /* Outbound byte queue (single write in flight at a time). */
    std::string out;        /* produced, not yet submitted */
    std::string in_flight;  /* submitted to socket_write */
    bool write_in_flight = false;

    /* Response parser state. */
    http_parser parser{};
    http_parser_settings settings{};
    int status = 0;
    bool message_complete = false;
    bool parse_error = false;
    std::string cur_hdr_name, cur_hdr_value;
    HState hstate = HState::None;
    heliosview_http_headers resp_headers;
    std::string resp_body;

    std::atomic<bool> done{false};
    int final_error = 0; /* completion code chosen by the winning finalize() call */
    heliosview_timer_t* timer = nullptr; /* timeout watchdog (heliosview_timer_create) */
};

namespace {

/* forward declarations (defined below) */
void onWrite(int error, uint32_t bytes, void* userdata);
void onRead(int error, const char* data, uint32_t len, void* userdata);
void onConnect(int error, heliosview_socket_t* tcp, void* userdata);
void finishHeader(heliosview_http_request* r);
bool startWrite(heliosview_http_request* r);
bool pump(heliosview_http_request* r);
bool finalize(heliosview_http_request* r, int error);
void dispatchResponse(heliosview_http_request* r);

std::wstring toWide(const std::string& s);
int tlsCode(SECURITY_STATUS ss);
bool verifyServerCert(heliosview_http_request* r);
int tlsHandshake(heliosview_http_request* r);
int tlsWrite(heliosview_http_request* r, const char* data, size_t len);
bool tlsReadLoop(heliosview_http_request* r);

/* Internal TLS error codes (delivered through the response callback's error
 * argument; the public contract only distinguishes 0 vs non-zero). */
constexpr int kTlsCertError = -1200; /* certificate chain / hostname verification failed */
constexpr int kTlsErrorBase = -3000; /* fixed small codes below; SSPI statuses map further below */

/* UTF-8 -> UTF-16 (used for the SChannel target name / SNI / hostname check). */
std::wstring toWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

/* Stable, non-colliding negative code for an SSPI status (diagnostic only). */
int tlsCode(SECURITY_STATUS ss)
{
    return kTlsErrorBase - (int)((unsigned)ss & 0xFFFFu);
}

/* Validate the server certificate after the TLS handshake completes. With
 * SCH_CRED_MANUAL_CRED_VALIDATION the application owns the check: build the
 * chain against the Windows system stores (root + CA, same trust anchors the
 * old code loaded explicitly) and run the SSL chain policy, which also performs
 * the RFC 6125-style hostname match against the requested server name. */
bool verifyServerCert(heliosview_http_request* r)
{
    PCCERT_CONTEXT remote = nullptr;
    if (QueryContextAttributesW(&r->tls.ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) != SEC_E_OK
        || !remote)
        return false;

    bool ok = false;
    CERT_CHAIN_PARA chainPara{};
    chainPara.cbSize = sizeof(chainPara);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (CertGetCertificateChain(nullptr, remote, nullptr, nullptr, &chainPara, 0, nullptr, &chain)) {
        CERT_CHAIN_POLICY_PARA policyPara{};
        policyPara.cbSize = sizeof(policyPara);
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslPara{};
        sslPara.cbSize = sizeof(sslPara);
        sslPara.dwAuthType = AUTHTYPE_SERVER;
        sslPara.pwszServerName = const_cast<LPWSTR>(r->tls.host_w.c_str());
        policyPara.pvExtraPolicyPara = &sslPara;
        CERT_CHAIN_POLICY_STATUS policyStatus{};
        policyStatus.cbSize = sizeof(policyStatus);
        if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policyPara, &policyStatus))
            ok = (policyStatus.dwError == 0);
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(remote);
    return ok;
}

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

void parseFeed(heliosview_http_request* r, const char* data, size_t len)
{
    http_parser_execute(&r->parser, &r->settings, data, len);
    if (r->parser.http_errno != HPE_OK)
        r->parse_error = true;
}

/* ---------- SChannel TLS drivers ---------- */

/* Drive the TLS handshake as far as it can go. Outbound handshake tokens are
 * appended to r->out (the caller flushes them with startWrite). Returns:
 *   1        handshake complete (certificate verified, stream sizes queried)
 *   0        need more server data (or nothing new to do) -- wait for onRead
 *   negative fatal error code (the caller finalizes with it) */
int tlsHandshake(heliosview_http_request* r)
{
    /* Drive InitializeSecurityContext until it needs something new. The first
     * call runs with empty input to emit the ClientHello; during TLS 1.3 a
     * DecryptMessage may signal SEC_I_RENEGOTIATE to ask it to resume with the
     * (possibly empty) buffer to send the deferred client Finished, so we always
     * attempt a step here and let ISC reply CONTINUE_NEEDED when it's idle. */

    for (int calls = 0; calls < 64; ++calls) {
        SecBuffer inbufs[2] = {};
        inbufs[0].BufferType = SECBUFFER_TOKEN;
        inbufs[0].pvBuffer = r->tls.in.empty() ? nullptr : r->tls.in.data();
        inbufs[0].cbBuffer = (DWORD)r->tls.in.size();
        inbufs[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inDesc = { SECBUFFER_VERSION, 2, inbufs };

        SecBuffer outbufs[1] = {};
        outbufs[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outDesc = { SECBUFFER_VERSION, 1, outbufs };

        SECURITY_STATUS ss = InitializeSecurityContextW(
            &r->client->cred,
            r->tls.have_ctx ? &r->tls.ctx : nullptr,
            const_cast<SEC_WCHAR*>(r->tls.host_w.c_str()),
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
                | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
            0, SECBUFFER_VERSION, &inDesc, 0, &r->tls.ctx, &outDesc,
            &r->tls.attrs, &r->tls.expiry);
        r->tls.have_ctx = true;

        /* Keep any leftover server data (e.g. an early response) for later. */
        if (inbufs[1].BufferType == SECBUFFER_EXTRA && inbufs[1].cbBuffer > 0) {
            /* SECBUFFER_EXTRA = unconsumed remainder. pvBuffer normally points at
             * it inside the input buffer, but SChannel can return pvBuffer == NULL,
             * in which case the leftover is the trailing `cbBuffer` bytes of the
             * current input (seen on the TLS 1.3 handshake-resume path). */
            const size_t avail = r->tls.in.size();
            char* src = inbufs[1].pvBuffer
                            ? static_cast<char*>(inbufs[1].pvBuffer)
                            : (inbufs[1].cbBuffer <= avail
                                   ? r->tls.in.data() + (avail - inbufs[1].cbBuffer)
                                   : r->tls.in.data());
            std::memmove(r->tls.in.data(), src, inbufs[1].cbBuffer);
            r->tls.in.resize(inbufs[1].cbBuffer);
        } else {
            r->tls.in.clear();
        }

        const bool produced = outbufs[0].cbBuffer > 0;
        if (produced)
            r->out.append(static_cast<const char*>(outbufs[0].pvBuffer), outbufs[0].cbBuffer);
        if (outbufs[0].pvBuffer)
            FreeContextBuffer(outbufs[0].pvBuffer);

        if (ss == SEC_E_OK) {
            if (QueryContextAttributesW(&r->tls.ctx, SECPKG_ATTR_STREAM_SIZES,
                                        &r->tls.sizes) != SEC_E_OK)
                return kTlsErrorBase + 1;
            if (!verifyServerCert(r))
                return kTlsCertError;
            return 1;
        }
        if (ss == SEC_I_CONTINUE_NEEDED) {
            if (produced)
                continue; /* send the token (already queued) and keep driving --
                           * TLS 1.3 needs one more empty-input call to finish */
            return 0;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
            return 0;
        if (ss == SEC_I_INCOMPLETE_CREDENTIALS)
            return kTlsErrorBase + 2; /* server requested a client certificate */
        return tlsCode(ss);
    }
    return kTlsErrorBase + 3; /* handshake made no progress */
}

/* Encrypt plaintext into the outbound queue, chunked to SChannel's maximum
 * message size. Returns the number of plaintext bytes consumed, or a negative
 * error code. */
int tlsWrite(heliosview_http_request* r, const char* data, size_t len)
{
    const size_t maxMsg = r->tls.sizes.cbMaximumMessage > 0 ? r->tls.sizes.cbMaximumMessage : 16384;
    size_t off = 0;
    while (off < len) {
        const size_t chunk = std::min(len - off, maxMsg);
        std::string buf(r->tls.sizes.cbHeader + chunk + r->tls.sizes.cbTrailer, '\0');
        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = buf.data();
        bufs[0].cbBuffer = r->tls.sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = buf.data() + r->tls.sizes.cbHeader;
        bufs[1].cbBuffer = (DWORD)chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = buf.data() + r->tls.sizes.cbHeader + chunk;
        bufs[2].cbBuffer = r->tls.sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        std::memcpy(bufs[1].pvBuffer, data + off, chunk);
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
        const SECURITY_STATUS ss = EncryptMessage(&r->tls.ctx, 0, &desc, 0);
        if (ss != SEC_E_OK)
            return tlsCode(ss);
        const size_t encLen = (size_t)bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        r->out.append(buf.data(), encLen);
        off += chunk;
    }
    return (int)off;
}

/* Decrypt as much buffered TLS record data as possible, feeding plaintext to
 * http-parser. Returns true when the request finalized (the caller must
 * dispatch the response). */
bool tlsReadLoop(heliosview_http_request* r)
{
    if (r->done.load())
        return false;
    for (;;) {
        if (r->tls.in.empty())
            return false;
        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = r->tls.in.data();
        bufs[0].cbBuffer = (DWORD)r->tls.in.size();
        for (int i = 1; i < 4; ++i)
            bufs[i].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
        DWORD qop = 0;
        const SECURITY_STATUS ss = DecryptMessage(&r->tls.ctx, &desc, 0, &qop);
        if (ss == SEC_E_OK) {
            bool plain = false;
            for (int i = 1; i < 4; ++i) {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                    parseFeed(r, static_cast<const char*>(bufs[i].pvBuffer), bufs[i].cbBuffer);
                    plain = true;
                }
            }
            if (plain) {
                if (r->parse_error)
                    return finalize(r, -1);
                if (r->message_complete)
                    return finalize(r, 0);
            }
            /* Keep the unconsumed remainder (if any) for the next call. */
            SecBuffer* extra = nullptr;
            for (int i = 1; i < 4; ++i)
                if (bufs[i].BufferType == SECBUFFER_EXTRA)
                    extra = &bufs[i];
            if (extra && extra->cbBuffer > 0) {
                const size_t avail = r->tls.in.size();
                char* src = extra->pvBuffer
                                ? static_cast<char*>(extra->pvBuffer)
                                : (extra->cbBuffer <= avail
                                       ? r->tls.in.data() + (avail - extra->cbBuffer)
                                       : r->tls.in.data());
                std::memmove(r->tls.in.data(), src, extra->cbBuffer);
                r->tls.in.resize(extra->cbBuffer);
            } else {
                r->tls.in.clear();
            }
            continue;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
            return false;
        if (ss == SEC_I_CONTEXT_EXPIRED) {
            /* Peer sent close_notify: finalize a connection-close-delimited body. */
            if (r->stage == Stage::Reading && !r->message_complete)
                parseFeed(r, "", 0);
            return finalize(r, (r->message_complete && !r->parse_error) ? 0 : -1);
        }
        if (ss == SEC_I_RENEGOTIATE) {
            /* TLS 1.3: SChannel can defer emitting the client's Finished (and any
             * other outbound handshake messages) until the first DecryptMessage,
             * signalling it here. Resume the handshake to produce and send those
             * tokens, then try to decrypt again. */
            const int rc = tlsHandshake(r);
            if (!r->out.empty() && !r->write_in_flight) {
                if (startWrite(r))
                    return true;
            }
            if (r->done.load())
                return false;
            if (rc == 1)
                continue; /* handshake closed out: decrypt the pending record */
            if (rc == 0)
                return false; /* still need more server data */
            return finalize(r, rc); /* fatal */
        }
        return finalize(r, tlsCode(ss));
    }
}

/* ---------- reference counting ---------- */

void freeRequest(heliosview_http_request* r)
{
    if (r->tls.have_ctx)
        DeleteSecurityContext(&r->tls.ctx);
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
        dispatch = finalize(r, HELIOSVIEW_HTTP_TIMEOUT);
    }
    if (dispatch)
        dispatchResponse(r);
    /* the timeout ref is released by finalize() when it wins; if it lost, the
     * other winning finalize() already released it */
}

/* ---------- completion ---------- */

/* Advances the request to its terminal state. Must be called with r->m held.
 * Returns true when this call won the race and is responsible for delivering the
 * response callback (dispatchResponse, to be called after releasing the lock so
 * user code never runs under the request mutex). Returns false when the request
 * was already completed. Does NOT invoke the callback itself. */
bool finalize(heliosview_http_request* r, int error)
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
        return finalize(r, -1);
    if (r->message_complete)
        return finalize(r, 0);
    return false;
}

/* ---------- transport helpers ---------- */

bool startWrite(heliosview_http_request* r)
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
        return finalize(r, rc);
    }
    return false;
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
            dispatch = finalize(r, error);
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

bool startRead(heliosview_http_request* r)
{
    r->read_started = true;
    addRef(r);
    const int rc = heliosview_socket_read_start(r->tcp, &onRead, r);
    if (rc != 0) {
        r->read_started = false;
        release(r);
        return finalize(r, rc);
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
            dispatch = finalize(r, error);
            terminal = true;
        } else if (len == 0) {
            /* peer closed (EOF): finalize a connection-close-delimited body */
            if (r->https && r->stage == Stage::Reading && !r->tls.in.empty()) {
                /* Flush any complete TLS records still buffered; if the last
                 * record is incomplete the stream was truncated mid-record. */
                const bool tlsDone = tlsReadLoop(r);
                if (tlsDone) {
                    dispatch = true;
                } else if (!r->done.load()) {
                    if (!r->message_complete)
                        parseFeed(r, "", 0);
                    dispatch = finalize(r, (r->message_complete && !r->parse_error) ? 0 : -1);
                }
            } else {
                if (r->stage == Stage::Reading && !r->message_complete)
                    parseFeed(r, "", 0);
                dispatch = finalize(r, (r->message_complete && !r->parse_error) ? 0 : -1);
            }
            terminal = true;
        } else {
            /* data chunk */
            if (r->https) {
                r->tls.in.insert(r->tls.in.end(), data, data + len);
                dispatch = pump(r); /* tlsReadLoop -> parseFeed -> finalize */
            } else if (r->stage == Stage::Reading) {
                parseFeed(r, data, len);
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
        if (startWrite(r))
            return true;
    }
    if (r->done.load())
        return false; /* startWrite failed and finalized — the caller of pump dispatches */

    if (r->https) {
        /* 1) TLS handshake */
        if (r->stage == Stage::Handshake) {
            const int rc = tlsHandshake(r);
            if (rc == 1) {
                r->stage = Stage::Sending;
            } else if (rc == 0) {
                if (!r->out.empty() && !r->write_in_flight) {
                    if (startWrite(r))
                        return true;
                }
                return false; /* send the queued token(s), then wait for more server data */
            } else {
                return finalize(r, rc);
            }
        }

        /* 2) send the request (encrypt everything, then flush once) */
        if (r->stage == Stage::Sending) {
            while (r->request_off < r->request_wire.size()) {
                const int n = tlsWrite(r, r->request_wire.data() + r->request_off,
                                       r->request_wire.size() - r->request_off);
                if (n < 0)
                    return finalize(r, n);
                r->request_off += (size_t)n;
            }
            if (!r->out.empty() && !r->write_in_flight) {
                if (startWrite(r))
                    return true;
            }
            if (r->done.load())
                return false;
            r->stage = Stage::Reading;
        }

        /* 3) read the response (decrypt buffered records) */
        if (r->stage == Stage::Reading)
            return tlsReadLoop(r);
        return false;
    }

    /* plain http */
    if (r->stage == Stage::Sending) {
        r->out.append(r->request_wire.data() + r->request_off,
                      r->request_wire.size() - r->request_off);
        r->request_off = r->request_wire.size();
        if (startWrite(r))
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
            dispatch = finalize(r, error ? error : -1000);
        } else {
            r->tcp = tcp;
            if (r->https) {
                r->tls.host_w = toWide(r->host);
                if (r->tls.host_w.empty()) {
                    dispatch = finalize(r, -1001); /* host not convertible for TLS */
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

/* ================= Public API ================= */

heliosview_http_client_t* heliosview_http_client_create(heliosview_loop_t* loop)
{
    if (!loop)
        return nullptr;

    auto* client = hv::hv_alloc<heliosview_http_client>();
    client->loop = loop;

    /* Acquire one SChannel credential for the client; every request derives its
     * security context from it. TLS 1.2 + 1.3 (when the OS supports it); the
     * server certificate is validated manually (SCH_CRED_MANUAL_CRED_VALIDATION)
     * against the system stores in verifyServerCert(). */
    /* Use the current SCH_CREDENTIALS (v5) struct: modern SChannel builds (e.g.
     * recent Windows 11) reject the legacy SCHANNEL_CRED with
     * SEC_E_UNSUPPORTED_FUNCTION. It lives behind SCHANNEL_USE_BLACKLISTS in the
     * SDK and uses TLS_PARAMETERS (grbitDisabledProtocols) to express protocol
     * policy, so TLS 1.0/1.1 are disabled to keep a TLS 1.2 floor (matching the
     * original OpenSSL client). */
    TLS_PARAMETERS tlsParams{};
    tlsParams.grbitDisabledProtocols =
        SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_0_SERVER
        | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_1_SERVER;
    tlsParams.dwFlags = TLS_PARAMS_OPTIONAL;

    SCH_CREDENTIALS cred{};
    cred.dwVersion = SCH_CREDENTIALS_VERSION;
    cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    cred.cTlsParameters = 1;
    cred.pTlsParameters = &tlsParams;
    const SECURITY_STATUS ss = AcquireCredentialsHandleW(
        nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &cred, nullptr,
        nullptr, &client->cred, nullptr);
    if (ss != SEC_E_OK) {
        hv::hv_dealloc(client);
        return nullptr;
    }
    client->have_cred = true;
    return client;
}

void heliosview_http_client_destroy(heliosview_http_client_t* client)
{
    if (!client)
        return;
    if (client->have_cred)
        FreeCredentialsHandle(&client->cred);
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
        dispatch = finalize(request, HELIOSVIEW_HTTP_CANCELLED);
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
