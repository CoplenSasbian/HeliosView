// HeliosView.dll -- SChannel (SSPI) TLS backend for the async HTTP client.
//
// The cross-platform HTTP core (src/heliosview_http.cpp) drives TLS through the
// hv_tls_ops interface (see heliosview_http_internal.h); this file is the
// Windows half: SChannel, part of the Windows SDK -- no third-party TLS
// dependency. Handshake tokens and record data flow through the request's
// encrypted tls_in / plaintext out buffers, mirroring the memory-BIO design:
// nothing blocks, the handshake and record I/O are advanced by the same pump()
// state machine.
//
// TLS needs no third-party dependency: SChannel is part of the Windows SDK, and
// server certificates are validated against the Windows system store
// (root + CA) with an RFC 6125-style hostname check.
#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"
#include "../heliosview_http_internal.h"

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

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

/* SChannel (SSPI) TLS state for one request. SChannel is driven with buffered
 * input/output (SECBUFFER_TOKEN / SECBUFFER_DATA), which maps 1:1 onto the
 * memory-BIO design the HTTP core uses: nothing blocks, the handshake and
 * record I/O are advanced by the core's pump() state machine. The raw inbound
 * bytes live in r->tls_in (owned by the core); this struct holds only the
 * SChannel-specific pieces. */
struct TlsState {
    CtxtHandle ctx{};                  /* security context (per request) */
    bool have_ctx = false;             /* ctx has been initialized */
    SecPkgContext_StreamSizes sizes{}; /* header/trailer/max-message sizes */
    ULONG attrs = 0;                   /* context attributes (ISC output) */
    TimeStamp expiry{};                /* context expiry (ISC output) */
    std::wstring host_w;               /* UTF-16 server name (SNI + cert check) */
};

namespace {

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
 * chain against the Windows system stores (root + CA) and run the SSL chain
 * policy, which also performs the RFC 6125-style hostname match against the
 * requested server name. */
bool verifyServerCert(TlsState* s)
{
    PCCERT_CONTEXT remote = nullptr;
    if (QueryContextAttributesW(&s->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) != SEC_E_OK
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
        sslPara.pwszServerName = const_cast<LPWSTR>(s->host_w.c_str());
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

/* ---------- SChannel TLS drivers ---------- */

/* Drive the TLS handshake as far as it can go. Outbound handshake tokens are
 * appended to r->out (the core flushes them with hv_http_start_write). Returns:
 *   1        handshake complete (certificate verified, stream sizes queried)
 *   0        need more server data (or nothing new to do) -- wait for onRead
 *   negative fatal error code (the core finalizes with it) */
int tlsHandshake(heliosview_http_request* r, TlsState* s)
{
    /* Drive InitializeSecurityContext until it needs something new. The first
     * call runs with empty input to emit the ClientHello; during TLS 1.3 a
     * DecryptMessage may signal SEC_I_RENEGOTIATE to ask it to resume with the
     * (possibly empty) buffer to send the deferred client Finished, so we always
     * attempt a step here and let ISC reply CONTINUE_NEEDED when it's idle. */

    for (int calls = 0; calls < 64; ++calls) {
        SecBuffer inbufs[2] = {};
        inbufs[0].BufferType = SECBUFFER_TOKEN;
        inbufs[0].pvBuffer = r->tls_in.empty() ? nullptr : r->tls_in.data();
        inbufs[0].cbBuffer = (DWORD)r->tls_in.size();
        inbufs[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inDesc = { SECBUFFER_VERSION, 2, inbufs };

        SecBuffer outbufs[1] = {};
        outbufs[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outDesc = { SECBUFFER_VERSION, 1, outbufs };

        SECURITY_STATUS ss = InitializeSecurityContextW(
            static_cast<CredHandle*>(r->client->tls_ctx),
            s->have_ctx ? &s->ctx : nullptr,
            const_cast<SEC_WCHAR*>(s->host_w.c_str()),
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
                | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
            0, SECBUFFER_VERSION, &inDesc, 0, &s->ctx, &outDesc,
            &s->attrs, &s->expiry);
        s->have_ctx = true;

        /* Keep any leftover server data (e.g. an early response) for later. */
        if (inbufs[1].BufferType == SECBUFFER_EXTRA && inbufs[1].cbBuffer > 0) {
            /* SECBUFFER_EXTRA = unconsumed remainder. pvBuffer normally points at
             * it inside the input buffer, but SChannel can return pvBuffer == NULL,
             * in which case the leftover is the trailing `cbBuffer` bytes of the
             * current input (seen on the TLS 1.3 handshake-resume path). */
            const size_t avail = r->tls_in.size();
            char* src = inbufs[1].pvBuffer
                            ? static_cast<char*>(inbufs[1].pvBuffer)
                            : (inbufs[1].cbBuffer <= avail
                                   ? r->tls_in.data() + (avail - inbufs[1].cbBuffer)
                                   : r->tls_in.data());
            std::memmove(r->tls_in.data(), src, inbufs[1].cbBuffer);
            r->tls_in.resize(inbufs[1].cbBuffer);
        } else {
            r->tls_in.clear();
        }

        const bool produced = outbufs[0].cbBuffer > 0;
        if (produced)
            r->out.append(static_cast<const char*>(outbufs[0].pvBuffer), outbufs[0].cbBuffer);
        if (outbufs[0].pvBuffer)
            FreeContextBuffer(outbufs[0].pvBuffer);

        if (ss == SEC_E_OK) {
            if (QueryContextAttributesW(&s->ctx, SECPKG_ATTR_STREAM_SIZES,
                                        &s->sizes) != SEC_E_OK)
                return kTlsErrorBase + 1;
            if (!verifyServerCert(s))
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
int tlsWrite(heliosview_http_request* r, TlsState* s, const char* data, size_t len)
{
    const size_t maxMsg = s->sizes.cbMaximumMessage > 0 ? s->sizes.cbMaximumMessage : 16384;
    size_t off = 0;
    while (off < len) {
        const size_t chunk = std::min(len - off, maxMsg);
        std::string buf(s->sizes.cbHeader + chunk + s->sizes.cbTrailer, '\0');
        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = buf.data();
        bufs[0].cbBuffer = s->sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = buf.data() + s->sizes.cbHeader;
        bufs[1].cbBuffer = (DWORD)chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = buf.data() + s->sizes.cbHeader + chunk;
        bufs[2].cbBuffer = s->sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        std::memcpy(bufs[1].pvBuffer, data + off, chunk);
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
        const SECURITY_STATUS ss = EncryptMessage(&s->ctx, 0, &desc, 0);
        if (ss != SEC_E_OK)
            return tlsCode(ss);
        const size_t encLen = (size_t)bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        r->out.append(buf.data(), encLen);
        off += chunk;
    }
    return (int)off;
}

/* Decrypt as much buffered TLS record data as possible, feeding plaintext to
 * the http-parser. Returns true when the request finalized (the caller must
 * dispatch the response). */
bool tlsReadLoop(heliosview_http_request* r, TlsState* s)
{
    if (r->done.load())
        return false;
    for (;;) {
        if (r->tls_in.empty())
            return false;
        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = r->tls_in.data();
        bufs[0].cbBuffer = (DWORD)r->tls_in.size();
        for (int i = 1; i < 4; ++i)
            bufs[i].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
        DWORD qop = 0;
        const SECURITY_STATUS ss = DecryptMessage(&s->ctx, &desc, 0, &qop);
        if (ss == SEC_E_OK) {
            bool plain = false;
            for (int i = 1; i < 4; ++i) {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                    hv_http_parse_feed(r, static_cast<const char*>(bufs[i].pvBuffer),
                                       bufs[i].cbBuffer);
                    plain = true;
                }
            }
            if (plain) {
                if (r->parse_error)
                    return hv_http_finalize(r, -1);
                if (r->message_complete)
                    return hv_http_finalize(r, 0);
            }
            /* Keep the unconsumed remainder (if any) for the next call. */
            SecBuffer* extra = nullptr;
            for (int i = 1; i < 4; ++i)
                if (bufs[i].BufferType == SECBUFFER_EXTRA)
                    extra = &bufs[i];
            if (extra && extra->cbBuffer > 0) {
                const size_t avail = r->tls_in.size();
                char* src = extra->pvBuffer
                                ? static_cast<char*>(extra->pvBuffer)
                                : (extra->cbBuffer <= avail
                                       ? r->tls_in.data() + (avail - extra->cbBuffer)
                                       : r->tls_in.data());
                std::memmove(r->tls_in.data(), src, extra->cbBuffer);
                r->tls_in.resize(extra->cbBuffer);
            } else {
                r->tls_in.clear();
            }
            continue;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
            return false;
        if (ss == SEC_I_CONTEXT_EXPIRED) {
            /* Peer sent close_notify: finalize a connection-close-delimited body. */
            if (r->stage == Stage::Reading && !r->message_complete)
                hv_http_parse_feed(r, "", 0);
            return hv_http_finalize(r, (r->message_complete && !r->parse_error) ? 0 : -1);
        }
        if (ss == SEC_I_RENEGOTIATE) {
            /* TLS 1.3: SChannel can defer emitting the client's Finished (and any
             * other outbound handshake messages) until the first DecryptMessage,
             * signalling it here. Resume the handshake to produce and send those
             * tokens, then try to decrypt again. */
            const int rc = tlsHandshake(r, s);
            if (!r->out.empty() && !r->write_in_flight) {
                if (hv_http_start_write(r))
                    return true;
            }
            if (r->done.load())
                return false;
            if (rc == 1)
                continue; /* handshake closed out: decrypt the pending record */
            if (rc == 0)
                return false; /* still need more server data */
            return hv_http_finalize(r, rc); /* fatal */
        }
        return hv_http_finalize(r, tlsCode(ss));
    }
}

/* ---------- hv_tls_ops implementation ---------- */

void* ctx_create(heliosview_http_client* client)
{
    (void)client;

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

    SCH_CREDENTIALS credStruct{};
    credStruct.dwVersion = SCH_CREDENTIALS_VERSION;
    credStruct.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    credStruct.cTlsParameters = 1;
    credStruct.pTlsParameters = &tlsParams;

    auto* cred = hv::hv_alloc<CredHandle>();
    const SECURITY_STATUS ss = AcquireCredentialsHandleW(
        nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
        &credStruct, nullptr, nullptr, cred, nullptr);
    if (ss != SEC_E_OK) {
        hv::hv_dealloc(cred);
        return nullptr;
    }
    return cred;
}

void ctx_destroy(void* ctx)
{
    auto* cred = static_cast<CredHandle*>(ctx);
    FreeCredentialsHandle(cred);
    hv::hv_dealloc(cred);
}

void* session_create(heliosview_http_client* client, const char* host)
{
    (void)client;
    auto* s = hv::hv_alloc<TlsState>();
    s->host_w = toWide(host ? host : "");
    if (s->host_w.empty()) { /* host not convertible for SChannel (SNI + cert check) */
        hv::hv_dealloc(s);
        return nullptr;
    }
    return s;
}

void session_destroy(void* sess)
{
    auto* s = static_cast<TlsState*>(sess);
    if (s->have_ctx)
        DeleteSecurityContext(&s->ctx);
    hv::hv_dealloc(s);
}

int op_handshake(heliosview_http_request* r, void* sess)
{
    return tlsHandshake(r, static_cast<TlsState*>(sess));
}

int op_write(heliosview_http_request* r, void* sess, const char* data, size_t len)
{
    return tlsWrite(r, static_cast<TlsState*>(sess), data, len);
}

bool op_read(heliosview_http_request* r, void* sess)
{
    return tlsReadLoop(r, static_cast<TlsState*>(sess));
}

} // namespace

const hv_tls_ops* heliosview_tls_ops(void)
{
    static const hv_tls_ops ops = {
        &ctx_create,
        &ctx_destroy,
        &session_create,
        &session_destroy,
        &op_handshake,
        &op_write,
        &op_read,
    };
    return &ops;
}
