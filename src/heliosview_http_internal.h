#pragma once

/**
 * Internal HTTP client header: the cross-platform async HTTP client core
 * (src/heliosview_http.cpp) plus the TLS backend interface every platform
 * implements (SChannel on Windows, OpenSSL on Linux, Secure Transport on
 * macOS). Not part of the public API.
 *
 * The core owns URL parsing, request wire building, the http-parser-driven
 * response state machine, reference counting and the
 * connect -> (TLS handshake) -> send request -> read response driver. It talks
 * to the loop's async socket/timer layer only through the public
 * heliosview_socket_* / heliosview_timer_* interfaces, so nothing in it is
 * platform specific. The only per-platform part is TLS: each platform
 * implements heliosview_tls_ops() and the core drives it through the
 * memory-BIO-style interface below (handshake tokens and record data flow
 * through the request's plaintext out / encrypted tls_in buffers, so nothing
 * ever blocks).
 */

#include <HeliosView/heliosview.h>
#include <http_parser.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
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

struct hv_tls_ops;

/* The opaque client: bound to a loop, owns the platform TLS credential/context
 * and the TLS backend it drives requests with. */
struct heliosview_http_client {
    heliosview_loop_t* loop;
    std::atomic<uint32_t> timeout_ms{0}; /* 0 = no timeout (read at submission) */
    void* tls_ctx = nullptr;             /* platform TLS credential / SSL_CTX (null = no TLS) */
    const hv_tls_ops* tls_ops = nullptr; /* platform TLS implementation (null = no TLS) */
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
    void* tls = nullptr;      /* opaque TLS session (https only; created by tls_ops->session_create) */
    std::vector<char> tls_in; /* pending encrypted inbound bytes (https only) */
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

/* ---------- TLS backend interface ----------
 * Each platform implements these operations (memory-BIO style: nothing blocks,
 * the core's pump() state machine advances them). A session's functions are
 * only ever called with the request mutex held. */

struct hv_tls_ops {
    /* Create/free the per-client TLS context (e.g. an SChannel credential or an
     * OpenSSL SSL_CTX). ctx_create returns nullptr on failure (the client then
     * supports plain http only). */
    void* (*ctx_create)(heliosview_http_client* client);
    void (*ctx_destroy)(void* ctx);

    /* Create/free the per-request TLS session. session_create returns nullptr on
     * failure (e.g. the host cannot be converted for the platform). */
    void* (*session_create)(heliosview_http_client* client, const char* host);
    void (*session_destroy)(void* sess);

    /* Drive the TLS handshake as far as it can go. Consumes r->tls_in and
     * appends any outbound handshake bytes to r->out (the core flushes them).
     * Returns:
     *   1   handshake complete (certificate verified)
     *   0   need more server data (or nothing new to do) -- wait for onRead
     *   <0  fatal error code (the core finalizes the request with it) */
    int (*handshake)(heliosview_http_request* r, void* sess);

    /* Encrypt plaintext into the outbound queue, chunked to the platform's
     * maximum message size. Returns the number of plaintext bytes consumed, or
     * a negative error code. */
    int (*write)(heliosview_http_request* r, void* sess, const char* data, size_t len);

    /* Decrypt as much buffered TLS record data as possible, feeding plaintext to
     * the http-parser (via hv_http_parse_feed). May append outbound bytes (e.g.
     * renegotiation tokens) to r->out and flush them with hv_http_start_write.
     * Returns true when the request finalized (the caller must dispatch the
     * response). */
    bool (*read)(heliosview_http_request* r, void* sess);
};

/* The platform's TLS backend. May return nullptr when the platform has no TLS
 * backend (the HTTP client then supports plain http only). */
const hv_tls_ops* heliosview_tls_ops(void);

/* ---------- Core helpers the TLS backend may call ----------
 * All hv_tls_ops functions run with the request mutex held, so these are safe
 * to call from a backend. */

/* Feed plaintext response bytes to the http-parser. */
void hv_http_parse_feed(heliosview_http_request* r, const char* data, size_t len);

/* Advance the request to its terminal state (mutex held by the caller). Returns
 * true when this call won the race and must deliver the response callback
 * (dispatchResponse, run outside the lock) -- same contract as the internal
 * finalize(). */
bool hv_http_finalize(heliosview_http_request* r, int error);

/* Submit r->out to the socket write if none is in flight. Returns true when the
 * request finalized while submitting (the caller must dispatch). */
bool hv_http_start_write(heliosview_http_request* r);

/* Internal TLS error codes shared by backends (delivered through the response
 * callback's error argument; the public contract only distinguishes 0 vs
 * non-zero). */
constexpr int kTlsCertError = -1200; /* certificate chain / hostname verification failed */
constexpr int kTlsErrorBase = -3000; /* fixed small codes below; platform statuses map further below */
