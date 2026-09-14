#ifndef HELIOSVIEW_HELIOSVIEW_MENU_H
#define HELIOSVIEW_HELIOSVIEW_MENU_H

/**
 * HeliosView C API -- actions, menus and the application menu bar
 *
 * Menu commands: the shareable Action (label, shortcut, enabled, checked), popup /
 * context menus, their item helpers, and the application menu bar. A selection is
 * reported through HELIOSVIEW_EVENT_MENU_SELECT (heliosview_event.h).
 *
 * Part of the public C ABI; included by <HeliosView/heliosview.h>, which is the
 * umbrella header. This header can also be included on its own -- the parts it
 * depends on are listed below and are include-guard safe.
 */

#include <HeliosView/heliosview_base.h>
#include <HeliosView/heliosview_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Action (shareable menu command) =================
 *
 * An action is a command with identity and state: its label, whether it is
 * enabled, whether it is checkable/checked, its shortcut and standard role. It
 * carries no window and no menu — a menu only *displays* actions.
 *
 * The same action may be added to any number of menus (an Edit menu, a context
 * menu, ...): one triggered event, one enabled/checked state, one label.
 *
 * Portability contract (every backend must satisfy it):
 *   - Menus re-read action state when they pop up, so changing an action
 *     updates every menu that shows it. Native item state is a cache, never the
 *     source of truth.
 *   - Shortcut scope differs by platform: on macOS a shortcut is a menu key
 *     equivalent, so it only takes effect for actions reachable from the
 *     application menu bar; on Windows every live action with a shortcut is an
 *     application-wide accelerator. Portable applications should put
 *     shortcut-bearing actions in the application menu bar (see
 *     heliosview_menu_set_app_menu) so both platforms behave identically.
 *   - heliosview_menu_set_default_action (bold item, Enter activates) is a
 *     Windows convention; other platforms may ignore it.
 *
 * Lifetime: reference-counted by the menus that display it. heliosview_action_destroy
 * releases the caller's reference; the object itself lives until the last menu
 * using it is destroyed, so a menu can never reference a freed action.
 *
 * Message-loop thread (like menus and windows).
 */

/* heliosview_action_t is declared in heliosview_base.h (the tray and menu headers
 * both name it). */

/* ---------- Standard roles ----------
 *
 * A role marks an action as a standard command. The library then supplies the
 * platform's conventional label and shortcut, and — where the application cannot
 * do the work itself — performs the action:
 *
 *   role                    macOS (system-provided)          Windows (library-provided)
 *   ABOUT                   About <App>                     event only
 *   PREFERENCES             "Settings…" ⌘,                 event only
 *   QUIT                    "Quit <App>" ⌘Q                event only (C++ App quits when
 *                                                          no handler is connected)
 *   HIDE / HIDE_OTHERS      ⌘H / ⌥⌘H                       event only
 *   SHOW_ALL                "Show All"                     event only
 *   SERVICES                system Services menu           event only
 *   UNDO / REDO             ⌘Z / ⇧⌘Z → responder chain     Ctrl+Z / Ctrl+Y → focused control
 *   CUT/COPY/PASTE          ⌘X/⌘C/⌘V → responder chain     Ctrl+X/C/V → focused control
 *   SELECT_ALL              ⌘A → responder chain           Ctrl+A → focused control
 *   DELETE                  forward delete                 Del → focused control
 *   MINIMIZE                ⌘M → performMiniaturize:       minimizes the active window
 *   ZOOM                    performZoom:                   maximizes/restores it
 *   CLOSE_WINDOW            ⌘W → performClose:             closes the active window
 *   TOGGLE_FULLSCREEN       ⌃⌘F → toggleFullScreen:        F11 → toggles it
 *   BRING_ALL_TO_FRONT      "Bring All to Front"           event only
 *
 * "Event only" roles still post MENU_SELECT, so the application can implement
 * them (and observe/confirm others); the library only performs the actions the
 * application cannot perform itself. A role's default label/shortcut can always
 * be overridden with heliosview_action_set_text / _set_shortcut.
 */
typedef enum heliosview_menu_role {
    HELIOSVIEW_MENU_ROLE_NONE = 0,   /* plain action */
    /* application */
    HELIOSVIEW_MENU_ROLE_ABOUT,
    HELIOSVIEW_MENU_ROLE_PREFERENCES,
    HELIOSVIEW_MENU_ROLE_QUIT,
    HELIOSVIEW_MENU_ROLE_HIDE,
    HELIOSVIEW_MENU_ROLE_HIDE_OTHERS,
    HELIOSVIEW_MENU_ROLE_SHOW_ALL,
    HELIOSVIEW_MENU_ROLE_SERVICES,
    /* edit */
    HELIOSVIEW_MENU_ROLE_UNDO,
    HELIOSVIEW_MENU_ROLE_REDO,
    HELIOSVIEW_MENU_ROLE_CUT,
    HELIOSVIEW_MENU_ROLE_COPY,
    HELIOSVIEW_MENU_ROLE_PASTE,
    HELIOSVIEW_MENU_ROLE_SELECT_ALL,
    HELIOSVIEW_MENU_ROLE_DELETE,
    /* window */
    HELIOSVIEW_MENU_ROLE_MINIMIZE,
    HELIOSVIEW_MENU_ROLE_ZOOM,
    HELIOSVIEW_MENU_ROLE_CLOSE_WINDOW,
    HELIOSVIEW_MENU_ROLE_TOGGLE_FULLSCREEN,
    HELIOSVIEW_MENU_ROLE_BRING_ALL_TO_FRONT,
} heliosview_menu_role_t;

/* Create an action with the given label (UTF-8; NULL = empty). `userdata` is
 * caller data (the C++ wrapper stores the Action object pointer) copied into
 * the MENU_SELECT events this action produces. Returns NULL on failure. */
HELIOSVIEW_API heliosview_action_t* heliosview_action_create(const char* text, void* userdata);

/* Create a standard role action (see the table above). `text` overrides the
 * platform's default label (NULL = default). Returns NULL on failure. */
HELIOSVIEW_API heliosview_action_t* heliosview_action_create_role(heliosview_menu_role_t role,
                                                                  const char* text,
                                                                  void* userdata);

/* The action's role (HELIOSVIEW_MENU_ROLE_NONE for a plain action). */
HELIOSVIEW_API heliosview_menu_role_t heliosview_action_role(const heliosview_action_t* action);

/* Release the caller's reference. The action is freed once no menu references it. */
HELIOSVIEW_API void heliosview_action_destroy(heliosview_action_t* action);

/* Update the label (UTF-8). Menus showing this action display it on their next popup. */
HELIOSVIEW_API int heliosview_action_set_text(heliosview_action_t* action, const char* text);

/* Set the keyboard shortcut, written portably as modifier(s) joined by '+' and a
 * key: "Primary+S", "Ctrl+Shift+Z", "Alt+F4", "F11", "Cmd+O".
 *   Primary  = Command on macOS, Control elsewhere (write this in portable code)
 *   Cmd      = Command on macOS, Control elsewhere (alias of Primary)
 *   Ctrl, Alt/Option, Shift, Meta/Win  = that key on every platform
 *   key      = a single character (A-Z, 0-9, punctuation) or a name: F1..F24,
 *              Escape, Return/Enter, Space, Tab, Backspace, Delete, Insert, Home,
 *              End, PageUp, PageDown, Left, Right, Up, Down, Comma, Period,
 *              Slash, Semicolon, Apostrophe, Grave, Minus, Equal, Backslash
 * The string is stored as given (menus display the platform's own form) and is
 * rejected (-1 + heliosview_last_error) when it cannot be parsed. NULL clears it. */
HELIOSVIEW_API int heliosview_action_set_shortcut(heliosview_action_t* action, const char* shortcut);

/* The action's shortcut string (NULL when unset; library-owned, valid until the
 * shortcut is changed or the action is destroyed). */
HELIOSVIEW_API const char* heliosview_action_shortcut(const heliosview_action_t* action);

/* Enable (enabled != 0) or disable the action: disabled items are grayed out and
 * not selectable in every menu that shows it. 0 = success. */
HELIOSVIEW_API int heliosview_action_set_enabled(heliosview_action_t* action, int enabled);

/* Make the action checkable (checkable != 0): menus then draw a checkmark while
 * it is checked. heliosview_action_set_radio_style switches the mark to a radio
 * bullet. Un-checkable actions ignore heliosview_action_set_checked. */
HELIOSVIEW_API int heliosview_action_set_checkable(heliosview_action_t* action, int checkable);
HELIOSVIEW_API int heliosview_action_set_radio_style(heliosview_action_t* action, int radio);

/* Set the checked state (only meaningful for a checkable action). */
HELIOSVIEW_API int heliosview_action_set_checked(heliosview_action_t* action, int checked);

/* Read state: 1 = enabled / checked / checkable / radio style, 0 = not (or NULL action). */
HELIOSVIEW_API int heliosview_action_is_enabled(const heliosview_action_t* action);
HELIOSVIEW_API int heliosview_action_is_checked(const heliosview_action_t* action);
HELIOSVIEW_API int heliosview_action_is_checkable(const heliosview_action_t* action);
HELIOSVIEW_API int heliosview_action_is_radio_style(const heliosview_action_t* action);

/* The action's label (UTF-8, library-owned; valid until the action is changed or
 * destroyed; never NULL — empty string when unset). */
HELIOSVIEW_API const char* heliosview_action_text(const heliosview_action_t* action);

/* The action's process-unique id: it is what MENU_SELECT carries in
 * heliosview_event_t::menu_item, and what an accelerator will trigger. 0 = none. */
HELIOSVIEW_API uint32_t heliosview_action_id(const heliosview_action_t* action);

/* Look up a live action by its id (0 / unknown id = NULL). Message-loop thread. */
HELIOSVIEW_API heliosview_action_t* heliosview_action_from_id(uint32_t id);

/* ================= Menu (popup / context / menu bar) =================
 *
 * A menu is a standalone object, like a tray icon: it is created and filled
 * without any window, and only showing it needs an owner (the OS delivers the
 * selection to the owner's message queue).
 *
 * Two kinds exist, and the difference is real on every platform (not a Win32
 * quirk):
 *   - a POPUP menu (heliosview_menu_create): shown with heliosview_menu_show,
 *     attached to a tray (heliosview_tray_set_menu), or added as a submenu
 *     (heliosview_menu_add_submenu). Win32: CreatePopupMenu; GTK: GtkMenu;
 *     macOS: any NSMenu that is not installed as the main menu.
 *   - a MENU BAR (heliosview_menu_create_bar): the application menu bar,
 *     installed with heliosview_menu_set_app_menu. Win32: CreateMenu; GTK:
 *     GtkMenuBar; macOS: the NSMenu installed as NSApp.mainMenu.
 *
 * Kind invariants, enforced by every backend:
 *   - only a menu bar can serve as the application menu bar / a window's menu
 *     (Win32 SetMenu rejects popup handles with ERROR_INVALID_PARAMETER);
 *   - only a popup can be shown directly, attached to a tray, or added as a
 *     submenu; a menu bar is a container of top-level items (usually submenus)
 *     and is never shown with heliosview_menu_show.
 *
 * heliosview_menu_show pops a menu up at the current cursor position; `window`
 * may be NULL, in which case the library uses its own hidden owner window — so
 * a menu can be shown from a tray icon even when the application has no window
 * at all (MENU_SELECT then carries window_id = 0).
 *
 * Each item is assigned a unique id; choosing an item posts a
 * HELIOSVIEW_EVENT_MENU_SELECT event (menu_item = the item id, userdata = the
 * menu's userdata) which flows through the normal event queue. Its window_id is
 * the window the menu belongs to; for an item of the application menu bar it is
 * the window that was active when the item was chosen (0 = none). Items are
 * added with heliosview_menu_add_item; the caller receives the item's id via
 * out_id (used to match the event). Item state flags (checkmark, disabled,
 * radio-style checkmark, default) are set at creation with
 * heliosview_menu_add_item_ex and toggled afterwards with the
 * heliosview_menu_set_*_item_* helpers (checked / enabled / default). Submenus
 * are added by handle: the parent keeps its own reference to them, so destroying
 * a submenu that is still attached only drops the caller's reference (the parent
 * keeps displaying it until the parent is released too).
 */

/* Item flags for heliosview_menu_add_item_ex (OR-able). */
enum {
    HELIOSVIEW_MENU_ITEM_CHECKED    = 1 << 0, /* show a checkmark next to the text */
    HELIOSVIEW_MENU_ITEM_DISABLED   = 1 << 1, /* grayed out and not selectable */
    HELIOSVIEW_MENU_ITEM_RADIOCHECK = 1 << 2, /* bullet (radio) instead of a checkmark */
    HELIOSVIEW_MENU_ITEM_DEFAULT    = 1 << 3  /* default item: bold, Enter / double-click activates */
};

/* Create an empty standalone POPUP menu. `userdata` is caller data (e.g. a C++
 * Menu object) copied verbatim into the MENU_SELECT events this menu produces;
 * it is how a selection is routed back to the menu, so no window is needed here.
 * A popup menu is shown with heliosview_menu_show, attached to a tray
 * (heliosview_tray_set_menu), or added as a submenu
 * (heliosview_menu_add_submenu). For the application menu bar use
 * heliosview_menu_create_bar instead. Returns NULL on failure. */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_create(void* userdata);

/* Create an empty window MENU BAR for the application menu bar
 * (heliosview_menu_set_app_menu). Unlike a popup menu, a bar holds top-level
 * items — usually submenus — and is never shown with heliosview_menu_show; the
 * two kinds are not interchangeable anywhere (Win32: CreateMenu vs
 * CreatePopupMenu, SetMenu rejects popup handles with ERROR_INVALID_PARAMETER;
 * GTK: GtkMenuBar vs GtkMenu; macOS: the NSMenu installed as NSApp.mainMenu).
 * Returns NULL on failure. */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_create_bar(void* userdata);

/* Alias for heliosview_menu_create_bar: create a horizontal menu bar. */
HELIOSVIEW_API heliosview_menu_t* heliosview_menubar_create(void* userdata);

/* Add an action to the menu (in order). The same action may be added to several
 * menus; the menu holds a reference (see heliosview_action_destroy) and borrows
 * nothing else. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_action(heliosview_menu_t* menu, heliosview_action_t* action);

/* Make `action` the menu's default item (bold; Enter / double-click activates
 * it; one per menu). NULL clears the current default. The action must already
 * be in this menu. Windows convention — other platforms may ignore it.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_default_action(heliosview_menu_t* menu,
                                                      heliosview_action_t* action);

/* ================= Menu kind & lazy population ================= */

/* What a submenu *is*, beyond its title. macOS needs this to wire the standard
 * menus (NSApp.servicesMenu / windowsMenu / helpMenu, and the App menu), where
 * the title is chosen by the system; other platforms treat it as a hint. */
typedef enum heliosview_menu_kind {
    HELIOSVIEW_MENU_KIND_NORMAL = 0,
    HELIOSVIEW_MENU_KIND_APP,       /* the application menu (macOS: first submenu of the bar) */
    HELIOSVIEW_MENU_KIND_SERVICES,  /* the Services menu (macOS) */
    HELIOSVIEW_MENU_KIND_WINDOW,    /* the Window menu (macOS) */
    HELIOSVIEW_MENU_KIND_HELP,      /* the Help menu (macOS) */
} heliosview_menu_kind_t;

/* Mark a menu with a standard kind (see above). 0 = success, negative = error. */
HELIOSVIEW_API int heliosview_menu_set_kind(heliosview_menu_t* menu, heliosview_menu_kind_t kind);

/* The menu's kind (HELIOSVIEW_MENU_KIND_NORMAL when unset). */
HELIOSVIEW_API heliosview_menu_kind_t heliosview_menu_kind(const heliosview_menu_t* menu);

/* Called just before a menu is shown — including every time a submenu opens —
 * so a menu can populate itself lazily (recent files, state-dependent items).
 * Runs on the message-loop thread; the callback may add actions/submenus to
 * `menu` (item state is refreshed after it returns). NULL removes it. */
typedef void (*heliosview_menu_open_cb)(heliosview_menu_t* menu, void* userdata);

/* Register/remove the open callback. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_open_callback(heliosview_menu_t* menu,
                                                     heliosview_menu_open_cb callback,
                                                     void* userdata);

/* ================= Application menu bar =================
 *
 * Install `menu` as the application's menu bar (NULL removes it).
 *
 *   macOS        the one global menu bar (NSApp.mainMenu); the first submenu
 *                becomes the App menu.
 *   Windows      the menu bar of every HeliosView top-level window, including
 *                windows created later — the platform's native shape (Windows
 *                has no global menu bar).
 *   Linux/GTK    each window's GtkMenuBar.
 *
 * `menu` must be a MENU BAR (heliosview_menu_create_bar): the Win32 API only
 * attaches CreateMenu handles to windows and rejects popup menus
 * (CreatePopupMenu) with ERROR_INVALID_PARAMETER. The bar must outlive the
 * windows showing it; destroying it detaches it from every window first.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_app_menu(heliosview_menu_t* menu);

/* Alias for heliosview_menu_set_app_menu. */
HELIOSVIEW_API int heliosview_menubar_set_app_menu(heliosview_menu_t* menu);

/* The current application menu bar (NULL when unset). */
HELIOSVIEW_API heliosview_menu_t* heliosview_menu_app_menu(void);

/* Translate a native keyboard message against the library's action shortcuts
 * (menu accelerators). Call it in your own message loop before
 * TranslateMessage/DispatchMessage — heliosview_pump_events/heliosview_run
 * already do. Returns 1 when the message was consumed as an accelerator (do not
 * dispatch it further), 0 when it is not ours.
 *
 * native_msg is platform-specific: a MSG* on Windows (an NSEvent* or GdkEvent*
 * on the other platforms once they exist). Only Windows needs this: macOS routes
 * key equivalents through the menu bar itself and the Linux toolkits do the same
 * for GTK/Qt menus, so there it is a no-op that always returns 0 — an application
 * with its own event loop can call it unconditionally. */
HELIOSVIEW_API int heliosview_translate_accelerator(void* native_msg);

/* Destroy the menu and all its submenus. */
HELIOSVIEW_API void heliosview_menu_destroy(heliosview_menu_t* menu);

/* Add a text item. Convenience over the action API: the menu creates and owns an
 * action with this text; its id is written to out_id (NULL = ignore) and its
 * state is reachable through the heliosview_menu_*_item_* helpers below.
 * Use heliosview_menu_add_action instead when the item must be shared or its
 * state managed explicitly. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_item(heliosview_menu_t* menu, const char* text,
                                            uint32_t* out_id);

/* Add a text item with initial state flags (HELIOSVIEW_MENU_ITEM_*); its
 * unique id is written to out_id (NULL = ignore). The flags are applied once
 * at creation; change them later with heliosview_menu_set_item_checked /
 * _enabled / _default. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_item_ex(heliosview_menu_t* menu, const char* text,
                                               uint32_t flags, uint32_t* out_id);

/* Convenience wrapper: heliosview_menu_add_item_ex with HELIOSVIEW_MENU_ITEM_CHECKED
 * (or 0) when `checked` is non-zero. See heliosview_menu_set_item_checked. */
HELIOSVIEW_API int heliosview_menu_add_checkable_item(heliosview_menu_t* menu, const char* text,
                                                      int checked, uint32_t* out_id);

/* Set (checked != 0) or clear the checkmark of the item with the given id.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_item_checked(heliosview_menu_t* menu, uint32_t id,
                                                    int checked);

/* Read whether the item with the given id is checked; 1/0 is written to
 * out_checked (NULL = ignore). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_is_item_checked(heliosview_menu_t* menu, uint32_t id,
                                                   int* out_checked);

/* Enable (enabled != 0) or disable (grayed out, not selectable) the item with
 * the given id. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_item_enabled(heliosview_menu_t* menu, uint32_t id,
                                                    int enabled);

/* Read whether the item with the given id is enabled; 1/0 is written to
 * out_enabled (NULL = ignore). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_is_item_enabled(heliosview_menu_t* menu, uint32_t id,
                                                   int* out_enabled);

/* Make (is_default != 0) or unmake the item with the given id the menu's
 * default item (shown bold; activated by Enter / double-click; one per menu).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_set_item_default(heliosview_menu_t* menu, uint32_t id,
                                                    int is_default);

/* Read whether the item with the given id is the default item; 1/0 is written
 * to out_default (NULL = ignore). 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_is_item_default(heliosview_menu_t* menu, uint32_t id,
                                                   int* out_default);

/* Add a separator. 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_separator(heliosview_menu_t* menu);

/* Add `submenu` as a submenu under `text`. The parent takes ownership of the
 * submenu. `submenu` must be a POPUP menu (heliosview_menu_create) — a menu bar
 * cannot be a submenu (Win32 MF_POPUP requires CreatePopupMenu handles).
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_add_submenu(heliosview_menu_t* menu, const char* text,
                                               heliosview_menu_t* submenu);

/* Add a top-level menu to the menu bar under `text` (e.g. "File", "Edit").
 * Alias for heliosview_menu_add_submenu on a menu bar. */
HELIOSVIEW_API int heliosview_menubar_add_menu(heliosview_menu_t* bar, const char* text,
                                               heliosview_menu_t* menu);

/* Show a POPUP menu (heliosview_menu_create) at the current cursor position.
 * `window` is the owner: it
 * receives the resulting MENU_SELECT event (window_id = its native handle), and
 * the popup is dismissed when the user clicks elsewhere. Pass NULL to show it
 * without an application window: the library uses a hidden owner window and the
 * event carries window_id = 0.
 * 0 = success, negative = error code. */
HELIOSVIEW_API int heliosview_menu_show(heliosview_menu_t* menu, heliosview_window_t* window);


#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_MENU_H */
