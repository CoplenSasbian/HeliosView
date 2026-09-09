// HeliosView - portable stub backend.
//
// Built on platforms that have no real backend yet (see the else() branch in
// src/CMakeLists.txt). Every feature reports HELIOSVIEW_ERROR_UNSUPPORTED (-4)
// and the WebView engine query reports "no engine", so the library still links
// and the public C API is exercisable everywhere - a conformance test suite can
// run on any platform and assert the "unsupported" contract.
//
// GENERATED FILE - do not edit by hand. The definitions below mirror
// include/HeliosView/heliosview.h one-for-one (signatures copied, parameter
// names dropped). Regenerate with:
//
//     pwsh -File scripts/gen_portable_stub.ps1
//
// The linker is the safety net: a new public API function that is missing here
// shows up as an unresolved symbol when this backend is built.
#include "../heliosview_backend.h"

#define HV_STUB_UNSUPPORTED \
    hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "no HeliosView backend for this platform yet (see src/heliosview_backend.h)")

/* ================= Backend entry points ================= */

const char* hv_backend_name()
{
    return "portable";
}

bool hv_backend_window_alive(uintptr_t)
{
    return false; /* the stub never creates native windows */
}
void heliosview_pump_events()
{
    /* no state to release */
}
int heliosview_run(heliosview_loop_callback, void*)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_window_t* heliosview_window_create_ex(int, int, const char*, heliosview_window_style_t, void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_window_t* heliosview_window_create(int, int, const char*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_window_t* heliosview_window_create_ex2(int, int, const char*, heliosview_window_style_t, uint32_t, void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
uint32_t heliosview_window_flags(const heliosview_window_t*)
{
    return 0;
}
float heliosview_window_scale_factor(const heliosview_window_t*)
{
    /* No window backend on this platform: report the documented "not created"
     * value instead of an error code (a getter, not an operation). */
    return 1.0f;
}
void* heliosview_window_userdata(const heliosview_window_t*)
{
    return nullptr;
}
void heliosview_window_set_userdata(heliosview_window_t*, void*)
{
    /* no state to release */
}
void heliosview_window_destroy(heliosview_window_t*)
{
    /* no state to release */
}
int heliosview_window_show(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_hide(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_show_state(heliosview_window_t*, heliosview_show_state_t)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_show_state_t heliosview_window_state(const heliosview_window_t*)
{
    return static_cast<heliosview_show_state_t>(0);
}
int heliosview_window_close(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_focus(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_is_visible(const heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_topmost(heliosview_window_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_position(heliosview_window_t*, int32_t, int32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_position(const heliosview_window_t*, int32_t*, int32_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_size(heliosview_window_t*, int32_t, int32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_size(const heliosview_window_t*, int32_t*, int32_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_title(heliosview_window_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_center(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_opacity(heliosview_window_t*, float)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_icon(heliosview_window_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_icon_ex(heliosview_window_t*, const char*, uint32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_minimize(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_maximize(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_restore(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_toggle_maximize(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_resizable(heliosview_window_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_is_resizable(const heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_add_drag_region(heliosview_window_t*, int32_t, int32_t, int32_t, int32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_clear_drag_regions(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_start_drag(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
uint32_t heliosview_window_dpi(const heliosview_window_t*)
{
    return 0;
}
int32_t heliosview_window_title_bar_height(const heliosview_window_t*)
{
    return 0;
}
int heliosview_window_set_min_size(heliosview_window_t*, int32_t, int32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_max_size(heliosview_window_t*, int32_t, int32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_flash(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_flash_until_focus(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_fullscreen(heliosview_window_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_is_fullscreen(const heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_enabled(heliosview_window_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_is_enabled(const heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_set_session_end_callback(heliosview_session_end_cb, void*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_set_dpi_awareness()
{
    return HV_STUB_UNSUPPORTED;
}
uintptr_t heliosview_window_id(const heliosview_window_t*)
{
    return 0;
}
int heliosview_screen_work_area(int32_t, int32_t, heliosview_rect_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_work_area(const heliosview_window_t*, heliosview_rect_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_primary_work_area(heliosview_rect_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_cursor_position(int32_t*, int32_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_progress(heliosview_window_t*, uint32_t, uint32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_progress_state(heliosview_window_t*, heliosview_progress_state_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_clear_progress(heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_backdrop(heliosview_window_t*, heliosview_backdrop_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_window_set_dark_mode(heliosview_window_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_tray_t* heliosview_tray_create(const char*, const char*, void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
int heliosview_tray_set_tooltip(heliosview_tray_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_tray_set_icon(heliosview_tray_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_tray_set_icon_ex(heliosview_tray_t*, const char*, uint32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_tray_set_menu(heliosview_tray_t*, heliosview_menu_t*)
{
    return HV_STUB_UNSUPPORTED;
}
void heliosview_tray_destroy(heliosview_tray_t*)
{
    /* no state to release */
}
int heliosview_tray_notify(heliosview_tray_t*, const char*, const char*, heliosview_tray_notify_icon_t, uint32_t)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_action_t* heliosview_action_create(const char*, void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_action_t* heliosview_action_create_role(heliosview_menu_role_t, const char*, void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_menu_role_t heliosview_action_role(const heliosview_action_t*)
{
    return static_cast<heliosview_menu_role_t>(0);
}
void heliosview_action_destroy(heliosview_action_t*)
{
    /* no state to release */
}
int heliosview_action_set_text(heliosview_action_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_set_shortcut(heliosview_action_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
const char* heliosview_action_shortcut(const heliosview_action_t*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
int heliosview_action_set_enabled(heliosview_action_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_set_checkable(heliosview_action_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_set_radio_style(heliosview_action_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_set_checked(heliosview_action_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_is_enabled(const heliosview_action_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_is_checked(const heliosview_action_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_is_checkable(const heliosview_action_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_action_is_radio_style(const heliosview_action_t*)
{
    return HV_STUB_UNSUPPORTED;
}
const char* heliosview_action_text(const heliosview_action_t*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
uint32_t heliosview_action_id(const heliosview_action_t*)
{
    return 0;
}
heliosview_menu_t* heliosview_menu_create(void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_menu_t* heliosview_menu_create_bar(void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_menu_t* heliosview_menubar_create(void*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
int heliosview_menu_add_action(heliosview_menu_t*, heliosview_action_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_set_default_action(heliosview_menu_t*, heliosview_action_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_set_kind(heliosview_menu_t*, heliosview_menu_kind_t)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_menu_kind_t heliosview_menu_kind(const heliosview_menu_t*)
{
    return static_cast<heliosview_menu_kind_t>(0);
}
int heliosview_menu_set_open_callback(heliosview_menu_t*, heliosview_menu_open_cb, void*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_set_app_menu(heliosview_menu_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menubar_set_app_menu(heliosview_menu_t*)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_menu_t* heliosview_menu_app_menu()
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
int heliosview_translate_accelerator(void*)
{
    return HV_STUB_UNSUPPORTED;
}
void heliosview_menu_destroy(heliosview_menu_t*)
{
    /* no state to release */
}
int heliosview_menu_add_item(heliosview_menu_t*, const char*, uint32_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_add_item_ex(heliosview_menu_t*, const char*, uint32_t, uint32_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_add_checkable_item(heliosview_menu_t*, const char*, int, uint32_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_set_item_checked(heliosview_menu_t*, uint32_t, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_is_item_checked(heliosview_menu_t*, uint32_t, int*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_set_item_enabled(heliosview_menu_t*, uint32_t, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_is_item_enabled(heliosview_menu_t*, uint32_t, int*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_set_item_default(heliosview_menu_t*, uint32_t, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_is_item_default(heliosview_menu_t*, uint32_t, int*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_add_separator(heliosview_menu_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_add_submenu(heliosview_menu_t*, const char*, heliosview_menu_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menubar_add_menu(heliosview_menu_t*, const char*, heliosview_menu_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_menu_show(heliosview_menu_t*, heliosview_window_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_engine_version(char* buf, size_t size)
{
    /* No web engine on this platform: report "" (the documented "unavailable"
     * value) instead of an error, so callers branch on emptiness. */
    if (!buf || size == 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "buf is NULL or size is 0");
    buf[0] = '\0';
    return 0;
}
heliosview_webview_t* heliosview_webview_create_ex(heliosview_window_t*, const heliosview_webview_env_opts_t*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
heliosview_webview_t* heliosview_webview_create(heliosview_window_t*)
{
    HV_STUB_UNSUPPORTED;
    return nullptr;
}
void heliosview_webview_destroy(heliosview_webview_t*)
{
    /* no state to release */
}
int heliosview_webview_navigate(heliosview_webview_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_navigate_html(heliosview_webview_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_suspend(heliosview_webview_t*, heliosview_webview_suspend_cb, void*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_resume(heliosview_webview_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_is_suspended(heliosview_webview_t*, int*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_background_color(heliosview_webview_t*, uint8_t, uint8_t, uint8_t, uint8_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_transparent_background(heliosview_webview_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_last_native_error(heliosview_webview_t*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_bind(heliosview_webview_t*, const char*, heliosview_webview_bind_cb, void*, heliosview_webview_userdata_dtor)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_resolve(heliosview_webview_t*, uint64_t, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_reject(heliosview_webview_t*, uint64_t, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_eval(heliosview_webview_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_eval_async(heliosview_webview_t*, const char*, heliosview_webview_eval_cb, void*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_broadcast(heliosview_webview_t*, const char*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_subscribe(heliosview_webview_t*, const char*, heliosview_webview_subscribe_cb, void*, heliosview_webview_userdata_dtor)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_unsubscribe(heliosview_webview_t*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_navigation_callback(heliosview_webview_t*, heliosview_webview_navigation_cb, void*, heliosview_webview_userdata_dtor)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_navigation_starting_callback(heliosview_webview_t*, heliosview_webview_navigation_starting_cb, void*, heliosview_webview_userdata_dtor)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_source_changed_callback(heliosview_webview_t*, heliosview_webview_source_changed_cb, void*, heliosview_webview_userdata_dtor)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_title_changed_callback(heliosview_webview_t*, heliosview_webview_title_changed_cb, void*, heliosview_webview_userdata_dtor)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_map_local_folder(heliosview_webview_t*, const char*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_local_url(heliosview_webview_t*, const char*, const char*, char*, size_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_insets(heliosview_webview_t*, int32_t, int32_t, int32_t, int32_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_status_bar(heliosview_webview_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_context_menu(heliosview_webview_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_devtools(heliosview_webview_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_window_controls_overlay(heliosview_webview_t*, int)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_webview_set_window_controls_background_color(heliosview_webview_t*, uint8_t, uint8_t, uint8_t, uint8_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_select_folder(heliosview_window_t*, const char*, char** out_path)
{
    if (out_path)
        *out_path = nullptr;
    return HV_STUB_UNSUPPORTED;
}
int heliosview_open_files(heliosview_window_t*, const char*, const heliosview_file_filter_t*,
                          size_t, int, char*** out_paths)
{
    if (out_paths)
        *out_paths = nullptr;
    return HV_STUB_UNSUPPORTED;
}
void heliosview_free_paths(char**)
{
    /* no state to release */
}
int heliosview_save_file(heliosview_window_t*, const char*, const heliosview_file_filter_t*,
                         size_t, const char*, char** out_path)
{
    if (out_path)
        *out_path = nullptr;
    return HV_STUB_UNSUPPORTED;
}
int heliosview_message_box(heliosview_window_t*, heliosview_message_type_t, heliosview_message_buttons_t, const char*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_open_url(const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_open_path(const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_show_in_folder(const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_run_program(const char*, const char*, heliosview_program_show_t)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_run_program_wait(const char*, const char*, heliosview_program_show_t, int* out_exit_code)
{
    if (out_exit_code)
        *out_exit_code = 0;
    return HV_STUB_UNSUPPORTED;
}
int heliosview_run_program_elevated(const char*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_clipboard_set_text(const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_clipboard_get_text(char** out)
{
    if (out)
        *out = nullptr;
    return HV_STUB_UNSUPPORTED;
}
int heliosview_system_path(heliosview_system_path_kind_t, char** out)
{
    if (out)
        *out = nullptr;
    return HV_STUB_UNSUPPORTED;
}
int heliosview_os_version(char* buf, size_t size)
{
    if (!buf || size == 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "buf is NULL or size is 0");
    buf[0] = '\0';
    return 0;
}
int heliosview_system_power(heliosview_power_action_t)
{
    return HV_STUB_UNSUPPORTED;
}
uint32_t heliosview_hotkey_register(const char*, heliosview_hotkey_cb, void*)
{
    return 0;
}
void heliosview_hotkey_unregister(uint32_t)
{
    /* no state to release */
}
int heliosview_notification_request_permission(heliosview_notification_permission_cb, void*)
{
    return HV_STUB_UNSUPPORTED;
}
heliosview_notification_permission_t heliosview_notification_permission_state()
{
    return static_cast<heliosview_notification_permission_t>(0);
}
int heliosview_notification_init(const char*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_notification_set_click_callback(heliosview_notification_click_cb, void*)
{
    return HV_STUB_UNSUPPORTED;
}
int heliosview_notification_show(const char*, const char*)
{
    return HV_STUB_UNSUPPORTED;
}
