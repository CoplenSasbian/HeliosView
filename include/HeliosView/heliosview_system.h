#ifndef HELIOSVIEW_HELIOSVIEW_SYSTEM_H
#define HELIOSVIEW_HELIOSVIEW_SYSTEM_H

/**
 * HeliosView C API -- system helpers and global hotkeys
 *
 * Everything the OS does for the application that is not a window: opening URLs and
 * paths, running programs, the clipboard, standard folders, OS information, power
 * actions, and process-wide hotkeys.
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>
#include <HeliosView/heliosview_dialogs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= System helpers ================= */

/* Open a URL in the default browser. 0 = success, negative = error. Message-loop thread. */
HELIOSVIEW_API int heliosview_open_url(const char* url);

/* Open a local file or folder with its default application (the OS picks the
 * handler; this is not a process launcher). Unlike open_url this takes a
 * filesystem path and needs no file:// URI — the two are one family: URLs get
 * open_url, paths get open_path. Win32: ShellExecuteW("open"); macOS:
 * NSWorkspace openFile:; Linux: xdg-open. 0 = success, negative = error.
 * Any thread. */
HELIOSVIEW_API int heliosview_open_path(const char* path);

/* Reveal a file/folder in Explorer, selecting it. 0 = success. Message-loop thread. */
HELIOSVIEW_API int heliosview_show_in_folder(const char* path);

/* How the window of a newly started process should start (Windows only; other
 * platforms ignore the hint and start the process normally). */
typedef enum heliosview_program_show {
    HELIOSVIEW_PROGRAM_SHOW_NORMAL = 0, /* start with the window visible (default) */
    HELIOSVIEW_PROGRAM_SHOW_HIDDEN,     /* start with the window hidden (Win32 SW_HIDE) */
} heliosview_program_show_t;

/* Run an executable as a new, detached process (no waiting, no output
 * capture). `exe` is the executable to run — a bare name (e.g. "tool") is
 * resolved via the platform's PATH search, a full/relative path is pinned
 * exactly; `args` is the raw command-line text passed to it (NULL/empty =
 * none). `args` is NOT shell code: no shell parsing, quoting, redirection or
 * env expansion happens (quote arguments containing spaces yourself). This is
 * a process launcher, not a document opener: it runs executables directly, so
 * .bat/.cmd/.lnk need cmd.exe / the shell. 0 = success, negative = error.
 * Any thread. */
HELIOSVIEW_API int heliosview_run_program(const char* exe, const char* args,
                                          heliosview_program_show_t show);

/* Like heliosview_run_program, but blocks until the child exits and writes its
 * exit code to *out_exit_code (unchanged/`-1` when NULL or on failure). Call it
 * from a worker thread, never the message-loop thread. 0 = the process ran and
 * exited (exit code is valid), negative = failed to start. Win32:
 * CreateProcessW + WaitForSingleObject; POSIX: posix_spawn + waitpid. */
HELIOSVIEW_API int heliosview_run_program_wait(const char* exe, const char* args,
                                               heliosview_program_show_t show,
                                               int* out_exit_code);

/* Run an executable with administrator rights. Win32 uses the ShellExecute
 * "runas" verb (a UAC prompt appears; cancelling it is reported as an error).
 * This launches and returns; it does not wait. macOS/Linux have no standard
 * equivalent and return HELIOSVIEW_ERROR_UNSUPPORTED. */
HELIOSVIEW_API int heliosview_run_program_elevated(const char* exe, const char* args);

/* Copy UTF-8 text to the clipboard. 0 = success. Message-loop thread. */
HELIOSVIEW_API int heliosview_clipboard_set_text(const char* text);

/* Read UTF-8 clipboard text: 1 = text written to out (heliosview_free), 0 = no
 * text, negative = error. Message-loop thread. */
HELIOSVIEW_API int heliosview_clipboard_get_text(char** out);

/* Where to put per-user app data (config / logs / cache / temp) — the "where do
 * my files live" helper. The mapping is stable across platforms: freedesktop
 * XDG base dirs on Linux, NSSearchPathForDirectoriesInDomains on macOS,
 * SHGetKnownFolderPath on Windows. Caller appends its own application subfolder
 * to the returned value. */
typedef enum heliosview_system_path_kind {
    HELIOSVIEW_SYSTEM_PATH_HOME = 0,      /* the user's home directory */
    HELIOSVIEW_SYSTEM_PATH_DOCUMENTS,     /* the user's Documents folder */
    HELIOSVIEW_SYSTEM_PATH_DOWNLOADS,     /* the user's Downloads folder */
    HELIOSVIEW_SYSTEM_PATH_DESKTOP,       /* the user's Desktop folder */
    HELIOSVIEW_SYSTEM_PATH_APPDATA,       /* roaming per-user config (Win: RoamingAppData, mac: Application Support, Linux: $XDG_CONFIG_HOME) */
    HELIOSVIEW_SYSTEM_PATH_LOCAL_DATA,    /* non-roaming per-user data (Win: LocalAppData, mac: Application Support, Linux: $XDG_DATA_HOME) */
    HELIOSVIEW_SYSTEM_PATH_CACHE,         /* per-user cache (Win: LocalAppData, mac: Caches, Linux: $XDG_CACHE_HOME) */
    HELIOSVIEW_SYSTEM_PATH_TEMP,          /* the OS temporary directory */
} heliosview_system_path_kind_t;

/* Query a standard folder. Returns 1 = path written to *out (heliosview_free it),
 * 0 = no such folder on this system/user (out = NULL), negative = error. */
HELIOSVIEW_API int heliosview_system_path(heliosview_system_path_kind_t kind, char** out);

/* Human-readable OS description, e.g. "Windows 11 Pro 24H2 (build 26100)" or
 * "macOS 14.6"; writes "" when it cannot be determined. Win32: registry
 * ProductName/DisplayVersion; macOS: NSProcessInfo; Linux: /etc/os-release.
 * 0 = success. */
HELIOSVIEW_API int heliosview_os_version(char* buf, size_t size);

/* Shut down / restart / log off the system. Win32: ExitWindowsEx; macOS: an
 * AppleEvent to System Events (may prompt for Automation permission); Linux:
 * org.freedesktop.login1 (systemd; may prompt through polkit). 0 = accepted,
 * negative = error. */
typedef enum heliosview_power_action {
    HELIOSVIEW_POWER_SHUTDOWN = 0,
    HELIOSVIEW_POWER_REBOOT,
    HELIOSVIEW_POWER_LOGOFF,
} heliosview_power_action_t;

HELIOSVIEW_API int heliosview_system_power(heliosview_power_action_t action);

/* ================= Global hotkeys =================
 *
 * System-wide hotkeys: they fire even when the application has no focus, as
 * long as the message-loop thread is running its loop. Register and unregister
 * on the message-loop thread; the callback also runs there. The shortcut string
 * uses the same grammar as action shortcuts ("Ctrl+Shift+F", "Primary+Z",
 * "F11"; Primary/Cmd = Control here, Command on macOS). Registering an
 * already-taken combination is an error on Windows.
 *
 * Win32: RegisterHotKey + WM_HOTKEY (a library-owned message-only window).
 * macOS: RegisterEventHotKey. Linux has no universal global-hotkey API:
 * registration returns 0 with HELIOSVIEW_ERROR_UNSUPPORTED. */

typedef void (*heliosview_hotkey_cb)(uint32_t hotkey_id, void* userdata);

/* Register a global hotkey. Returns a nonzero id on success, 0 on failure
 * (last error set — see heliosview_last_error). */
HELIOSVIEW_API uint32_t heliosview_hotkey_register(const char* shortcut,
                                                   heliosview_hotkey_cb cb, void* userdata);

/* Unregister a hotkey. Safe to call while the loop is running; it stops firing
 * from the next loop iteration. Passing a stale id is a no-op. */
HELIOSVIEW_API void heliosview_hotkey_unregister(uint32_t hotkey_id);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_SYSTEM_H */
