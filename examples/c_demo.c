/*
 * HeliosView C API example (pure C99): proves the C header compiles as C and
 * walks through windows, events, tray, menus, message boxes and the loop.
 */
#include <HeliosView/heliosview.h>

#include <stdio.h>

static heliosview_window_t* g_win;
static heliosview_menu_t* g_menu;

static int frame(void* userdata)
{
    (void)userdata;
    heliosview_event_t ev;
    while (heliosview_poll(&ev)) {
        switch (ev.type) {
        case HELIOSVIEW_EVENT_KEY_DOWN:
            printf("[c] key %d\n", (int)ev.key);
            if (ev.key == HELIOSVIEW_KEY_ESCAPE)
                heliosview_window_close(g_win);
            break;
        case HELIOSVIEW_EVENT_WINDOW_RESIZE:
            printf("[c] resize %d x %d\n", ev.width, ev.height);
            break;
        case HELIOSVIEW_EVENT_TRAY_LEFT_CLICK:
            printf("[c] tray left click\n");
            break;
        case HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK:
            printf("[c] tray right click -> menu\n");
            heliosview_menu_show(g_menu, g_win);
            break;
        case HELIOSVIEW_EVENT_WINDOW_CLOSE:
            printf("[c] close requested -> closing\n");
            heliosview_window_close(g_win);
            break;
        case HELIOSVIEW_EVENT_MENU_SELECT:
            printf("[c] menu item %u\n", (unsigned)ev.menu_item);
            heliosview_quit();
            break;
        default:
            break;
        }
    }
    return 0;
}

int main(void)
{
    printf("HeliosView %s\n", heliosview_version());

    g_win = heliosview_window_create(800, 600, "C demo");
    heliosview_window_show(g_win);

    /* tray icon + balloon (works with no setup) */
    heliosview_tray_t* tray = heliosview_tray_create(g_win, "C tray", NULL, NULL);
    if (tray)
        heliosview_tray_notify(tray, "Tray", "Hello from C", HELIOSVIEW_TRAY_NOTIFY_INFO, 3000);

    /* popup menu */
    g_menu = heliosview_menu_create(g_win, NULL);
    heliosview_menu_add_item(g_menu, "Quit", NULL);

    /* modal message box */
    heliosview_message_box(g_win, HELIOSVIEW_MESSAGE_INFO, HELIOSVIEW_MESSAGE_OK,
                           "Info", "HeliosView from pure C");

    printf("[c] entering loop (tray clicks / right-click for menu / Esc closes)\n");
    heliosview_run(frame, NULL);

    heliosview_menu_destroy(g_menu);
    heliosview_tray_destroy(tray);
    heliosview_window_destroy(g_win);
    return 0;
}
