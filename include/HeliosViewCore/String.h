#pragma once

/**
 * HeliosView.Core -- String: UTF-8 <-> wchar_t conversion helpers.
 *
 * Thin wrappers over the C layer's two-phase codecs heliosview_utf8_to_wide /
 * heliosview_wide_to_utf8 (see heliosview.h): the required size is queried
 * first (NULL output), then the std::string / std::wstring is resized to exactly
 * that and filled in place -- one allocation, no intermediate buffer, no extra
 * copy. On Windows wchar_t is UTF-16; on platforms where wchar_t is 32-bit
 * (Linux) the conversion is UTF-8 <-> UTF-32, matching wchar_t's native
 * encoding. Invalid input sequences are replaced with U+FFFD.
 *
 *   window.setTitle(helios::utf8ToWide(title));   // std::string -> wchar_t
 *   std::string s = helios::wideToUtf8(tooltip);  // wchar_t -> std::string
 */

#include <HeliosView/heliosview.h>

#include <cstddef>
#include <string>

namespace helios {

// Convert UTF-8 bytes to a wide string. utf8_len may be (size_t)-1 to read a
// NUL-terminated input. Returns an empty wide string on failure.
inline std::wstring utf8ToWide(const char* utf8, size_t utf8_len)
{
    if (!utf8)
        return {};
    const size_t n = heliosview_utf8_to_wide(utf8, utf8_len, nullptr); /* incl. NUL */
    if (n == 0)
        return {};
    std::wstring result(n, L'\0');
    heliosview_utf8_to_wide(utf8, utf8_len, result.data()); /* fills n - 1 chars + NUL */
    result.resize(n - 1); /* drop the NUL; no reallocation */
    return result;
}

// Convenience: whole std::string (embedded NULs are preserved).
inline std::wstring utf8ToWide(const std::string& utf8)
{
    return utf8ToWide(utf8.data(), utf8.size());
}

// Convert a wide string to UTF-8 bytes. wide_len may be (size_t)-1 to read a
// NUL-terminated input. Returns an empty string on failure.
inline std::string wideToUtf8(const wchar_t* wide, size_t wide_len)
{
    if (!wide)
        return {};
    const size_t n = heliosview_wide_to_utf8(wide, wide_len, nullptr); /* incl. NUL */
    if (n == 0)
        return {};
    std::string result(n, '\0');
    heliosview_wide_to_utf8(wide, wide_len, result.data()); /* fills n - 1 bytes + NUL */
    result.resize(n - 1); /* drop the NUL; no reallocation */
    return result;
}

// Convenience: whole std::wstring (embedded NULs are preserved).
inline std::string wideToUtf8(const std::wstring& wide)
{
    return wideToUtf8(wide.data(), wide.size());
}

} // namespace helios
