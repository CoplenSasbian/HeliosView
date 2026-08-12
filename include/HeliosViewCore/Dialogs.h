#pragma once

/**
 * HeliosView.Core -- Dialogs: native dialog helpers.
 *
 * Free functions (no window-class coupling): the folder picker takes an optional
 * parent handle (from Window::nativeHandle(), or nullptr for an unparented
 * dialog). On win32 these are thin wrappers over the C API's
 * heliosview_select_folder (IFileOpenDialog with FOS_PICKFOLDERS).
 *
 * All functions must be called on the message-loop thread.
 */

#include <HeliosView/heliosview.h>

#include <cstdlib>
#include <string>

namespace helios {

// Open a native folder-picker dialog (modal, optionally parented to `parent`,
// which may be nullptr). Returns true when the user picked a folder (out_path
// receives its absolute UTF-8 path), false on cancel or failure.
inline bool selectFolder(heliosview_window_t* parent, const char* title, std::string& out_path)
{
    char* path = nullptr;
    const int rc = heliosview_select_folder(parent, title, &path);
    if (rc <= 0)
        return false;
    out_path = path ? path : "";
    std::free(path);
    return true;
}

// Convenience: an unparented folder picker.
inline bool selectFolder(const char* title, std::string& out_path)
{
    return selectFolder(nullptr, title, out_path);
}

} // namespace helios
