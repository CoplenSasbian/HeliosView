#pragma once

/**
 * HeliosView.Core -- Menu: a popup / context menu.
 *
 * Built on the C layer's per-window routing registry: each item is registered
 * on the owner window, so choosing it posts a HELIOSVIEW_EVENT_MENU_SELECT
 * event (menu_item = the item id, userdata = the Menu object). Like Tray, a
 * Menu works without a C++ Window wrapper — only the raw handle is needed.
 *
 * Usage (from the README):
 *   helios::Menu menu(window.nativeHandle());
 *   helios::Menu::Item* show = menu.addItem("Show / Restore");
 *   helios::Menu::Item* quit = menu.addItem("Quit");
 *   helios::Menu::Item* top = menu.addCheckItem("Toggle Topmost"); // checkable
 *   helios::Menu::Item* disabled = menu.addItem("Unavailable");
 *   disabled->setEnabled(false);               // grayed out, not selectable
 *   menu.addSeparator();
 *   show->setDefault(true);                    // bold default item
 *   show->triggered.connect([&] { window.showNormal(); });
 *   quit->triggered.connect([&] { app.quit(); });
 *   top->triggered.connect([&] {
 *       top->setChecked(!top->checked()); // toggle the on-screen checkmark
 *       ...
 *   });
 *   ...
 *   menu.show(window.nativeHandle());   // popup at the current cursor position
 *
 * Items are owned by the menu; the returned Item pointers stay valid until
 * the menu is destroyed. Submenus are owned by their parent. Destroy the
 * menu before its window.
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/Types.h>

#include <cstdint>
#include <flat_map>
#include <memory>
#include <vector>

namespace helios {

class Menu {
public:
    // One menu item: triggered fires (UI thread) when the user chooses it.
    struct Item {
        Signal<> triggered;

        // Checkable items (see addCheckItem): read / update the on-screen
        // checkmark. The underlying Win32 call works for any item, so these
        // can be used as toggles even without addCheckItem.
        bool checked() const { return m_menu && m_menu->itemChecked(m_id); }
        void setChecked(bool on)
        {
            if (m_menu)
                m_menu->setItemChecked(m_id, on);
        }

        // Enabled state: disabled items are grayed out and cannot be chosen.
        bool enabled() const { return m_menu && m_menu->itemEnabled(m_id); }
        void setEnabled(bool on)
        {
            if (m_menu)
                m_menu->setItemEnabled(m_id, on);
        }

        // Default item: shown bold and activated by Enter / double-click.
        // A menu has at most one default item (setting another moves it).
        bool isDefault() const { return m_menu && m_menu->itemDefault(m_id); }
        void setDefault(bool on)
        {
            if (m_menu)
                m_menu->setItemDefault(m_id, on);
        }

    private:
        friend class Menu;
        Menu* m_menu = nullptr; /* owning menu (for the C-layer checkmark calls) */
        uint32_t m_id = 0;      /* this item's routing id */
    };

    // Create an empty popup menu attached to `window`. Not copyable/movable.
    Menu(heliosview_window_t* window)
        : m_window(window)
        , m_menu(heliosview_menu_create(window, this))
    {
        if (m_menu)
            m_sink = App::instance()->addSink([this](const Event& ev) { return handleEvent(ev); });
    }

    ~Menu()
    {
        if (m_sink != 0)
            App::instance()->removeSink(m_sink);
        if (m_owned)
            heliosview_menu_destroy(m_menu);
    }

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    // True when the menu was created successfully
    bool valid() const { return m_menu != nullptr; }

    // Add a text item (UTF-8); returns its Item (owned by this menu). nullptr on failure.
    Item* addItem(const char* text)
    {
        uint32_t id = 0;
        if (heliosview_menu_add_item(m_menu, text, &id) != 0)
            return nullptr;
        auto& item = m_items[id] = std::make_unique<Item>();
        item->m_id = id;
        item->m_menu = this;
        return item.get();
    }

    // Add a checkable text item (UTF-8): a checkmark shows next to its text
    // while checked (starts checked when `checked`). Update it from the item's
    // triggered signal, e.g. `item->setChecked(!item->checked())`, to make a
    // toggle. Returns its Item (owned by this menu); nullptr on failure.
    Item* addCheckItem(const char* text, bool checked = false)
    {
        uint32_t id = 0;
        if (heliosview_menu_add_checkable_item(m_menu, text, checked ? 1 : 0, &id) != 0)
            return nullptr;
        auto& item = m_items[id] = std::make_unique<Item>();
        item->m_id = id;
        item->m_menu = this;
        return item.get();
    }

    // Add a visual separator line
    void addSeparator() { heliosview_menu_add_separator(m_menu); }

    // Add a submenu under `text`; the submenu is owned by this menu.
    // Returns the submenu (for adding items to it), or nullptr on failure.
    // The submenu's C-layer handle is owned by this menu's C layer (destroying
    // this menu destroys the submenu's handle), so this wrapper marks itself
    // m_owned = false to avoid a double destroy — but keeps the handle so
    // addItem()/addSeparator() on the submenu keep working.
    Menu* addSubmenu(const char* text)
    {
        auto submenu = std::make_unique<Menu>(m_window);
        if (!submenu->valid() ||
            heliosview_menu_add_submenu(m_menu, text, submenu->m_menu) != 0)
            return nullptr;
        auto* raw = submenu.get();
        submenu->m_owned = false; /* parent's C layer owns the handle (freed with parent) */
        m_submenus.push_back(std::move(submenu));
        return raw;
    }

    // Show the popup at the current cursor position, owned by `window`
    // (its HWND receives the WM_COMMAND that yields the MENU_SELECT event).
    void show(heliosview_window_t* window) { heliosview_menu_show(m_menu, window); }

private:
    // Route MENU_SELECT events for this menu to the matching item's signal
    bool handleEvent(const Event& ev)
    {
        if (ev.userdata != this || ev.type != EventType::MenuSelect)
            return false;
        if (auto it = m_items.find(ev.menuItem); it != m_items.end())
            it->second->triggered();
        return true; // consumed even for unknown ids (they belong to this menu)
    }

    // Checkmark state of the item with the given id (Item::checked / setChecked)
    bool itemChecked(uint32_t id) const
    {
        int on = 0;
        return m_menu && heliosview_menu_is_item_checked(m_menu, id, &on) == 0 && on != 0;
    }
    void setItemChecked(uint32_t id, bool on)
    {
        if (m_menu)
            heliosview_menu_set_item_checked(m_menu, id, on ? 1 : 0);
    }

    // Enabled state of the item (Item::enabled / setEnabled)
    bool itemEnabled(uint32_t id) const
    {
        int on = 0;
        return m_menu && heliosview_menu_is_item_enabled(m_menu, id, &on) == 0 && on != 0;
    }
    void setItemEnabled(uint32_t id, bool on)
    {
        if (m_menu)
            heliosview_menu_set_item_enabled(m_menu, id, on ? 1 : 0);
    }

    // Default-item state (Item::isDefault / setDefault)
    bool itemDefault(uint32_t id) const
    {
        int on = 0;
        return m_menu && heliosview_menu_is_item_default(m_menu, id, &on) == 0 && on != 0;
    }
    void setItemDefault(uint32_t id, bool on)
    {
        if (m_menu)
            heliosview_menu_set_item_default(m_menu, id, on ? 1 : 0);
    }

    heliosview_window_t* m_window = nullptr;
    heliosview_menu_t* m_menu = nullptr;
    bool m_owned = true; /* false = submenu (the parent menu's C layer owns the handle) */
    App::SinkId m_sink = 0;
    std::flat_map<uint32_t, std::unique_ptr<Item>> m_items; /* item id -> Item */
    std::vector<std::unique_ptr<Menu>> m_submenus;          /* owned submenus */
};

} // namespace helios
