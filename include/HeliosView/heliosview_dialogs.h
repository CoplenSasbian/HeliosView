#ifndef HELIOSVIEW_HELIOSVIEW_DIALOGS_H
#define HELIOSVIEW_HELIOSVIEW_DIALOGS_H

/**
 * HeliosView C API -- native dialogs
 *
 * Folder / file pickers and the message box. File paths come back as UTF-8 strings
 * that must be released with heliosview_free (heliosview_core.h).
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

/* ================= Native dialogs =================
 *
 * All dialogs are modal and must be called on the message-loop thread. They take
 * an optional parent window (NULL = unparented). A selected path is returned as
 * a UTF-8 string allocated by the library; free it with heliosview_free. Return
 * values: 1 = a result was produced, 0 = cancelled, negative = error. */

/* Folder picker. On success (1) out_path receives the selected folder (heliosview_free). */
HELIOSVIEW_API int heliosview_select_folder(heliosview_window_t* window,
                                            const char* title,
                                            char** out_path);

/* File dialog filter rule (display name and semicolon-separated extensions) */
typedef struct heliosview_file_filter {
    const char* name;        /* filter display name, e.g. "Image files" or "Text documents" */
    const char* extensions;  /* semicolon-separated extensions, e.g. "png;jpg;jpeg" or "*.png;*.jpg" */
} heliosview_file_filter_t;

/* Open-file dialog. `filters` is an array of `filter_count` filters (NULL or 0 = "All files");
 * `multi` != 0 enables multi-selection. On success (n > 0) out_paths receives a NULL-terminated
 * array of n UTF-8 paths (each string and the array itself are freed with heliosview_free, or in one
 * call with heliosview_free_paths). 0 = cancelled, negative = error. */
HELIOSVIEW_API int heliosview_open_files(heliosview_window_t* window, const char* title,
                                         const heliosview_file_filter_t* filters, size_t filter_count,
                                         int multi, char*** out_paths);

/* Free a path array returned by heliosview_open_files (each string and the array). NULL is ignored. */
HELIOSVIEW_API void heliosview_free_paths(char** paths);

/* Save-file dialog. On success (1) out_path receives the chosen path (heliosview_free). */
HELIOSVIEW_API int heliosview_save_file(heliosview_window_t* window, const char* title,
                                        const heliosview_file_filter_t* filters, size_t filter_count,
                                        const char* default_name,
                                        char** out_path);

/* ================= Message box ================= */

typedef enum heliosview_message_type {
    HELIOSVIEW_MESSAGE_INFO = 1,
    HELIOSVIEW_MESSAGE_WARNING,
    HELIOSVIEW_MESSAGE_ERROR,
    HELIOSVIEW_MESSAGE_QUESTION,
} heliosview_message_type_t;

typedef enum heliosview_message_buttons {
    HELIOSVIEW_MESSAGE_OK = 1,
    HELIOSVIEW_MESSAGE_OK_CANCEL,
    HELIOSVIEW_MESSAGE_YES_NO,
    HELIOSVIEW_MESSAGE_YES_NO_CANCEL,
    HELIOSVIEW_MESSAGE_RETRY_CANCEL,
    HELIOSVIEW_MESSAGE_ABORT_RETRY_IGNORE,
} heliosview_message_buttons_t;

typedef enum heliosview_message_result {
    HELIOSVIEW_MESSAGE_RESULT_NONE = 0,
    HELIOSVIEW_MESSAGE_RESULT_OK,
    HELIOSVIEW_MESSAGE_RESULT_CANCEL,
    HELIOSVIEW_MESSAGE_RESULT_YES,
    HELIOSVIEW_MESSAGE_RESULT_NO,
    HELIOSVIEW_MESSAGE_RESULT_RETRY,
    HELIOSVIEW_MESSAGE_RESULT_ABORT,
    HELIOSVIEW_MESSAGE_RESULT_IGNORE,
} heliosview_message_result_t;

/* Show a modal message box. Returns the button the user pressed
 * (HELIOSVIEW_MESSAGE_RESULT_NONE = failure). Message-loop thread. */
HELIOSVIEW_API int heliosview_message_box(heliosview_window_t* window,
                                          heliosview_message_type_t type,
                                          heliosview_message_buttons_t buttons,
                                          const char* title, const char* message);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_DIALOGS_H */
