#pragma once

/**
 * HeliosView.Core -- Dialogs: native dialogs and message boxes.
 *
 * Free functions (no window-class coupling): the pickers take an optional parent
 * handle (from Window::nativeHandle(), or nullptr for an unparented dialog).
 * On win32 these are thin wrappers over the C API (IFileOpenDialog /
 * IFileSaveDialog / MessageBoxW). All functions are modal and must be called on
 * the message-loop thread.
 */

#include <HeliosView/heliosview.h>

#include <string>
#include <vector>

namespace helios {

/* ---------- message box ---------- */

// Message box type (mirrors heliosview_message_type_t)
enum class MessageBoxType : int32_t {
    Info = HELIOSVIEW_MESSAGE_INFO,
    Warning = HELIOSVIEW_MESSAGE_WARNING,
    Error = HELIOSVIEW_MESSAGE_ERROR,
    Question = HELIOSVIEW_MESSAGE_QUESTION,
};

// Button set (mirrors heliosview_message_buttons_t)
enum class MessageBoxButtons : int32_t {
    Ok = HELIOSVIEW_MESSAGE_OK,
    OkCancel = HELIOSVIEW_MESSAGE_OK_CANCEL,
    YesNo = HELIOSVIEW_MESSAGE_YES_NO,
    YesNoCancel = HELIOSVIEW_MESSAGE_YES_NO_CANCEL,
    RetryCancel = HELIOSVIEW_MESSAGE_RETRY_CANCEL,
};

// Which button the user pressed (mirrors heliosview_message_result_t)
enum class MessageBoxResult : int32_t {
    None = HELIOSVIEW_MESSAGE_RESULT_NONE,
    Ok = HELIOSVIEW_MESSAGE_RESULT_OK,
    Cancel = HELIOSVIEW_MESSAGE_RESULT_CANCEL,
    Yes = HELIOSVIEW_MESSAGE_RESULT_YES,
    No = HELIOSVIEW_MESSAGE_RESULT_NO,
    Retry = HELIOSVIEW_MESSAGE_RESULT_RETRY,
};

// Show a modal message box; returns the button the user pressed.
inline MessageBoxResult messageBox(heliosview_window_t* parent, MessageBoxType type,
                                   MessageBoxButtons buttons, const char* title,
                                   const char* message)
{
    return static_cast<MessageBoxResult>(heliosview_message_box(
        parent, static_cast<heliosview_message_type_t>(type),
        static_cast<heliosview_message_buttons_t>(buttons), title, message));
}

// Convenience overloads (unparented, default OK button).
inline MessageBoxResult messageBox(MessageBoxType type, const char* title, const char* message)
{
    return messageBox(nullptr, type, MessageBoxButtons::Ok, title, message);
}

/* ---------- folder picker ---------- */

// Open a native folder-picker dialog (modal, optionally parented to `parent`).
// Returns true when the user picked a folder (out_path receives its UTF-8 path),
// false on cancel or failure.
inline bool selectFolder(heliosview_window_t* parent, const char* title, std::string& out_path)
{
    char* path = nullptr;
    const int rc = heliosview_select_folder(parent, title, &path);
    if (rc <= 0)
        return false;
    out_path = path ? path : "";
    heliosview_free(path);
    return true;
}

// Convenience: an unparented folder picker.
inline bool selectFolder(const char* title, std::string& out_path)
{
    return selectFolder(nullptr, title, out_path);
}

/* ---------- file pickers ---------- */

// Open-file dialog. `filter` uses the "Name1 (*.ext)|*.ext|Name2|..." format
// (nullptr = all files); `multi` enables multi-selection. Returns the chosen
// path(s), or an empty vector on cancel / failure. UTF-8 paths.
inline std::vector<std::string> openFiles(heliosview_window_t* parent, const char* title,
                                          const char* filter = nullptr, bool multi = false)
{
    char** paths = nullptr;
    std::vector<std::string> out;
    const int n = heliosview_open_files(parent, title, filter, multi ? 1 : 0, &paths);
    if (n > 0 && paths) {
        out.reserve(static_cast<size_t>(n));
        for (char** p = paths; *p; ++p)
            out.emplace_back(*p);
        heliosview_free_paths(paths);
    }
    return out;
}

// Convenience: an unparented open-file dialog.
inline std::vector<std::string> openFiles(const char* title, const char* filter = nullptr,
                                          bool multi = false)
{
    return openFiles(nullptr, title, filter, multi);
}

// Save-file dialog. Returns true when the user chose a path (out_path receives
// its UTF-8 path), false on cancel / failure.
inline bool saveFile(heliosview_window_t* parent, const char* title, const char* filter,
                     const char* default_name, std::string& out_path)
{
    char* path = nullptr;
    const int rc = heliosview_save_file(parent, title, filter, default_name, &path);
    if (rc <= 0)
        return false;
    out_path = path ? path : "";
    heliosview_free(path);
    return true;
}

// Convenience: an unparented save-file dialog.
inline bool saveFile(const char* title, const char* filter, const char* default_name,
                     std::string& out_path)
{
    return saveFile(nullptr, title, filter, default_name, out_path);
}

} // namespace helios
