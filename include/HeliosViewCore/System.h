#pragma once

/**
 * HeliosView.Core -- System: small OS helpers (open URL, show in folder,
 * clipboard). Thin wrappers over the C API. All functions must be called on the
 * message-loop thread.
 */

#include <HeliosView/heliosview.h>

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

} // namespace helios
