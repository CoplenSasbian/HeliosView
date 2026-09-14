#ifndef HELIOSVIEW_HELIOSVIEW_CORE_H
#define HELIOSVIEW_HELIOSVIEW_CORE_H

/**
 * HeliosView C API -- core: version, errors, allocation, string codecs
 *
 * The always-present foundation of the library: the version string, the error
 * reporting convention every function follows, the configurable allocator, and the
 * UTF-8 <-> wchar_t codecs. Nothing here needs a window, a WebView or a canvas.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Version ================= */

HELIOSVIEW_API const char* heliosview_version(void);

/* Backend identity: "win32", "macos", "linux", or "portable" (this platform has
 * no backend yet — every feature then reports HELIOSVIEW_ERROR_UNSUPPORTED).
 * Static string, never NULL. Diagnostic / test output. */
HELIOSVIEW_API const char* heliosview_backend_name(void);

/* ================= Error reporting =================
 *
 * Return convention (every function): 0 = success, < 0 = an error code,
 * > 0 = a payload / count (e.g. number of items or characters).
 *
 * Standard error codes:
 *     0   HELIOSVIEW_SUCCESS: operation completed successfully
 *    -1   HELIOSVIEW_ERROR_GENERIC: generic failure — invalid/missing argument,
 *         underlying platform call failed with no specific code, or called on the wrong thread
 *    -2   HELIOSVIEW_ERROR_INVALID_ARGUMENT: invalid argument or name (e.g. not a valid C identifier)
 *    -3   HELIOSVIEW_WEBVIEW_DESTROYED: the WebView instance was already destroyed
 *    -4   HELIOSVIEW_ERROR_UNSUPPORTED: the platform or OS version cannot provide this
 *         feature (e.g. a Mica backdrop on Windows 10, any Windows-only feature on
 *         another platform). Safe to ignore: callers degrade to their fallback.
 *
 * Other error codes are platform codes:
 *   - a small negative value (e.g. -5) is the negated OS error status (Win32
 *     GetLastError);
 *   - a large positive value (e.g. 2147467259) is the negated HRESULT as returned
 *     by COM / WebView2 / DWM failures (E_FAIL 0x80004005 surfaces as 2147467259).
 * A negated platform code can collide numerically with the reserved codes above;
 * read heliosview_last_error_string for the actual reason.
 *
 * Async completion callbacks (error != 0) use the same code space.
 *
 * For the descriptive reason behind the most recent failure on this thread, use
 * heliosview_last_error / heliosview_last_error_string — every failing call
 * records a failure-site message.
 * Note: heliosview_wait / heliosview_poll and dialog functions return small
 * tri-state status codes (1/0) documented per function.
 */

/* The standard codes are defined in heliosview_base.h, included above -- together
 * with heliosview_rect_t and the opaque heliosview_window_t. */
/* (Defined in heliosview_base.h, included above, together with heliosview_rect_t
 * and the opaque heliosview_window_t.) */

/* The error code recorded by the most recent failing library call on this
 * thread (0 = no error recorded). Meaningful only immediately after a call
 * returned < 0 (or a NULL handle). */
HELIOSVIEW_API int heliosview_last_error(void);

/* The failure-site message recorded for heliosview_last_error: write it into
 * buf (always NUL-terminated, truncated to fit `size`; empty string when no
 * error was recorded). It answers "why did the last call fail" (the context at
 * the failure point, e.g. which operation / argument). The error code itself
 * is available via heliosview_last_error; decoding it to a platform message is
 * left to the caller (e.g. FormatMessage on Windows).
 * 0 = success, negative = invalid arguments (buf == NULL or size == 0). */
HELIOSVIEW_API int heliosview_last_error_string(char* buf, size_t size);

/* ================= Memory allocation =================
 *
 * The library allocates its internal objects (windows, webviews, WebView2
 * callback stubs, dialog results, ...) through a configurable allocator, so a
 * C app can supply its own memory management (e.g. a pool or arena) instead of
 * the process heap. Defaults to the standard allocator (malloc / free).
 *
 * Set it once, before any other library call. The allocator is read by
 * subsequent allocations; changing it while objects are alive is undefined
 * (memory must be freed with the same allocator that allocated it).
 */

typedef void* (*heliosview_alloc_fn)(size_t size, void* context);
typedef void  (*heliosview_free_fn)(void* ptr, void* context);

typedef struct heliosview_allocator {
    heliosview_alloc_fn alloc;  /* allocate `size` bytes, aligned for any object; NULL = malloc */
    heliosview_free_fn  free_;  /* free a pointer returned by `alloc`; NULL = free */
    void* context;              /* opaque, passed unchanged to alloc/free */
} heliosview_allocator_t;

/* Set the default allocator (NULL restores malloc/free). Not thread-safe while allocations are live. */
HELIOSVIEW_API void heliosview_set_allocator(const heliosview_allocator_t* allocator);

/* Free memory the library allocated (paths from the dialog APIs, clipboard text,
 * ...). Always pair a library-returned pointer with this, never the platform's
 * free(): the library may allocate through its configured allocator, and freeing
 * across CRT boundaries on Windows is undefined. NULL is ignored. Thread-safe. */
HELIOSVIEW_API void heliosview_free(void* ptr);

/* ================= String conversion (UTF-8 <-> UTF-16) =================
 *
 * Two-phase codecs for consumers that need to convert the library's UTF-8
 * strings to/from wchar_t (on Windows wchar_t is UTF-16; on platforms where
 * wchar_t is 32-bit the conversion is UTF-8 <-> wchar_t's native encoding).
 * The library itself stores UTF-8 and converts internally at platform boundaries.
 *
 * Call each function twice to convert without an intermediate buffer:
 *   size_t n = heliosview_utf8_to_wide(utf8, utf8_len, nullptr);  // required wchar_t count (incl. NUL), 0 = failure
 *   wchar_t buf[n];
 *   heliosview_utf8_to_wide(utf8, utf8_len, buf);                 // fills buf, NUL-terminated
 *
 * Input lengths are explicit; pass (size_t)-1 to read a NUL-terminated input.
 * Return value: with a NULL output, the required element count INCLUDING the
 * terminating NUL (0 = failure); with a non-NULL output, the element count
 * written EXCLUDING the NUL (the buffer must hold at least the previously
 * returned count). Invalid input sequences are replaced with U+FFFD.
 */

/* Convert UTF-8 bytes to wchar_t (see the two-phase contract above). */
HELIOSVIEW_API size_t heliosview_utf8_to_wide(const char* utf8, size_t utf8_len,
                                              wchar_t* out_wide);

/* Convert wchar_t to UTF-8 bytes (see the two-phase contract above). */
HELIOSVIEW_API size_t heliosview_wide_to_utf8(const wchar_t* wide, size_t wide_len,
                                              char* out_utf8);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_CORE_H */
