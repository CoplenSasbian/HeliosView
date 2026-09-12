/*
 * HeliosView C API example (pure C99).
 *
 * The C API is the library's only exported interface and is complete enough to
 * build a whole app without writing any C++: opaque handles, POD events and
 * error codes instead of exceptions. The C++ wrapper (HeliosView.Core) is a thin
 * layer over exactly these calls, so this file doubles as a tour of it.
 *
 *   - heliosview_app_init       process identity (AppUserModelID / bundle id)
 *   - heliosview_window_*       create / show / move / title / progress / close
 *   - heliosview_run + frame()  the message loop: pump native messages, then
 *                               drain the event queue with heliosview_poll
 *   - heliosview_tray_*         notification-area icon + attached context menu
 *   - heliosview_menu_*         standalone popup menu with item ids
 *   - heliosview_message_box    modal dialog
 *
 * Note the event loop contract: heliosview_run pumps and queues, the frame
 * callback decides what to do with each event. Closing/destroying the last
 * window ends the loop (heliosview_window_destroy).
 */
#include <HeliosView/heliosview.h>

#include <stdio.h>

static heliosview_window_t* g_window;   /* NULL once destroyed */
static uint32_t g_progress_item;        /* menu item ids from heliosview_menu_add_item */
static uint32_t g_about_item;
static uint32_t g_quit_item;
static int g_progress;

/* Taskbar progress: cycles 0 -> 100% and then clears itself. */
static void bump_progress(void)
{
    if (!g_window)
        return;
    g_progress = (g_progress + 25) % 125;
    if (g_progress >= 100)
        heliosview_window_clear_progress(g_window);
    else
        heliosview_window_set_progress(g_window, (uint32_t)g_progress, 100);
    printf("[c] taskbar progress -> %d%%\n", g_progress);
}

static void close_window(void)
{
    heliosview_window_destroy(g_window); /* destroying the last window ends the loop */
    g_window = NULL;                     /* ... so never destroy it twice */
}

static void on_frame_event(const heliosview_event_t* ev)
{
    switch (ev->type) {
    case HELIOSVIEW_EVENT_KEY_DOWN:
        printf("[c] key %d\n", (int)ev->key);
        if (ev->key == HELIOSVIEW_KEY_ESCAPE)
            close_window();
        else if (ev->key == HELIOSVIEW_KEY_F1)
            bump_progress();
        break;
    case HELIOSVIEW_EVENT_WINDOW_RESIZE:
        printf("[c] resize %d x %d\n", ev->width, ev->height);
        break;
    case HELIOSVIEW_EVENT_TRAY_LEFT_CLICK:
        printf("[c] tray left click\n");
        break;
    case HELIOSVIEW_EVENT_MENU_SELECT:
        printf("[c] menu item %u\n", (unsigned)ev->menu_item);
        if (ev->menu_item == g_quit_item)
            heliosview_quit();
        else if (ev->menu_item == g_progress_item)
            bump_progress();
        else if (ev->menu_item == g_about_item)
            heliosview_message_box(g_window, HELIOSVIEW_MESSAGE_INFO, HELIOSVIEW_MESSAGE_OK,
                                   "About", "HeliosView - a C++ WebView windowing library "
                                            "with a stable pure-C ABI");
        break;
    case HELIOSVIEW_EVENT_WINDOW_CLOSE:
        printf("[c] close requested -> destroying the window\n");
        close_window();
        break;
    default:
        break;
    }
}

/* The frame callback: drain everything the loop queued, then return (0 = keep running). */
static int frame(void* userdata)
{
    (void)userdata;
    heliosview_event_t ev;
    while (heliosview_poll(&ev))
        on_frame_event(&ev);
    return 0;
}

int main(void)
{
    printf("HeliosView %s (%s backend) - pure C demo\n", heliosview_version(),
           heliosview_backend_name());

    /* Process identity (optional): used for toasts, taskbar grouping and desktop
     * integration. Set it before creating windows / showing notifications. */
    heliosview_app_init("com.example.heliosview.cdemo");

    g_window = heliosview_window_create(800, 600, "HeliosView C demo");
    if (!g_window) {
        char reason[256] = {0};
        heliosview_last_error_string(reason, sizeof reason);
        fprintf(stderr, "window creation failed: %s\n", reason);
        return 1;
    }
    heliosview_window_show(g_window);

    /* Tray icon (no window required). The balloon works with no extra setup. */
    heliosview_tray_t* tray = heliosview_tray_create("HeliosView C demo", NULL, NULL);
    if (tray)
        heliosview_tray_notify(tray, "HeliosView", "Hello from pure C",
                               HELIOSVIEW_TRAY_NOTIFY_INFO, 3000);

    /* A standalone popup menu. heliosview_menu_add_item is the convenience path
     * (the menu owns the action); it writes the item id that comes back in
     * HELIOSVIEW_EVENT_MENU_SELECT. */
    heliosview_menu_t* menu = heliosview_menu_create(NULL);
    heliosview_menu_add_item(menu, "Progress +25%", &g_progress_item);
    heliosview_menu_add_item(menu, "About", &g_about_item);
    heliosview_menu_add_separator(menu);
    heliosview_menu_add_item(menu, "Quit", &g_quit_item);
    /* Attaching the menu to the tray is the portable context-menu route (on Linux
     * the menu is exported over DBus, on macOS it becomes the status-item menu). */
    if (tray)
        heliosview_tray_set_menu(tray, menu);

    /* Modal dialog (blocking, before the loop starts). */
    heliosview_message_box(g_window, HELIOSVIEW_MESSAGE_INFO, HELIOSVIEW_MESSAGE_OK, "Info",
                           "HeliosView from pure C");

    printf("[c] controls: Esc / window X closes | F1 taskbar progress | "
           "tray right-click = menu | tray left click\n");
    heliosview_run(frame, NULL);

    /* Teardown, mirroring creation order (the tray references the menu, so it goes
     * first). g_window may already be NULL if the user closed it. */
    heliosview_tray_destroy(tray);
    heliosview_menu_destroy(menu);
    heliosview_window_destroy(g_window);
    return 0;
}
