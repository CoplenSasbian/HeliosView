#pragma once

/**
 * HeliosView.Core -- MenuBar: an application / window menu bar.
 *
 * A MenuBar represents the horizontal menu bar at the top of windows (Windows/Linux)
 * or the global application menu bar (macOS). It holds top-level menus (e.g. "File",
 * "Edit", "Help") which open vertical popup menus when clicked.
 *
 * Usage:
 *   helios::MenuBar bar;
 *   helios::Menu* fileMenu = bar.addMenu("File");
 *   fileMenu->addItem("Quit")->triggered.connect([&] { app.quit(); });
 *
 *   helios::Menu* editMenu = bar.addMenu("Edit");
 *   editMenu->addRole(helios::MenuRole::Copy);
 *
 *   bar.setAppMenu();  // install as global/window menu bar
 */

#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/Menu.h>
#include <HeliosView/heliosview.h>

#include <memory>
#include <vector>

namespace helios {

class MenuBar {
public:
    // Create an empty window menu bar for the application (Windows/Linux top-level
    // window menu bar, macOS global application menu bar).
    MenuBar()
        : m_bar(heliosview_menu_create_bar(this))
    {
    }

    ~MenuBar()
    {
        if (m_bar)
            heliosview_menu_destroy(m_bar);
    }

    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    MenuBar(MenuBar&& other) noexcept
        : m_bar(other.m_bar)
        , m_menus(std::move(other.m_menus))
    {
        other.m_bar = nullptr;
    }

    MenuBar& operator=(MenuBar&& other) noexcept
    {
        if (this != &other) {
            if (m_bar)
                heliosview_menu_destroy(m_bar);
            m_bar = other.m_bar;
            m_menus = std::move(other.m_menus);
            other.m_bar = nullptr;
        }
        return *this;
    }

    // True when the menu bar was created successfully
    bool valid() const { return m_bar != nullptr; }

    // The underlying C handle
    heliosview_menu_t* handle() const { return m_bar; }

    // Add a top-level menu to the bar (e.g. "File", "Edit"). The Menu is owned by
    // the MenuBar and returned for populating with items and submenus.
    Menu* addMenu(const char* text)
    {
        auto menu = std::make_unique<Menu>();
        if (!menu->valid() ||
            heliosview_menu_add_submenu(m_bar, text, menu->handle()) != 0)
            throwLastError("menubar addMenu");
        auto* raw = menu.get();
        menu->m_owned = false; // The underlying C menubar owns the submenu handle
        m_menus.push_back(std::move(menu));
        return raw;
    }

    // Alias for addMenu (matches Menu::addSubmenu for backwards compatibility)
    Menu* addSubmenu(const char* text)
    {
        return addMenu(text);
    }

    // Install this menu bar as the application menu bar: macOS puts it in the
    // one global bar (its first submenu becomes the App menu); Windows/Linux
    // show it as the menu bar of every HeliosView window, including future ones.
    void setAppMenu()
    {
        if (heliosview_menu_set_app_menu(m_bar) != 0)
            throwLastError("menubar setAppMenu");
    }

    // Remove the current application menu bar (windows keep their own layout).
    static void clearAppMenu()
    {
        heliosview_menu_set_app_menu(nullptr);
    }

private:
    heliosview_menu_t* m_bar = nullptr;
    std::vector<std::unique_ptr<Menu>> m_menus;
};

inline MenuBar Menu::createBar()
{
    return MenuBar();
}

} // namespace helios
