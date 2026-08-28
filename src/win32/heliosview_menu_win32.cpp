// HeliosView.dll — Windows popup-menu backend (heliosview_menu_* API).
//
// A popup menu is a Win32 HMENU shown with TrackPopupMenu. Every menu is
// created with MNS_NOTIFYBYPOS (SetMenuInfo), so selecting an item does NOT
// send WM_COMMAND with a command id — the system sends WM_MENUCOMMAND instead:
//
//     WM_MENUCOMMAND   wParam = zero-based index of the selected item
//                      lParam = the HMENU that contains the item
//
// For a submenu item, lParam is the SUBMENU's own HMENU. Each menu stores its C
// object (heliosview_menu_t*) in the OS menu structure itself — MENUINFO's
// dwMenuData is set at create (SetMenuInfo, MIM_MENUDATA) — so the callout
// recovers the owning menu straight from the message's HMENU via GetMenuInfo:
//
//     WM_MENUCOMMAND ──► GetMenuInfo(lParam HMENU) ──► dwMenuData = the menu
//
// no id → menu mapping, no open-menu state, no registries, no TLS. Item command
// ids are menu-local (never used for routing). MIM_APPLYTOSUBMENUS propagates
// the MNS_NOTIFYBYPOS style; construction order matters: every submenu is
// created through heliosview_menu_create and therefore sets its OWN dwMenuData
// before being attached, so a submenu item resolves to the SUBMENU.
//
// Lifetime: the C object and its HMENU die together in heliosview_menu_destroy
// (submenus first, then this menu's hmenu, then the object), so dwMenuData is
// only ever read while the menu is alive — a destroyed menu can never be
// reached by a late WM_MENUCOMMAND (fail-closed by the OS: no hmenu, no
// message).

#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"    /* hv::hv_alloc / hv_dealloc / event queue */
#include "heliosview_win32_internal.h" /* utf8_to_wide / hv_window_hwnd */

#include <cstdint>
#include <string>
#include <vector>

/* ================= Menu (CreatePopupMenu / TrackPopupMenu) ================= */

struct heliosview_menu {
    heliosview_window_t* window = nullptr;   /* owner window (pointer identity for the destroy guard) */
    HWND hwnd = nullptr;                     /* owner window's native handle (captured at create / lazily on first use) */
    HMENU hmenu = nullptr;                   /* Win32 popup menu (MNS_NOTIFYBYPOS) — the routing identity */
    void* userdata = nullptr;                /* caller data (the C++ wrapper stores an object pointer) */
    uint32_t next_item_id = 1;               /* menu-local item command id allocator (routing does not use ids) */
    std::vector<heliosview_menu_t*> submenus; /* owned submenus (destroyed with parent) */
};

namespace {

/* Subclass key of the menu callout. SetWindowSubclass identifies an entry by
 * the (pfnSubclass, uIdSubclass) PAIR, so the value only needs to be unique
 * among entries of the SAME proc on the same window — and the menu installs at
 * most one hv_menu_subclass_proc entry per window. Any constant works: 0 is
 * fine, and coexists with the WebView's (hv_webview_subclass_proc, 0) and the
 * tray's (hv_tray_subclass_proc, <msg id>) because those are different procs. */
constexpr UINT_PTR kMenuSubclassKey = 0;

/* The menu routing callout: installed ONCE per window (fixed key), shared by
 * all menus on it. A subclass receives every message of its window, so the
 * callout just filters WM_MENUCOMMAND and resolves the owning menu from the
 * message's HMENU via MENUINFO.dwMenuData (set at create; live exactly as long
 * as the HMENU, see the header comment). Every other message is forwarded
 * (DefSubclassProc). */
LRESULT CALLBACK hv_menu_subclass_proc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MENUCOMMAND) {
            HMENU hMenu  = (HMENU)lparam;
            int item_index = static_cast<int>(wparam);
            MENUINFO mi{};
            mi.cbSize = sizeof(MENUINFO);
            mi.fMask = MIM_MENUDATA;
            GetMenuInfo(hMenu, &mi);
            {
                auto* menu = reinterpret_cast<heliosview_menu_t*> (mi.dwMenuData);
                if (menu) {
                    /* GetMenuItemID returns UINT; (UINT)-1 (0xFFFFFFFF) is its
                     * sentinel for separator / submenu entries, which are never
                     * selectable — compare against the unsigned sentinel. */
                    const UINT id = GetMenuItemID(hMenu,item_index);
                    if (id != static_cast<UINT>(-1)) {
                        heliosview_event_t ev{};
                        ev.window_id = reinterpret_cast<uintptr_t>(hwnd);
                        ev.timestamp_ms = hv::now_ms();
                        ev.type = HELIOSVIEW_EVENT_MENU_SELECT;
                        ev.menu_item = id;
                        ev.userdata = menu->userdata;
                        hv::queue_push(ev);
                    }
                    return 0; /* consumed: this menu's WM_MENUCOMMAND */
                }
            }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

/* Enable MNS_NOTIFYBYPOS on a menu and store the owning C object in its
 * MENUINFO.dwMenuData (the routing callout reads it back from WM_MENUCOMMAND's
 * lParam HMENU). MIM_APPLYTOSUBMENUS propagates the STYLE to submenus; the
 * MENUDATA must NOT ride along for submenu items, which is why every submenu is
 * created through heliosview_menu_create (setting its OWN dwMenuData before it
 * is attached — construction order matters, see the header comment). */
void menu_enable_notify_by_pos(HMENU hmenu, heliosview_menu_t* menu)
{
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE | MIM_APPLYTOSUBMENUS | MIM_MENUDATA;
    mi.dwStyle = MNS_NOTIFYBYPOS;
    mi.dwMenuData = reinterpret_cast<uintptr_t>(menu);
    SetMenuInfo(hmenu, &mi);
}

/* Install the menu callout on `hwnd`. Idempotent: SetWindowSubclass upserts
 * the same (proc, key) pair, and there is exactly one such pair per window —
 * all menus on the window share this single callout. */
void menu_ensure_window_subclass(HWND hwnd)
{
    if (hwnd)
        SetWindowSubclass(hwnd, hv_menu_subclass_proc, kMenuSubclassKey, 0);
}

/* Shared tail of add_item / add_item_ex: allocate a menu-local command id and
 * append the item. Routing does not depend on ids (WM_MENUCOMMAND carries the
 * position + HMENU), so ids only need to be unique within this menu. */
int menu_append_item(heliosview_menu_t* menu, const char* text, UINT mf, uint32_t* out_id)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    if (!menu->hwnd) {
        /* the window may have been created after the menu: pick up the handle
         * and install the window's menu callout lazily */
        menu->hwnd = menu->window ? hv_window_hwnd(menu->window) : nullptr;
    }
    if (!menu->hwnd)
        return hv_fail(-1, "menu has no native window yet (create the owner window first)");
    menu_ensure_window_subclass(menu->hwnd);
    const UINT id = menu->next_item_id++;
    const std::wstring wtext = utf8_to_wide(text ? text : "");
    if (!AppendMenuW(menu->hmenu, mf, id, wtext.c_str())) {
        menu->next_item_id--; /* keep ids contiguous on failure */
        return hv_fail(-1, "AppendMenuW failed");
    }
    if (out_id)
        *out_id = id;
    return 0;
}

} // namespace

heliosview_menu_t* heliosview_menu_create(heliosview_window_t* window, void* userdata)
{
    auto* menu = hv::hv_alloc<heliosview_menu>();
    menu->window = window;
    menu->hwnd = window ? hv_window_hwnd(window) : nullptr;
    menu->userdata = userdata;
    menu->hmenu = CreatePopupMenu();
    if (!menu->hmenu) {
        hv_fail(-1, "CreatePopupMenu failed");
        hv::hv_dealloc(menu);
        return nullptr;
    }
    menu_enable_notify_by_pos(menu->hmenu,menu); /* WM_MENUCOMMAND routing (see the header comment) */
    menu_ensure_window_subclass(menu->hwnd); /* one callout per window, shared by all its menus */
    return menu;
}

void heliosview_menu_destroy(heliosview_menu_t* menu)
{
    if (!menu)
        return;
    /* no registries to clean and no subclass to remove (the window's single
     * menu callout is shared and torn down by the system with the window).
     * Lifetime rule: submenus (and their HMENUs / dwMenuData) go first, then
     * this menu's HMENU, then the object — dwMenuData is only ever read while
     * the HMENU is alive, so the object is never reached after the free. */
    while (!menu->submenus.empty()) {
        heliosview_menu_t* sub = menu->submenus.back();
        menu->submenus.pop_back();
        heliosview_menu_destroy(sub);
    }
    if (menu->hmenu)
        DestroyMenu(menu->hmenu);
    hv::hv_dealloc(menu);
}

int heliosview_menu_add_item(heliosview_menu_t* menu, const char* text, uint32_t* out_id)
{
    return menu_append_item(menu, text, MF_STRING, out_id);
}

int heliosview_menu_add_separator(heliosview_menu_t* menu)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    return AppendMenuW(menu->hmenu, MF_SEPARATOR, 0, nullptr) ? 0 : hv_fail(-1, "AppendMenuW (separator) failed");
}

int heliosview_menu_add_submenu(heliosview_menu_t* menu, const char* text,
                                heliosview_menu_t* submenu)
{
    if (!menu || !menu->hmenu || !submenu || !submenu->hmenu)
        return hv_fail(-1, "invalid menu or submenu (hmenu is NULL)");
    const std::wstring wtext = utf8_to_wide(text ? text : "");
    if (!AppendMenuW(menu->hmenu, MF_POPUP,
                     reinterpret_cast<UINT_PTR>(submenu->hmenu), wtext.c_str()))
        return hv_fail(-1, "AppendMenuW (submenu) failed");
    menu->submenus.push_back(submenu); /* parent owns the submenu's lifetime */
    return 0;
}

int heliosview_menu_add_item_ex(heliosview_menu_t* menu, const char* text,
                                uint32_t flags, uint32_t* out_id)
{
    if (!menu)
        return hv_fail(-1, "menu is NULL");
    UINT mf = MF_STRING;
    if (flags & HELIOSVIEW_MENU_ITEM_CHECKED)
        mf |= MF_CHECKED;
    if (flags & HELIOSVIEW_MENU_ITEM_DISABLED)
        mf |= MF_GRAYED; /* disabled + grayed (the standard "disabled" look) */
    if (flags & HELIOSVIEW_MENU_ITEM_RADIOCHECK)
        mf |= MFT_RADIOCHECK; /* bullet instead of a checkmark */
    if (flags & HELIOSVIEW_MENU_ITEM_DEFAULT)
        mf |= MF_DEFAULT; /* default item: bold, Enter / double-click activates */
    return menu_append_item(menu, text, mf, out_id);
}

int heliosview_menu_add_checkable_item(heliosview_menu_t* menu, const char* text,
                                       int checked, uint32_t* out_id)
{
    return heliosview_menu_add_item_ex(menu, text,
                                       checked ? HELIOSVIEW_MENU_ITEM_CHECKED : 0, out_id);
}

int heliosview_menu_set_item_checked(heliosview_menu_t* menu, uint32_t id, int checked)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    const UINT flags = MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED);
    return CheckMenuItem(menu->hmenu, id, flags) != -1 ? 0 : -1;
}

int heliosview_menu_is_item_checked(heliosview_menu_t* menu, uint32_t id, int* out_checked)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    const UINT state = GetMenuState(menu->hmenu, id, MF_BYCOMMAND);
    if (state == UINT_MAX)
        return hv_fail(-1, "menu item id not found");
    if (out_checked)
        *out_checked = (state & MF_CHECKED) ? 1 : 0;
    return 0;
}

int heliosview_menu_set_item_enabled(heliosview_menu_t* menu, uint32_t id, int enabled)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    const UINT flags = MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED);
    return EnableMenuItem(menu->hmenu, id, flags) != -1 ? 0 : -1;
}

int heliosview_menu_is_item_enabled(heliosview_menu_t* menu, uint32_t id, int* out_enabled)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    const UINT state = GetMenuState(menu->hmenu, id, MF_BYCOMMAND);
    if (state == UINT_MAX)
        return hv_fail(-1, "menu item id not found");
    if (out_enabled)
        *out_enabled = (state & (MF_DISABLED | MF_GRAYED)) ? 0 : 1;
    return 0;
}

int heliosview_menu_set_item_default(heliosview_menu_t* menu, uint32_t id, int is_default)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    /* (UINT)-1 + MF_BYCOMMAND clears the default item */
    return SetMenuDefaultItem(menu->hmenu, is_default ? id : static_cast<UINT>(-1),
                              MF_BYCOMMAND)
               ? 0
               : -1;
}

int heliosview_menu_is_item_default(heliosview_menu_t* menu, uint32_t id, int* out_default)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    const int def = static_cast<int>(GetMenuDefaultItem(menu->hmenu, FALSE, 0));
    if (out_default)
        *out_default = (def >= 0 && static_cast<UINT>(def) == id) ? 1 : 0;
    return 0;
}

int heliosview_menu_show(heliosview_menu_t* menu, heliosview_window_t* window)
{
    if (!menu || !menu->hmenu)
        return hv_fail(-1, "invalid menu (hmenu is NULL)");
    menu_ensure_window_subclass(menu->hwnd); /* the window may have been created after the menu */
    const HWND hwnd = hv_window_hwnd(window);
    if (!hwnd)
        return hv_fail(-1, "invalid owner window (native window not created)");
    if (hwnd != menu->hwnd)
        return hv_fail(-1, "the menu must be shown on its owner window");
    POINT pt{};
    GetCursorPos(&pt);
    /* give the menu a foreground window so it is dismissed when the user clicks
     * elsewhere; item selection is delivered via WM_MENUCOMMAND to this window
     * and resolved by the window's menu callout through the menu's dwMenuData */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu->hmenu,
                   TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, hwnd, nullptr);
    return 0;
}