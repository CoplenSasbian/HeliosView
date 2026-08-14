// HeliosView.dll - platform-independent core: version, allocator, events, conversion-delegate registration.
// Platform-specific implementation lives in src/win32/ (window/message loop, WebView2, dialogs, toasts).
#include <HeliosView/heliosview.h>
#include "heliosview_internal.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>

/* ================= Version ================= */

const char* heliosview_version(void)
{
    return HELIOSVIEW_VERSION_STR;
}

/* ================= Memory allocation ================= */

void heliosview_set_allocator(const heliosview_allocator_t* allocator)
{
    if (allocator)
        hv::g_allocator = *allocator;
    else
        hv::g_allocator = heliosview_allocator_t{}; /* restore malloc/free */
}

void heliosview_free(void* ptr)
{
    if (!ptr)
        return;
    if (hv::g_allocator.free_)
        hv::g_allocator.free_(ptr, hv::g_allocator.context);
    else
        std::free(ptr);
}

/* ================= Event queue (thread-local) =================
 * The queue is thread-local: it lives on the message-loop thread (WndProc posts
 * and poll/wait consume on the same thread). Cross-thread event access is not
 * supported. */

int heliosview_poll(heliosview_event_t* out_event)
{
    if (!out_event)
        return 0;
    if (hv::g_queue.empty())
        return 0;
    *out_event = hv::g_queue.front();
    hv::g_queue.pop_front();
    return 1;
}

int heliosview_wait(heliosview_event_t* out_event)
{
    if (!out_event)
        return 0;
    while (hv::g_queue.empty() && !hv::g_quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (hv::g_queue.empty())
        return -1; /* quit requested and queue empty */
    *out_event = hv::g_queue.front();
    hv::g_queue.pop_front();
    return 1;
}

void heliosview_post_event(const heliosview_event_t* event)
{
    if (!event)
        return;
    heliosview_event_t copy = *event;
    if (copy.timestamp_ms == 0)
        copy.timestamp_ms = hv::now_ms();
    hv::queue_push(copy);
}

void heliosview_quit(void)
{
    hv::g_quit = true;
    if (hv::g_platform_wake)
        hv::g_platform_wake();
}

void heliosview_wake_loop(void)
{
    if (hv::g_platform_wake)
        hv::g_platform_wake();
}

/* ================= Conversion delegates ================= */

/* Register `handler`. It is tried after the library's built-in default conversion
 * (which always runs first); the first converter returning 1 or 0 wins. Returns an
 * id (0 = failure, e.g. null handler). */
uint32_t heliosview_add_native_handler(heliosview_native_handler_fn handler)
{
    if (!handler)
        return 0;
    const uint32_t id = hv::g_next_handler_id.fetch_add(1);
    hv::g_native_handlers[id] = handler;
    return id;
}

/* Remove a handler previously registered with heliosview_add_native_handler.
 * 0 = success, negative = the id is not registered. */
int heliosview_remove_native_handler(uint32_t id)
{
    return hv::g_native_handlers.erase(id) ? 0 : -1;
}

/* ================= String conversion (UTF-8 <-> wchar_t) =================
 * Portable codec, no OS calls: wchar_t is UTF-16 on Windows (16-bit) and
 * wchar_t's native encoding (UTF-32) where wchar_t is 32-bit. Invalid input
 * sequences are replaced with U+FFFD, matching the lenient behavior of the OS
 * converters.
 *
 * Two-phase API (no intermediate buffer): call with a NULL output to get the
 * required element count (including the NUL), then call again with a buffer of
 * that size to fill it. The query pass returns 0 on failure; the fill pass
 * returns the element count written, excluding the NUL. */

namespace {

/* Bytes of the UTF-8 sequence starting at a leading byte c, or 0 when c is not
 * a valid leading byte. */
size_t utf8_seq_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 0;
}

/* Decode one code point from the UTF-8 bytes at p (p points at a leading byte,
 * with `avail` bytes available). Writes the bytes consumed to *consumed (1 for
 * an invalid sequence, which yields U+FFFD). */
uint32_t utf8_decode(const char* p, size_t avail, size_t* consumed)
{
    const unsigned char c = static_cast<unsigned char>(*p);
    const size_t n = utf8_seq_len(c);
    if (n == 0 || n > avail) {
        *consumed = 1;
        return 0xFFFDu;
    }
    for (size_t i = 1; i < n; ++i)
        if ((static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) {
            *consumed = 1;
            return 0xFFFDu;
        }
    uint32_t cp;
    if (n == 1)
        cp = c;
    else if (n == 2)
        cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(static_cast<unsigned char>(p[1]) & 0x3F);
    else if (n == 3)
        cp = ((uint32_t)(c & 0x0F) << 12)
             | ((uint32_t)(static_cast<unsigned char>(p[1]) & 0x3F) << 6)
             | (uint32_t)(static_cast<unsigned char>(p[2]) & 0x3F);
    else
        cp = ((uint32_t)(c & 0x07) << 18)
             | ((uint32_t)(static_cast<unsigned char>(p[1]) & 0x3F) << 12)
             | ((uint32_t)(static_cast<unsigned char>(p[2]) & 0x3F) << 6)
             | (uint32_t)(static_cast<unsigned char>(p[3]) & 0x3F);
    const size_t min = (n == 2) ? 0x80 : (n == 3) ? 0x800 : (n == 4) ? 0x10000 : 0;
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        *consumed = 1; /* overlong / out of range / surrogate */
        return 0xFFFDu;
    }
    *consumed = n;
    return cp;
}

size_t utf8_encoded_len(uint32_t cp)
{
    if (cp <= 0x7Fu)
        return 1;
    if (cp <= 0x7FFu)
        return 2;
    if (cp <= 0xFFFFu)
        return 3;
    return 4;
}

void utf8_encode(uint32_t cp, char* out)
{
    if (cp <= 0x7Fu) {
        out[0] = static_cast<char>(cp);
    } else if (cp <= 0x7FFu) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFFu) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/* Read the next code point from `wide`, advancing *i by the number of code
 * units consumed. Lone surrogates and out-of-range values yield U+FFFD. */
uint32_t wide_next(const wchar_t* wide, size_t wide_len, size_t* i)
{
    uint32_t cp = (uint32_t)(unsigned)wide[*i];
    if (sizeof(wchar_t) == 2 && cp >= 0xD800u && cp <= 0xDBFFu) {
        /* high surrogate: combine with a following low surrogate */
        if (*i + 1 < wide_len) {
            const uint32_t lo = (uint32_t)(unsigned)wide[*i + 1];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                *i += 1;
            } else {
                cp = 0xFFFDu;
            }
        } else {
            cp = 0xFFFDu;
        }
    } else if (sizeof(wchar_t) == 2 && cp >= 0xDC00u && cp <= 0xDFFFu) {
        cp = 0xFFFDu; /* lone low surrogate */
    }
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        cp = 0xFFFDu; /* 32-bit wchar_t: out of range / stray surrogate */
    *i += 1;
    return cp;
}

} // namespace

size_t heliosview_utf8_to_wide(const char* utf8, size_t utf8_len, wchar_t* out_wide)
{
    if (!utf8)
        return 0;
    if (utf8_len == (size_t)-1)
        utf8_len = std::strlen(utf8);

    /* wchar_t code units needed (a surrogate pair when wchar_t is 16-bit and the
     * code point exceeds U+FFFF), including the terminating NUL. */
    size_t units = 1;
    for (size_t i = 0; i < utf8_len;) {
        size_t consumed = 0;
        const uint32_t cp = utf8_decode(utf8 + i, utf8_len - i, &consumed);
        i += consumed;
        units += (sizeof(wchar_t) == 2 && cp > 0xFFFFu) ? 2 : 1;
    }

    if (!out_wide)
        return units;

    size_t o = 0;
    for (size_t i = 0; i < utf8_len;) {
        size_t consumed = 0;
        const uint32_t cp = utf8_decode(utf8 + i, utf8_len - i, &consumed);
        i += consumed;
        if (sizeof(wchar_t) == 2 && cp > 0xFFFFu) {
            const uint32_t v = cp - 0x10000u;
            out_wide[o++] = static_cast<wchar_t>(0xD800u + (v >> 10));
            out_wide[o++] = static_cast<wchar_t>(0xDC00u + (v & 0x3FFu));
        } else {
            out_wide[o++] = static_cast<wchar_t>(cp);
        }
    }
    out_wide[o] = 0;
    return o;
}

size_t heliosview_wide_to_utf8(const wchar_t* wide, size_t wide_len, char* out_utf8)
{
    if (!wide)
        return 0;
    if (wide_len == (size_t)-1)
        wide_len = std::wcslen(wide);

    /* UTF-8
     c ount, including the terminating NUL. */
    size_t bytes = 1;
    for (size_t i = 0; i < wide_len;) {
        const uint32_t cp = wide_next(wide, wide_len, &i);
        bytes += utf8_encoded_len(cp);
    }

    if (!out_utf8)
        return bytes;

    size_t o = 0;
    for (size_t i = 0; i < wide_len;) {
        const uint32_t cp = wide_next(wide, wide_len, &i);
        utf8_encode(cp, out_utf8 + o);
        o += utf8_encoded_len(cp);
    }
    out_utf8[o] = '\0';
    return o;
}
