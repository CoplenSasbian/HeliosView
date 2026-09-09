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

// Open a local file or folder with its default application. Returns true on success.
inline bool openPath(const char* path)
{
    return heliosview_open_path(path) == 0;
}

inline bool openPath(const std::string& path)
{
    return heliosview_open_path(path.c_str()) == 0;
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

// Run an executable as a detached process. Returns true on success. `args` is
// passed to the program directly (not shell code). Any thread.
inline bool runProgram(const char* exe, const char* args = nullptr,
                       heliosview_program_show_t show = HELIOSVIEW_PROGRAM_SHOW_NORMAL)
{
    return heliosview_run_program(exe, args, show) == 0;
}

inline bool runProgram(const std::string& exe, const std::string& args = {},
                       heliosview_program_show_t show = HELIOSVIEW_PROGRAM_SHOW_NORMAL)
{
    return heliosview_run_program(exe.c_str(), args.empty() ? nullptr : args.c_str(), show) == 0;
}

// Run an executable and wait for it to exit. Returns true when it ran and exited
// (exit code in `exit_code`); false on failure to start. Call from a worker
// thread, never the message-loop thread.
inline bool runProgramWait(const char* exe, const char* args, int& exit_code,
                           heliosview_program_show_t show = HELIOSVIEW_PROGRAM_SHOW_NORMAL)
{
    return heliosview_run_program_wait(exe, args, show, &exit_code) == 0;
}

inline bool runProgramWait(const std::string& exe, const std::string& args, int& exit_code,
                           heliosview_program_show_t show = HELIOSVIEW_PROGRAM_SHOW_NORMAL)
{
    return heliosview_run_program_wait(exe.c_str(), args.empty() ? nullptr : args.c_str(),
                                       show, &exit_code) == 0;
}

// Run an executable with administrator rights (UAC on Windows). Returns true if it
// was launched.
inline bool runProgramElevated(const char* exe, const char* args = nullptr)
{
    return heliosview_run_program_elevated(exe, args) == 0;
}

inline bool runProgramElevated(const std::string& exe, const std::string& args = {})
{
    return heliosview_run_program_elevated(exe.c_str(), args.empty() ? nullptr : args.c_str()) == 0;
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

// Ensure per-monitor DPI awareness. Note: on supported platforms this is now
// initialized automatically upon window creation; calling it manually remains
// supported for explicit early initialization. Message-loop thread.
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

/* ---------- standard folders / OS info / power ---------- */

// Query a standard per-user folder. Returns true and sets `out` on success.
inline bool systemPath(heliosview_system_path_kind_t kind, std::string& out)
{
    char* path = nullptr;
    if (heliosview_system_path(kind, &path) != 1)
        return false;
    out = path ? path : "";
    heliosview_free(path);
    return true;
}

// A human-readable OS description ("" if unknown). Returns true when non-empty.
inline bool osVersion(std::string& out)
{
    char buf[256];
    if (heliosview_os_version(buf, sizeof(buf)) != 0)
        return false;
    out = buf;
    return !out.empty();
}

// Shut down / restart / log off the system. Returns true when accepted.
inline bool systemPower(heliosview_power_action_t action)
{
    return heliosview_system_power(action) == 0;
}

/* ---------- global hotkeys ---------- */

// A system-wide hotkey that fires even when the app has no focus, while the
// message-loop thread runs its loop. The callback runs on the message-loop
// thread. Returns true and sets `id` on success.
inline bool hotkeyRegister(const char* shortcut, heliosview_hotkey_cb cb,
                           void* userdata, uint32_t& id)
{
    id = heliosview_hotkey_register(shortcut, cb, userdata);
    return id != 0;
}

inline void hotkeyUnregister(uint32_t id)
{
    heliosview_hotkey_unregister(id);
}

} // namespace helios
