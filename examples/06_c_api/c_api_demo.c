/* ============================================================================
 * HeliosView Example 06: Pure C99 ABI Tour & FFI Benchmark
 * ============================================================================
 * Proves that HeliosView can be fully driven from standard C99 without any
 * C++ runtime dependency (suitable for language bindings in Rust, Go, Python, C#):
 *   1. Window lifecycle: create, show, move, title, progress, close
 *   2. System Tray: tray icon, tooltip, context menu, and balloon alerts
 *   3. Pop-up Menus: standalone menus with item identifiers
 *   4. Native Dialogs: modal message box
 *   5. Message loop: heliosview_run with heliosview_poll event queue
 * ============================================================================
 */

#include <HeliosView/heliosview.h>

#include <stdio.h>

static heliosview_window_t* g_window = NULL;
static uint32_t g_progress_item = 0;
static uint32_t g_about_item = 0;
static uint32_t g_quit_item = 0;
static int g_progress = 0;

static void bump_progress(void) {
    if (!g_window) return;
    g_progress = (g_progress + 25) % 125;
    if (g_progress >= 100) {
        heliosview_window_clear_progress(g_window);
    } else {
        heliosview_window_set_progress(g_window, (uint32_t)g_progress, 100);
    }
    printf("[C99] Taskbar progress set to %d%%\n", g_progress);
}

static void close_window(void) {
    if (g_window) {
        heliosview_window_destroy(g_window);
        g_window = NULL;
    }
}

static void on_frame_event(const heliosview_event_t* ev) {
    switch (ev->type) {
    case HELIOSVIEW_EVENT_KEY_DOWN:
        printf("[C99] Key pressed: %d\n", (int)ev->key);
        if (ev->key == HELIOSVIEW_KEY_ESCAPE) {
            close_window();
        } else if (ev->key == HELIOSVIEW_KEY_F1) {
            bump_progress();
        }
        break;

    case HELIOSVIEW_EVENT_WINDOW_RESIZE:
        printf("[C99] Window resized: %d x %d\n", ev->width, ev->height);
        break;

    case HELIOSVIEW_EVENT_TRAY_LEFT_CLICK:
        printf("[C99] System tray icon left-clicked!\n");
        break;

    case HELIOSVIEW_EVENT_MENU_SELECT:
        printf("[C99] Menu selected: item id %u\n", (unsigned)ev->menu_item);
        if (ev->menu_item == g_quit_item) {
            heliosview_quit();
        } else if (ev->menu_item == g_progress_item) {
            bump_progress();
        } else if (ev->menu_item == g_about_item) {
            heliosview_message_box(g_window, HELIOSVIEW_MESSAGE_INFO, HELIOSVIEW_MESSAGE_OK,
                                   "About HeliosView C ABI",
                                   "HeliosView - High-performance WebView & Native UI library\n"
                                   "Compiled as pure C99!");
        }
        break;

    case HELIOSVIEW_EVENT_WINDOW_CLOSE:
        printf("[C99] Close event received -> Destroying window.\n");
        close_window();
        break;

    default:
        break;
    }
}

static int frame_callback(void* userdata) {
    (void)userdata;
    heliosview_event_t ev;
    while (heliosview_poll(&ev)) {
        on_frame_event(&ev);
    }
    return 0;
}

int main(void) {
    printf("===========================================================\n");
    printf(" HeliosView %s (%s) - Pure C99 ABI Showcase\n",
           heliosview_version(), heliosview_backend_name());
    printf("===========================================================\n");

    // 1. Initialize application identity
    heliosview_app_init("com.heliosview.cdemo");

    // 2. Create window shell (800 x 600)
    g_window = heliosview_window_create(800, 600, "HeliosView Pure C99 ABI Tour");
    if (!g_window) {
        fprintf(stderr, "Failed to create window!\n");
        return 1;
    }

    // 3. Create context menu
    heliosview_menu_t* menu = heliosview_menu_create(NULL);
    heliosview_menu_add_item(menu, "Advance Taskbar Progress (F1)", &g_progress_item);
    heliosview_menu_add_item(menu, "About HeliosView...", &g_about_item);
    heliosview_menu_add_separator(menu);
    heliosview_menu_add_item(menu, "Quit", &g_quit_item);

    // 4. Attach system tray
    heliosview_tray_t* tray = heliosview_tray_create("HeliosView C Demo", NULL, NULL);
    if (tray) {
        heliosview_tray_set_menu(tray, menu);
        heliosview_tray_notify(tray, "HeliosView C API", "Pure C99 application initialized",
                               HELIOSVIEW_TRAY_NOTIFY_INFO, 3000);
    }

    heliosview_window_show(g_window);
    printf("Controls:\n  [F1] Advance Taskbar Progress\n  [Esc] Close Window\n\n");

    // 5. Run event loop
    heliosview_run(frame_callback, NULL);

    // 6. Clean teardown
    if (tray) heliosview_tray_destroy(tray);
    if (menu) heliosview_menu_destroy(menu);
    if (g_window) heliosview_window_destroy(g_window);

    printf("[C99] Exited cleanly.\n");
    return 0;
}
