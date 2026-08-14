#pragma once

/**
 * HeliosView.Core -- System: small OS helpers (open URL, show in folder,
 * clipboard). Thin wrappers over the C API. All functions must be called on the
 * message-loop thread.
 */

#include <HeliosView/heliosview.h>

#include <cstdint>
#include <string>

namespace helios {

// Open a URL in the default browser. Returns true on success.
inline bool openUrl(const char* url)
{
    return heliosview_open_url(url) == 0;
}

// Open a URL in the default browser (std::string overload).
inline bool openUrl(const std::string& url)
{
    return heliosview_open_url(url.c_str()) == 0;
}

// Reveal a file/folder in Explorer, selecting it. Returns true on success.
inline bool showInFolder(const char* path)
{
    return heliosview_show_in_folder(path) == 0;
}

inline bool showInFolder(const std::string& path)
{
    return heliosview_show_in_folder(path.c_str()) == 0;
}

// Copy UTF-8 text to the clipboard. Returns true on success.
inline bool clipboardSetText(const char* text)
{
    return heliosview_clipboard_set_text(text) == 0;
}

inline bool clipboardSetText(const std::string& text)
{
    return heliosview_clipboard_set_text(text.c_str()) == 0;
}

// Read UTF-8 clipboard text. Returns true when text was present (out receives it).
inline bool clipboardGetText(std::string& out)
{
    char* text = nullptr;
    const int rc = heliosview_clipboard_get_text(&text);
    if (rc <= 0)
        return false;
    out = text ? text : "";
    heliosview_free(text);
    return true;
}

// Make the process per-monitor DPI aware (v2). Call once, before creating any
// window. Returns true on success (or when already set). Message-loop thread.
inline bool enableDpiAwareness()
{
    return heliosview_set_dpi_awareness() == 0;
}

/* ---------- session end (shutdown / logoff) ---------- */

// Register a callback invoked synchronously on the message-loop thread when the
// OS session is ending (shutdown / restart / logoff), before it actually ends.
// Return non-zero to veto the shutdown (0 = allow). Pass nullptr to unregister.
inline void setSessionEndCallback(heliosview_session_end_cb callback, void* userdata = nullptr)
{
    heliosview_set_session_end_callback(callback, userdata);
}

/* ---------- screen / monitor geometry ---------- */

// A rectangle in screen coordinates (mirrors heliosview_rect_t).
struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

inline Rect toRect(const heliosview_rect_t& r)
{
    return {r.x, r.y, r.width, r.height};
}

// Work area (excluding taskbar) of the monitor containing the given screen point.
inline bool screenWorkArea(int32_t x, int32_t y, Rect& out)
{
    heliosview_rect_t r{};
    if (heliosview_screen_work_area(x, y, &r) != 0)
        return false;
    out = toRect(r);
    return true;
}

// Work area of the primary monitor.
inline bool primaryWorkArea(Rect& out)
{
    heliosview_rect_t r{};
    if (heliosview_primary_work_area(&r) != 0)
        return false;
    out = toRect(r);
    return true;
}

// The cursor's position in screen coordinates.
inline bool cursorPosition(int32_t& x, int32_t& y)
{
    return heliosview_cursor_position(&x, &y) == 0;
}

} // namespace helios
