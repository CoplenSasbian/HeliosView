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
 *   helios::Menu::Item* show = menu.addItem(L"Show / Restore");
 *   helios::Menu::Item* quit = menu.addItem(L"Quit");
 *   menu.addSeparator();
 *   show->triggered.connect([&] { window.showNormal(); });
 *   quit->triggered.connect([&] { app.quit(); });
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
        heliosview_menu_destroy(m_menu);
    }

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    // True when the menu was created successfully
    bool valid() const { return m_menu != nullptr; }

    // Add a text item; returns its Item (owned by this menu). nullptr on failure.
    Item* addItem(const wchar_t* text)
    {
        uint32_t id = 0;
        if (heliosview_menu_add_item(m_menu, text, &id) != 0)
            return nullptr;
        auto& item = m_items[id] = std::make_unique<Item>();
        return item.get();
    }

    // Add a visual separator line
    void addSeparator() { heliosview_menu_add_separator(m_menu); }

    // Add a submenu under `text`; the submenu is owned by this menu.
    // Returns the submenu (for adding items to it), or nullptr on failure.
    // The submenu stays alive (and its sink keeps routing MENU_SELECT events)
    // while this menu lives; only the C-layer handle is handed to the parent
    // (destroying the parent destroys the submenu's handle).
    Menu* addSubmenu(const wchar_t* text)
    {
        auto submenu = std::make_unique<Menu>(m_window);
        if (!submenu->valid() ||
            heliosview_menu_add_submenu(m_menu, text, submenu->m_menu) != 0)
            return nullptr;
        auto* raw = submenu.get();
        submenu->m_menu = nullptr; /* the C layer owns the handle now (freed with the parent) */
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

    heliosview_window_t* m_window = nullptr;
    heliosview_menu_t* m_menu = nullptr;
    App::SinkId m_sink = 0;
    std::flat_map<uint32_t, std::unique_ptr<Item>> m_items; /* item id -> Item */
    std::vector<std::unique_ptr<Menu>> m_submenus;          /* owned submenus */
};

} // namespace helios
