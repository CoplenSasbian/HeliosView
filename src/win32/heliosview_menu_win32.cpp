// HeliosView.dll — Windows menu backend (heliosview_action_* / heliosview_menu_* API).
//
// Two objects, deliberately separated:
//
//   heliosview_action_t  a command: label, enabled/checkable/checked state, a
//                        process-unique id and caller userdata. Window- and
//                        menu-free; may be displayed by any number of menus.
//   heliosview_menu_t    a display structure: an ordered list of entries
//                        (action / submenu / separator) over one HMENU.
//
// Routing has no id → object lookup table. Every menu is created with
// MNS_NOTIFYBYPOS (SetMenuInfo), so selecting an item does NOT send WM_COMMAND
// with a command id — the system sends WM_MENUCOMMAND instead:
//
//     WM_MENUCOMMAND   wParam = zero-based index of the selected item
//                      lParam = the HMENU that contains the item
//
// For a submenu item, lParam is the SUBMENU's own HMENU. Two OS-provided slots
// carry the objects:
//   - MENUINFO.dwMenuData (set at create) recovers the owning menu from the
//     message's HMENU via GetMenuInfo — no HMENU → menu registry;
//   - MENUITEMINFO.dwItemData (set at append) recovers the action from the
//     selected item — no id → action registry on this path.
//
// Action state is read back into the native items in WM_INITMENUPOPUP (right
// before a menu pops up), so an action shared by several menus always shows its
// current label/enabled/checked state without maintaining a reverse
// action → menus table.
//
// Lifetime: the menu object and its HMENU die together in heliosview_menu_destroy
// (submenus first, then this menu's hmenu, then the object). Actions are
// reference-counted by the menus displaying them, so a menu can never reference
// a freed action.

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"    /* hv::hv_alloc / hv_dealloc / event queue / action registry */
#include "heliosview_win32_internal.h" /* utf8_to_wide / hv_window_hwnd */

#include <cstdint>
#include <string>
#include <vector>

/* ================= Action ================= */

struct heliosview_action {
    std::string text;         /* UTF-8 label ("" = none) */
    std::string shortcut;     /* portable shortcut string ("" = none) */
    uint32_t id = 0;          /* process-unique id (hv::tls_actions key) */
    void* userdata = nullptr; /* caller data copied into MENU_SELECT events */
    heliosview_menu_role_t role = HELIOSVIEW_MENU_ROLE_NONE;
    int refcount = 1;         /* caller's reference + one per menu displaying it */
    bool enabled = true;
    bool checkable = false;
    bool checked = false;
    bool radio = false;       /* radio bullet instead of a checkmark */
};

namespace {

/* Accelerator table invalidation (defined below with the table itself). */
void invalidate_accelerators();

/* ---------- standard roles ----------
 *
 * Platform-provided defaults: label, conventional shortcut, and whether the
 * library performs the action itself (because the application cannot). See the
 * table in heliosview.h. */
struct hv_role_info {
    const char* label;
    const char* shortcut; /* portable string, "" = none */
    bool library_acts;    /* library performs the default action on selection */
};

hv_role_info role_info(heliosview_menu_role_t role)
{
    switch (role) {
    case HELIOSVIEW_MENU_ROLE_ABOUT:              return {"About", "", false};
    case HELIOSVIEW_MENU_ROLE_PREFERENCES:        return {"Preferences", "", false};
    case HELIOSVIEW_MENU_ROLE_QUIT:               return {"Exit", "", false};
    case HELIOSVIEW_MENU_ROLE_HIDE:               return {"Hide", "", false};
    case HELIOSVIEW_MENU_ROLE_HIDE_OTHERS:        return {"Hide Others", "", false};
    case HELIOSVIEW_MENU_ROLE_SHOW_ALL:           return {"Show All", "", false};
    case HELIOSVIEW_MENU_ROLE_SERVICES:           return {"Services", "", false};
    case HELIOSVIEW_MENU_ROLE_UNDO:               return {"Undo", "Ctrl+Z", true};
    case HELIOSVIEW_MENU_ROLE_REDO:               return {"Redo", "Ctrl+Y", false};
    case HELIOSVIEW_MENU_ROLE_CUT:                return {"Cut", "Ctrl+X", true};
    case HELIOSVIEW_MENU_ROLE_COPY:               return {"Copy", "Ctrl+C", true};
    case HELIOSVIEW_MENU_ROLE_PASTE:              return {"Paste", "Ctrl+V", true};
    case HELIOSVIEW_MENU_ROLE_SELECT_ALL:         return {"Select All", "Ctrl+A", false};
    case HELIOSVIEW_MENU_ROLE_DELETE:             return {"Delete", "Del", true};
    case HELIOSVIEW_MENU_ROLE_MINIMIZE:           return {"Minimize", "", true};
    case HELIOSVIEW_MENU_ROLE_ZOOM:               return {"Maximize", "", true};
    case HELIOSVIEW_MENU_ROLE_CLOSE_WINDOW:       return {"Close", "", true};
    case HELIOSVIEW_MENU_ROLE_TOGGLE_FULLSCREEN:  return {"Full Screen", "F11", true};
    case HELIOSVIEW_MENU_ROLE_BRING_ALL_TO_FRONT: return {"Bring All to Front", "", false};
    case HELIOSVIEW_MENU_ROLE_NONE:
    default:                                      return {"", "", false};
    }
}

void action_release(heliosview_action_t* action)
{
    if (!action || --action->refcount > 0)
        return;
    hv::hv_unregister_action(action->id);
    invalidate_accelerators(); /* its shortcut leaves the table */
    hv::hv_dealloc(action);
}

void action_retain(heliosview_action_t* action)
{
    if (action)
        action->refcount++;
}

/* ---------- application menu bar ----------
 * Windows has no global menu bar: the "application menu" is the menu bar of
 * every HeliosView window (see heliosview.h). Attached by
 * heliosview_menu_set_app_menu and re-applied to windows created later.
 * Defined with the menu object (the helpers need heliosview_menu). */
inline thread_local heliosview_menu_t* tls_app_menu = nullptr;

void apply_app_menu_to_window(HWND hwnd);

/* ---------- menu lifetime ----------
 * A menu is reference-counted like an action: the caller holds one reference
 * (dropped by heliosview_menu_destroy), and every holder that displays the menu
 * (a parent menu, the application menu bar, an attached tray) holds one more.
 * The object — and its HMENU — is freed when the last reference goes away.
 * Defined with the menu object below (menu_retain touches its refcount). */
void menu_retain(heliosview_menu_t* menu);
void menu_release(heliosview_menu_t* menu);

/* ---------- keyboard accelerators ----------
 * Every live action with a shortcut is an application-wide accelerator. The
 * table is rebuilt lazily: CreateAcceleratorTableW needs the whole set, and
 * shortcuts change rarely. */
inline thread_local HACCEL tls_accel = nullptr;
inline thread_local bool tls_accel_dirty = true;

HACCEL accelerators();

} // namespace

heliosview_action_t* heliosview_action_create(const char* text, void* userdata)
{
    auto* action = hv::hv_alloc<heliosview_action>();
    if (text)
        action->text = text;
    action->userdata = userdata;
    action->id = hv::hv_register_action(action);
    if (action->id == 0) {
        hv::hv_dealloc(action);
        return nullptr;
    }
    return action;
}

heliosview_action_t* heliosview_action_create_role(heliosview_menu_role_t role,
                                                   const char* text, void* userdata)
{
    if (role == HELIOSVIEW_MENU_ROLE_NONE)
        return heliosview_action_create(text, userdata);
    const hv_role_info info = role_info(role);
    heliosview_action_t* action = heliosview_action_create(
        (text && *text) ? text : info.label, userdata);
    if (!action)
        return nullptr;
    action->role = role;
    if ((!text || !*text) && info.shortcut && *info.shortcut) {
        action->shortcut = info.shortcut;
        invalidate_accelerators();
    }
    return action;
}

heliosview_menu_role_t heliosview_action_role(const heliosview_action_t* action)
{
    return action ? action->role : HELIOSVIEW_MENU_ROLE_NONE;
}

void heliosview_action_destroy(heliosview_action_t* action)
{
    action_release(action);
}

int heliosview_action_set_text(heliosview_action_t* action, const char* text)
{
    if (!action)
        return hv_fail(-1, "action is NULL");
    action->text = text ? text : "";
    return 0;
}

int heliosview_action_set_shortcut(heliosview_action_t* action, const char* shortcut)
{
    if (!action)
        return hv_fail(-1, "action is NULL");
    if (!shortcut || !*shortcut) {
        action->shortcut.clear();
        return 0;
    }
    if (hv_parse_shortcut(shortcut).vk == 0)
        return hv_fail(-1, "invalid shortcut (expected e.g. \"Primary+S\", \"Ctrl+Shift+Z\", \"F11\")");
    action->shortcut = shortcut;
    invalidate_accelerators();
    return 0;
}

const char* heliosview_action_shortcut(const heliosview_action_t* action)
{
    return action ? action->shortcut.c_str() : "";
}

int heliosview_action_set_enabled(heliosview_action_t* action, int enabled)
{
    if (!action)
        return hv_fail(-1, "action is NULL");
    action->enabled = enabled != 0;
    return 0;
}

int heliosview_action_set_checkable(heliosview_action_t* action, int checkable)
{
    if (!action)
        return hv_fail(-1, "action is NULL");
    action->checkable = checkable != 0;
    return 0;
}

int heliosview_action_set_radio_style(heliosview_action_t* action, int radio)
{
    if (!action)
        return hv_fail(-1, "action is NULL");
    action->radio = radio != 0;
    return 0;
}

int heliosview_action_set_checked(heliosview_action_t* action, int checked)
{
    if (!action)
        return hv_fail(-1, "action is NULL");
    action->checked = checked != 0;
    return 0;
}

int heliosview_action_is_enabled(const heliosview_action_t* action)
{
    return action && action->enabled;
}

int heliosview_action_is_checked(const heliosview_action_t* action)
{
    return action && action->checked;
}

int heliosview_action_is_checkable(const heliosview_action_t* action)
{
    return action && action->checkable;
}

int heliosview_action_is_radio_style(const heliosview_action_t* action)
{
    return action && action->radio;
}

const char* heliosview_action_text(const heliosview_action_t* action)
{
    return action ? action->text.c_str() : "";
}

uint32_t heliosview_action_id(const heliosview_action_t* action)
{
    return action ? action->id : 0;
}

/* ================= Menu ================= */

struct heliosview_menu_entry {
    enum class Kind { Action, Submenu, Separator } kind = Kind::Separator;
    heliosview_action_t* action = nullptr; /* Kind::Action (borrowed; refcounted) */
    heliosview_menu_t* submenu = nullptr;  /* Kind::Submenu (owned) */
    bool is_default = false;               /* per-menu property: Enter activates it */
};

struct heliosview_menu {
    HWND hwnd = nullptr;                     /* owner window used the last time the popup was shown */
    HMENU hmenu = nullptr;                   /* Win32 menu (MNS_NOTIFYBYPOS) — the routing identity */
    bool is_bar = false;                     /* true = CreateMenu (window menu bar); false = CreatePopupMenu */
    void* userdata = nullptr;                /* caller data (the C++ wrapper stores an object pointer) */
    uint32_t next_item_id = 1;               /* menu-local command id allocator (only for id-less entries) */
    heliosview_menu_kind_t kind = HELIOSVIEW_MENU_KIND_NORMAL; /* standard-menu hint (macOS wires it) */
    heliosview_menu_open_cb open_cb = nullptr; /* lazy population, called before the menu opens */
    void* open_cb_userdata = nullptr;
    int refcount = 1;                        /* caller's reference + one per parent menu / app menu / tray */
    std::vector<heliosview_menu_entry> entries;
};

namespace {

/* ---------- application menu bar + accelerators (see the declarations above) ---------- */

void apply_app_menu_to_window(HWND hwnd)
{
    if (!hwnd)
        return;
    SetMenu(hwnd, tls_app_menu ? tls_app_menu->hmenu : nullptr);
    DrawMenuBar(hwnd);
}

/* Last reference gone: detach from the app-menu slot and every window (a window
 * must never keep an HMENU that is about to be destroyed), then free the tree.
 * Submenus (and their HMENUs / dwMenuData) go first, then this menu's HMENU,
 * then the actions this menu displays — their pointers live in dwItemData, which
 * is read while the HMENU is alive. */
void menu_retain(heliosview_menu_t* menu)
{
    if (menu)
        menu->refcount++;
}

void menu_release(heliosview_menu_t* menu)
{
    if (!menu || --menu->refcount > 0)
        return;
    if (tls_app_menu == menu) {
        tls_app_menu = nullptr;
        for (const auto& [window_id, window] : hv::tls_windows) {
            (void)window;
            apply_app_menu_to_window(reinterpret_cast<HWND>(window_id));
        }
    }
    std::vector<heliosview_action_t*> actions;
    std::vector<heliosview_menu_t*> submenus;
    for (auto& entry : menu->entries) {
        if (entry.kind == heliosview_menu_entry::Kind::Submenu && entry.submenu)
            submenus.push_back(entry.submenu);
        else if (entry.kind == heliosview_menu_entry::Kind::Action && entry.action)
            actions.push_back(entry.action);
    }
    menu->entries.clear();
    if (menu->hmenu)
        DestroyMenu(menu->hmenu);
    for (heliosview_menu_t* submenu : submenus)
        menu_release(submenu); /* the parent's reference */
    for (heliosview_action_t* action : actions)
        action_release(action); /* the last release frees it */
    hv::hv_dealloc(menu);
}

void invalidate_accelerators()
{
    tls_accel_dirty = true;
}

HACCEL accelerators()
{
    if (!tls_accel_dirty)
        return tls_accel;
    tls_accel_dirty = false;

    std::vector<ACCEL> entries;
    for (const auto& [id, object] : hv::tls_actions) {
        const auto* action = static_cast<const heliosview_action_t*>(object);
        if (!action || action->shortcut.empty())
            continue;
        const hv_shortcut sc = hv_parse_shortcut(action->shortcut);
        if (sc.vk == 0)
            continue;
        ACCEL accel{};
        accel.fVirt = static_cast<BYTE>(FVIRTKEY | sc.mods);
        accel.key = static_cast<WORD>(sc.vk);
        accel.cmd = static_cast<WORD>(action->id); /* ids start at 1 and are small */
        entries.push_back(accel);
        (void)id;
    }
    if (tls_accel) {
        DestroyAcceleratorTable(tls_accel);
        tls_accel = nullptr;
    }
    if (!entries.empty())
        tls_accel = CreateAcceleratorTableW(entries.data(), static_cast<int>(entries.size()));
    return tls_accel;
}

/* Menu message handling, shared by our window procedures (defined below). */
bool menu_handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/* Recover the menu owning `hmenu` (MENUINFO.dwMenuData was set at create). */
heliosview_menu_t* menu_from_hmenu(HMENU hmenu)
{
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_MENUDATA;
    if (!GetMenuInfo(hmenu, &mi))
        return nullptr;
    return reinterpret_cast<heliosview_menu_t*>(mi.dwMenuData);
}

/* Recover the action behind item `index` of `hmenu` (dwItemData was set when
 * the item was appended); null for separators/submenus. */
heliosview_action_t* action_from_item(HMENU hmenu, int index)
{
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(hmenu, static_cast<UINT>(index), TRUE, &mii))
        return nullptr;
    return reinterpret_cast<heliosview_action_t*>(mii.dwItemData);
}

/* Push an action's current state into one native item (label, enabled, check,
 * default). `is_default` is a per-menu property, not an action property. */
void apply_action_to_item(HMENU hmenu, UINT index, const heliosview_action_t* action, bool is_default)
{
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_STRING | MIIM_DATA;
    mii.fType = MFT_STRING | (action->radio ? MFT_RADIOCHECK : 0);
    mii.fState = 0;
    if (!action->enabled)
        mii.fState |= MFS_DISABLED | MFS_GRAYED;
    if (action->checked)
        mii.fState |= MFS_CHECKED;
    if (is_default)
        mii.fState |= MFS_DEFAULT;
    /* Windows renders the accelerator after a tab in the item text. */
    std::wstring label = utf8_to_wide(action->text);
    if (!action->shortcut.empty()) {
        const std::wstring accel = hv_shortcut_display(hv_parse_shortcut(action->shortcut));
        if (!accel.empty())
            label += L"\t" + accel;
    }
    mii.dwTypeData = const_cast<wchar_t*>(label.c_str());
    mii.cch = static_cast<UINT>(label.size());
    mii.dwItemData = reinterpret_cast<ULONG_PTR>(const_cast<heliosview_action_t*>(action));
    SetMenuItemInfoW(hmenu, index, TRUE, &mii);
}

/* Refresh every action-backed item of `menu` from its action. Called from
 * WM_INITMENUPOPUP (and before a popup opens), so an action shared by several
 * menus always shows its current state without any action → menus bookkeeping.
 * Entry order matches item positions: entries are only appended, never removed. */
void refresh_menu(heliosview_menu_t* menu)
{
    if (!menu || !menu->hmenu)
        return;
    for (size_t i = 0; i < menu->entries.size(); ++i) {
        const auto& entry = menu->entries[i];
        if (entry.kind != heliosview_menu_entry::Kind::Action || !entry.action)
            continue;
        apply_action_to_item(menu->hmenu, static_cast<UINT>(i), entry.action, entry.is_default);
    }
}

/* Perform a role's default action where the library, not the application, owns
 * it (edit commands to the focused control, window commands on the active
 * window). Roles the application owns (Quit, About, ...) only post the event. */
void perform_role_action(HWND owner, heliosview_menu_role_t role)
{
    if (role == HELIOSVIEW_MENU_ROLE_NONE || !role_info(role).library_acts)
        return;

    /* Edit commands go to whatever has the keyboard focus (a native control or
     * the WebView's window, which handles them itself). */
    auto send_to_focus = [](UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
        HWND target = GetFocus();
        if (!target)
            target = GetForegroundWindow();
        if (target)
            SendMessageW(target, message, wparam, lparam);
    };

    switch (role) {
    case HELIOSVIEW_MENU_ROLE_UNDO:       send_to_focus(WM_UNDO); break;
    case HELIOSVIEW_MENU_ROLE_CUT:        send_to_focus(WM_CUT); break;
    case HELIOSVIEW_MENU_ROLE_COPY:       send_to_focus(WM_COPY); break;
    case HELIOSVIEW_MENU_ROLE_PASTE:      send_to_focus(WM_PASTE); break;
    case HELIOSVIEW_MENU_ROLE_DELETE:     send_to_focus(WM_CLEAR); break;
    default:
        break;
    }

    /* Window commands act on the active HeliosView window (falling back to the
     * window whose menu was used). */
    heliosview_window_t* window = heliosview_window_from_id(
        reinterpret_cast<uintptr_t>(GetForegroundWindow()));
    if (!window)
        window = heliosview_window_from_id(reinterpret_cast<uintptr_t>(owner));
    if (!window)
        return;
    switch (role) {
    case HELIOSVIEW_MENU_ROLE_MINIMIZE:          heliosview_window_minimize(window); break;
    case HELIOSVIEW_MENU_ROLE_ZOOM:              heliosview_window_toggle_maximize(window); break;
    case HELIOSVIEW_MENU_ROLE_CLOSE_WINDOW:      heliosview_window_close(window); break;
    case HELIOSVIEW_MENU_ROLE_TOGGLE_FULLSCREEN: heliosview_window_set_fullscreen(
                                                     window, !heliosview_window_is_fullscreen(window));
                                                 break;
    default:
        break;
    }
}

/* Queue a MENU_SELECT event for an action chosen in a menu owned by `hwnd`. */
void post_menu_select(HWND hwnd, heliosview_action_t* action)
{
    heliosview_event_t ev{};
    /* the hidden host owner is a library-internal window: report window_id = 0
     * rather than an HWND the caller never saw */
    ev.window_id = hv_host_is(hwnd) ? 0 : reinterpret_cast<uintptr_t>(hwnd);
    ev.timestamp_ms = hv::now_ms();
    ev.type = HELIOSVIEW_EVENT_MENU_SELECT;
    ev.menu_item = action->id;
    ev.userdata = action->userdata;
    hv::queue_push(ev);

    /* role actions the library owns run right away; the event is already queued
     * so the application can still observe the selection */
    perform_role_action(hwnd, action->role);
}

/* Route a selection in `hmenu` (owned by `hwnd`) to its action. Returns true when
 * the menu is ours (the message is consumed whether or not the item maps to an
 * action — a separator/submenu selection must not fall through to DefWindowProc). */
bool menu_handle_command(HWND hwnd, HMENU hmenu, int index)
{
    heliosview_menu_t* menu = menu_from_hmenu(hmenu);
    if (!menu)
        return false;
    if (heliosview_action_t* action = action_from_item(hmenu, index))
        post_menu_select(hwnd, action);
    return true;
}

/* comctl32 subclass fallback: some menu selections are delivered through the
 * subclass chain rather than the window procedure. Whichever runs first consumes
 * the message, so a selection can never produce two events. */
LRESULT CALLBACK hv_menu_subclass_proc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MENUCOMMAND
        && menu_handle_command(hwnd, reinterpret_cast<HMENU>(lparam), static_cast<int>(wparam)))
        return 0;
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void menu_ensure_window_subclass(HWND hwnd)
{
    if (hwnd)
        SetWindowSubclass(hwnd, hv_menu_subclass_proc, 0, 0);
}

/* Handle a menu message in the window procedure of a window that owns a menu.
 * Called from heliosview_wndproc_t (our windows) and from the hidden host's
 * procedure; returns true when the message was consumed.
 *
 * Deliberately NOT implemented as a comctl32 subclass: WM_MENUCOMMAND is not
 * delivered through the SetWindowSubclass chain (verified on Windows 11), so
 * the routing has to live in the real window procedure. Both windows involved
 * are ours, so that is always possible. */
bool menu_handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITMENUPOPUP) {
        /* wParam = the menu about to become active (NOT lParam: that carries the
         * position of the item that opened it). Let the menu populate itself,
         * then refresh action state, so an action shared by several menus always
         * shows its current state. */
        if (heliosview_menu_t* menu = menu_from_hmenu(reinterpret_cast<HMENU>(wparam))) {
            if (menu->open_cb)
                menu->open_cb(menu, menu->open_cb_userdata);
            refresh_menu(menu);
        }
        return false; /* let the default procedure continue */
    }
    if (message == WM_MENUCOMMAND)
        return menu_handle_command(hwnd, reinterpret_cast<HMENU>(lparam), static_cast<int>(wparam));
    if (message == WM_COMMAND) {
        /* Accelerators (and any menu that does not use MNS_NOTIFYBYPOS) deliver
         * only the command id: resolve it through the action registry.
         * HIWORD is 0 for menu commands and 1 for accelerator commands; anything
         * else is a notification from a child control. */
        const uint32_t id = LOWORD(wparam);
        if (id != 0 && HIWORD(wparam) <= 1) {
            if (auto* action = static_cast<heliosview_action_t*>(hv::hv_find_action(id))) {
                post_menu_select(hwnd, action);
                return true;
            }
        }
    }
    return false;
}

/* Enable MNS_NOTIFYBYPOS on a menu and store the owning C object in its
 * MENUINFO.dwMenuData (the routing callout reads it back from WM_MENUCOMMAND's
 * lParam HMENU). MIM_APPLYTOSUBMENUS propagates the STYLE to submenus; the
 * MENUDATA must NOT ride along for submenu items, which is why every submenu is
 * created through heliosview_menu_create (setting its OWN dwMenuData before it
 * is attached — construction order matters). */
void menu_enable_notify_by_pos(HMENU hmenu, heliosview_menu_t* menu)
{
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE | MIM_APPLYTOSUBMENUS | MIM_MENUDATA;
    mi.dwStyle = MNS_NOTIFYBYPOS;
    mi.dwMenuData = reinterpret_cast<uintptr_t>(menu);
    SetMenuInfo(hmenu, &mi);
}

/* Append an action-backed item. `take_creation_ref` = the caller created the
 * action itself and hands its reference over to the menu (used by the compat
 * add_item helpers, whose action is invisible to the caller). */
int menu_append_action(heliosview_menu_t* menu, heliosview_action_t* action,
                       bool take_creation_ref, bool is_default, uint32_t* out_id)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    if (!action)
        return hv_fail(-1, "action is NULL");

    const std::wstring wtext = utf8_to_wide(action->text);
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_STRING | MIIM_ID | MIIM_DATA;
    mii.fType = MFT_STRING | (action->radio ? MFT_RADIOCHECK : 0);
    mii.fState = (action->enabled ? MFS_ENABLED : (MFS_DISABLED | MFS_GRAYED))
               | (action->checked ? MFS_CHECKED : 0)
               | (is_default ? MFS_DEFAULT : 0);
    mii.wID = action->id; /* stable per-action id: what MENU_SELECT reports */
    mii.dwTypeData = const_cast<wchar_t*>(wtext.c_str());
    mii.cch = static_cast<UINT>(wtext.size());
    mii.dwItemData = reinterpret_cast<ULONG_PTR>(action);
    if (!InsertMenuItemW(menu->hmenu, static_cast<UINT>(menu->entries.size()), TRUE, &mii))
        return hv_fail(-1, "InsertMenuItemW failed");

    heliosview_menu_entry entry;
    entry.kind = heliosview_menu_entry::Kind::Action;
    entry.action = action;
    entry.is_default = is_default;
    menu->entries.push_back(entry);
    action_retain(action);
    if (take_creation_ref)
        action_release(action); /* the menu's reference is the only one left */

    if (out_id)
        *out_id = action->id;
    return 0;
}

/* Find the entry displaying `action` in this menu (NULL = not in this menu). */
heliosview_menu_entry* find_entry(heliosview_menu_t* menu, const heliosview_action_t* action)
{
    if (!menu || !action)
        return nullptr;
    for (auto& entry : menu->entries) {
        if (entry.kind == heliosview_menu_entry::Kind::Action && entry.action == action)
            return &entry;
    }
    return nullptr;
}

heliosview_action_t* find_action_by_id(heliosview_menu_t* menu, uint32_t id)
{
    if (!menu)
        return nullptr;
    for (auto& entry : menu->entries) {
        if (entry.kind == heliosview_menu_entry::Kind::Action && entry.action
            && entry.action->id == id)
            return entry.action;
    }
    return nullptr;
}

} // namespace

/* Called by the window procedures (heliosview_wndproc_t for our windows and the
 * hidden host window). */
bool hv_menu_handle_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return menu_handle_message(hwnd, message, wparam, lparam);
}

/* Called by the window backend (heliosview_window_create_ex) so windows created
 * after heliosview_menu_set_app_menu get the current menu bar too. */
void hv_menu_apply_app_menu(HWND hwnd)
{
    apply_app_menu_to_window(hwnd);
}

/* Called by other subsystems that display a menu (the tray's attached context
 * menu), so the menu's HMENU outlives its holder. */
void hv_menu_retain(heliosview_menu_t* menu)
{
    menu_retain(menu);
}

void hv_menu_release(heliosview_menu_t* menu)
{
    menu_release(menu);
}

bool hv_menu_is_bar(heliosview_menu_t* menu)
{
    return menu && menu->is_bar;
}

/* Called by the message loop (and by heliosview_translate_accelerator for apps
 * that run their own loop). True = the message was consumed as an accelerator. */
bool hv_menu_translate_accelerator(MSG* msg)
{
    if (!msg)
        return false;
    HACCEL table = accelerators();
    if (!table)
        return false;
    return TranslateAcceleratorW(msg->hwnd, table, msg) != 0;
}

int heliosview_translate_accelerator(void* native_msg)
{
    return hv_menu_translate_accelerator(static_cast<MSG*>(native_msg)) ? 1 : 0;
}

int heliosview_menu_set_app_menu(heliosview_menu_t* menu)
{
    if (menu && !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    /* Only a MENU BAR (CreateMenu) can be attached to windows with SetMenu: the
     * Win32 API rejects a popup menu (CreatePopupMenu) with ERROR_INVALID_PARAMETER
     * (see heliosview_menu_create_bar). */
    if (menu && !menu->is_bar)
        return hv_fail(-1, "the application menu bar must be created with "
                           "heliosview_menu_create_bar (a window menu bar is a "
                           "CreateMenu; popup menus cannot be attached to windows)");
    if (tls_app_menu == menu)
        return 0;
    heliosview_menu_t* previous = tls_app_menu;
    tls_app_menu = menu;
    menu_retain(menu); /* the bar holds a reference while it displays it */
    /* apply to every live window on this thread (the registry keys are HWNDs) */
    for (const auto& [window_id, window] : hv::tls_windows) {
        (void)window;
        apply_app_menu_to_window(reinterpret_cast<HWND>(window_id));
    }
    menu_release(previous);
    return 0;
}

heliosview_menu_t* heliosview_menu_app_menu()
{
    return tls_app_menu;
}

heliosview_menu_t* heliosview_menu_create(void* userdata)
{
    auto* menu = hv::hv_alloc<heliosview_menu>();
    menu->userdata = userdata;
    /* A POPUP menu (CreatePopupMenu): shown with heliosview_menu_show, attached
     * to a tray, or added as a submenu. NOT a window menu bar — for that use
     * heliosview_menu_create_bar (CreateMenu): only a menu bar can be attached
     * to a window with SetMenu; popup handles are rejected there. */
    menu->hmenu = CreatePopupMenu();
    if (!menu->hmenu) {
        hv_fail(-1, "CreatePopupMenu failed");
        hv::hv_dealloc(menu);
        return nullptr;
    }
    menu_enable_notify_by_pos(menu->hmenu, menu); /* WM_MENUCOMMAND routing */
    return menu;
}

heliosview_menu_t* heliosview_menu_create_bar(void* userdata)
{
    auto* menu = hv::hv_alloc<heliosview_menu>();
    menu->userdata = userdata;
    menu->is_bar = true;
    /* A window MENU BAR (CreateMenu). Only this kind can be installed as the
     * application menu bar (SetMenu). It holds top-level items — typically
     * submenus (popups) — and is never shown with TrackPopupMenu. */
    menu->hmenu = CreateMenu();
    if (!menu->hmenu) {
        hv_fail(-1, "CreateMenu failed");
        hv::hv_dealloc(menu);
        return nullptr;
    }
    menu_enable_notify_by_pos(menu->hmenu, menu); /* WM_MENUCOMMAND routing */
    return menu;
}

void heliosview_menu_destroy(heliosview_menu_t* menu)
{
    /* Drops the caller's reference; the menu (and its HMENU) is freed only when
     * nothing else displays it any more — a parent menu, the application menu
     * bar or an attached tray may still hold a reference. */
    menu_release(menu);
}

int heliosview_menu_add_action(heliosview_menu_t* menu, heliosview_action_t* action)
{
    return menu_append_action(menu, action, /*take_creation_ref=*/false, /*is_default=*/false, nullptr);
}

int heliosview_menu_set_kind(heliosview_menu_t* menu, heliosview_menu_kind_t kind)
{
    if (!menu)
        return hv_fail(-1, "menu is NULL");
    /* Windows has no system-provided standard menus, so the kind is a stored
     * hint (the macOS backend wires NSApp.servicesMenu / windowsMenu / helpMenu
     * and the App menu with it). Validated here so a typo cannot pass silently. */
    if (kind < HELIOSVIEW_MENU_KIND_NORMAL || kind > HELIOSVIEW_MENU_KIND_HELP)
        return hv_fail(-2, "unknown menu kind");
    menu->kind = kind;
    return 0;
}

heliosview_menu_kind_t heliosview_menu_kind(const heliosview_menu_t* menu)
{
    return menu ? menu->kind : HELIOSVIEW_MENU_KIND_NORMAL;
}

int heliosview_menu_set_open_callback(heliosview_menu_t* menu, heliosview_menu_open_cb callback,
                                      void* userdata)
{
    if (!menu)
        return hv_fail(-1, "menu is NULL");
    menu->open_cb = callback;
    menu->open_cb_userdata = userdata;
    return 0;
}

int heliosview_menu_set_default_action(heliosview_menu_t* menu, heliosview_action_t* action)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    if (action && !find_entry(menu, action))
        return hv_fail(-1, "the action is not in this menu");
    for (size_t i = 0; i < menu->entries.size(); ++i) {
        auto& entry = menu->entries[i];
        if (entry.kind != heliosview_menu_entry::Kind::Action)
            continue;
        entry.is_default = (entry.action == action);
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STATE;
        mii.fState = entry.is_default ? MFS_DEFAULT : 0;
        SetMenuItemInfoW(menu->hmenu, static_cast<UINT>(i), TRUE, &mii);
    }
    return 0;
}

int heliosview_menu_add_item(heliosview_menu_t* menu, const char* text, uint32_t* out_id)
{
    heliosview_action_t* action = heliosview_action_create(text, nullptr);
    if (!action)
        return hv_fail(-1, "failed to create the item's action");
    const int rc = menu_append_action(menu, action, /*take_creation_ref=*/true, /*is_default=*/false, out_id);
    if (rc != 0)
        heliosview_action_destroy(action); /* append failed: drop the creation reference */
    return rc;
}

int heliosview_menu_add_item_ex(heliosview_menu_t* menu, const char* text,
                                uint32_t flags, uint32_t* out_id)
{
    heliosview_action_t* action = heliosview_action_create(text, nullptr);
    if (!action)
        return hv_fail(-1, "failed to create the item's action");
    if (flags & HELIOSVIEW_MENU_ITEM_DISABLED)
        action->enabled = false;
    if (flags & HELIOSVIEW_MENU_ITEM_CHECKED) {
        action->checkable = true;
        action->checked = true;
    }
    if (flags & HELIOSVIEW_MENU_ITEM_RADIOCHECK)
        action->radio = true;
    const bool is_default = (flags & HELIOSVIEW_MENU_ITEM_DEFAULT) != 0;
    const int rc = menu_append_action(menu, action, /*take_creation_ref=*/true, is_default, out_id);
    if (rc != 0)
        heliosview_action_destroy(action);
    return rc;
}

int heliosview_menu_add_checkable_item(heliosview_menu_t* menu, const char* text,
                                       int checked, uint32_t* out_id)
{
    return heliosview_menu_add_item_ex(menu, text,
                                       checked ? HELIOSVIEW_MENU_ITEM_CHECKED : 0, out_id);
}

int heliosview_menu_add_separator(heliosview_menu_t* menu)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    if (!AppendMenuW(menu->hmenu, MF_SEPARATOR, 0, nullptr))
        return hv_fail(-1, "AppendMenuW (separator) failed");
    heliosview_menu_entry entry;
    entry.kind = heliosview_menu_entry::Kind::Separator;
    menu->entries.push_back(entry);
    return 0;
}

int heliosview_menu_add_submenu(heliosview_menu_t* menu, const char* text,
                                heliosview_menu_t* submenu)
{
    if (!menu || !menu->hmenu || !submenu || !submenu->hmenu)
        return hv_fail(-1, "invalid menu or submenu (hmenu is NULL)");
    /* MF_POPUP items must be POPUP menus (CreatePopupMenu, see
     * heliosview_menu_create); a menu bar is not a valid submenu. */
    if (submenu->is_bar)
        return hv_fail(-1, "a submenu must be a popup menu (heliosview_menu_create), "
                           "not a menu bar (heliosview_menu_create_bar)");
    const std::wstring wtext = utf8_to_wide(text ? text : "");
    if (!AppendMenuW(menu->hmenu, MF_POPUP,
                     reinterpret_cast<UINT_PTR>(submenu->hmenu), wtext.c_str()))
        return hv_fail(-1, "AppendMenuW (submenu) failed");
    heliosview_menu_entry entry;
    entry.kind = heliosview_menu_entry::Kind::Submenu;
    entry.submenu = submenu; /* the parent holds a reference to the submenu */
    menu->entries.push_back(entry);
    menu_retain(submenu);
    return 0;
}

int heliosview_menu_set_item_checked(heliosview_menu_t* menu, uint32_t id, int checked)
{
    heliosview_action_t* action = find_action_by_id(menu, id);
    if (!action)
        return hv_fail(-1, "menu item id not found");
    action->checkable = true; /* the legacy API allowed checking any item */
    action->checked = checked != 0;
    return 0;
}

int heliosview_menu_is_item_checked(heliosview_menu_t* menu, uint32_t id, int* out_checked)
{
    heliosview_action_t* action = find_action_by_id(menu, id);
    if (!action)
        return hv_fail(-1, "menu item id not found");
    if (out_checked)
        *out_checked = action->checked ? 1 : 0;
    return 0;
}

int heliosview_menu_set_item_enabled(heliosview_menu_t* menu, uint32_t id, int enabled)
{
    heliosview_action_t* action = find_action_by_id(menu, id);
    if (!action)
        return hv_fail(-1, "menu item id not found");
    action->enabled = enabled != 0;
    return 0;
}

int heliosview_menu_is_item_enabled(heliosview_menu_t* menu, uint32_t id, int* out_enabled)
{
    heliosview_action_t* action = find_action_by_id(menu, id);
    if (!action)
        return hv_fail(-1, "menu item id not found");
    if (out_enabled)
        *out_enabled = action->enabled ? 1 : 0;
    return 0;
}

int heliosview_menu_set_item_default(heliosview_menu_t* menu, uint32_t id, int is_default)
{
    heliosview_action_t* action = find_action_by_id(menu, id);
    if (!action)
        return hv_fail(-1, "menu item id not found");
    return heliosview_menu_set_default_action(menu, is_default ? action : nullptr);
}

int heliosview_menu_is_item_default(heliosview_menu_t* menu, uint32_t id, int* out_default)
{
    heliosview_action_t* action = find_action_by_id(menu, id);
    if (!action)
        return hv_fail(-1, "menu item id not found");
    heliosview_menu_entry* entry = find_entry(menu, action);
    if (out_default)
        *out_default = (entry && entry->is_default) ? 1 : 0;
    return 0;
}

int heliosview_menu_show(heliosview_menu_t* menu, heliosview_window_t* window)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    /* Only POPUP menus are shown: a menu bar is a window's menu, never a
     * popup (matches the other backends' contract — see heliosview.h). */
    if (menu->is_bar)
        return hv_fail(-1, "a menu bar cannot be shown as a popup; show() takes a "
                           "popup menu (heliosview_menu_create)");
    /* Owner: the caller's window, or the hidden host when there is none (tray-only
     * apps). TrackPopupMenu needs a foreground owner to dismiss on outside clicks
     * and to deliver WM_MENUCOMMAND, so the routing callout goes on whichever
     * window ends up owning the popup. */
    HWND hwnd = window ? hv_window_hwnd(window)
                       : hv_host_window(&hv_menu_handle_message);
    if (!hwnd)
        return hv_fail(-1, window ? "invalid owner window (native window not created)"
                                  : "failed to create the hidden menu owner window");
    menu->hwnd = hwnd;
    menu_ensure_window_subclass(hwnd); /* fallback path for selections (see above) */
    refresh_menu(menu); /* states are current before the popup opens */
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu->hmenu,
                   TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, hwnd, nullptr);
    return 0;
}


