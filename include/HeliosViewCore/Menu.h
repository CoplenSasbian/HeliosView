#pragma once

/**
 * HeliosView.Core -- Menu: a vertical display structure for actions.
 *
 * A Menu is a standalone popup/context menu (like Tray): it is created and
 * filled with no window at all, and only show() needs an owner. Its entries
 * are actions, submenus and separators — the menu owns the *layout*, the Action
 * owns the command (label, state, triggered).
 *
 * Scenarios:
 *   - Context / popup menu: shown at the cursor via menu.show(window).
 *   - Submenu: cascaded inside another menu via parentMenu->addSubmenu("More...").
 *   - MenuBar dropdown menu: created via bar.addMenu("File").
 *   - Tray icon menu: attached via tray.setMenu(menu).
 *
 * For window or application menu bars, use helios::MenuBar.
 *
 * Usage (from the README):
 *   helios::Menu menu;                          // no window needed
 *   helios::Menu::Item* show = menu.addItem("Show / Restore");
 *   helios::Menu::Item* quit = menu.addItem("Quit");
 *   helios::Menu::Item* top = menu.addCheckItem("Toggle Topmost"); // checkable
 *   helios::Menu::Item* disabled = menu.addItem("Unavailable");
 *   disabled->setEnabled(false);               // grayed out, not selectable
 *   menu.addSeparator();
 *   menu.setDefaultAction(*show);              // bold default item (Enter activates)
 *   show->triggered.connect([&] { window.showNormal(); });
 *   quit->triggered.connect([&] { app.quit(); });
 *   ...
 *   menu.show(window.nativeHandle());   // popup at the cursor; NULL = standalone
 *
 * Items are owned by the menu; the returned pointers stay valid until the menu
 * is destroyed. Submenus are owned by their parent.
 */

#include <HeliosViewCore/Action.h>
#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/Types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace helios {

class MenuBar;

class Menu {
public:
    friend class MenuBar;

    // A menu item is an Action (kept as an alias for the classic spelling).
    using Item = Action;

    // Create an empty standalone vertical/popup menu (no window needed — like Tray).
    // The menu is shown with show(window), where the owner window is optional.
    // Not copyable/movable.
    Menu()
        : m_menu(heliosview_menu_create(this))
    {
    }

    // Deprecated: use helios::MenuBar directly for menu bars.
    [[deprecated("Use helios::MenuBar instead")]]
    static MenuBar createBar();

    // Deprecated: Menu is always a vertical/popup menu.
    [[deprecated("Menu is always a popup/vertical menu; use helios::MenuBar for menu bars")]]
    bool isBar() const { return false; }

    /**
     * @deprecated A menu no longer belongs to a window; the argument is ignored.
     * Use Menu() and pass the owner to show() instead.
     */
    [[deprecated("Menu no longer requires a window; use Menu() and show(window) instead")]]
    explicit Menu(heliosview_window_t* /*window*/)
        : Menu()
    {
    }

    ~Menu()
    {
        if (m_owned)
            heliosview_menu_destroy(m_menu); /* drops this object's reference; a parent
                                              * menu, the app bar or a tray may keep it alive */
        /* m_owned_actions are destroyed here (after the C menu): each releases
         * its own reference, so the last one frees the action. */
    }

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    // True when the menu was created successfully
    bool valid() const { return m_menu != nullptr; }

    // The underlying C handle (e.g. to attach the menu to a Tray)
    heliosview_menu_t* handle() const { return m_menu; }

    // Add a text item (UTF-8); returns its Action, owned by this menu.
    // Throws std::runtime_error on failure with the reason recorded by the C layer.
    Action* addItem(const char* text)
    {
        auto owned = std::make_unique<Action>(text);
        if (!owned->valid() || heliosview_menu_add_action(m_menu, owned->handle()) != 0)
            throwLastError("menu addItem");
        Action* raw = owned.get();
        m_owned_actions.push_back(std::move(owned));
        return raw;
    }

    // Add a checkable text item (UTF-8): a checkmark shows next to its text
    // while checked (starts checked when `checked`). Update it from the item's
    // triggered signal, e.g. `item->setChecked(!item->checked())`.
    Action* addCheckItem(const char* text, bool checked = false)
    {
        Action* item = addItem(text);
        item->setCheckable(true);
        item->setChecked(checked);
        return item;
    }

    // Add a standard role item (MenuRole::Quit, ::Copy, ::Cut, ::Minimize, ...):
    // the library supplies the platform's label and shortcut and performs the
    // action where the application cannot. `text` overrides the default label.
    // Returns the action (owned by this menu).
    Action* addRole(MenuRole role, const char* text = nullptr)
    {
        auto owned = std::make_unique<Action>(role, text);
        if (!owned->valid() || heliosview_menu_add_action(m_menu, owned->handle()) != 0)
            throwLastError("menu addRole");
        Action* raw = owned.get();
        m_owned_actions.push_back(std::move(owned));
        return raw;
    }

    // Add an action created by the caller (shared): the same Action may be added
    // to several menus, fires one triggered signal and has one state. The action
    // must outlive the menus displaying it (each menu holds a reference).
    // Throws std::runtime_error on failure.
    Action* addAction(Action& action)
    {
        if (heliosview_menu_add_action(m_menu, action.handle()) != 0)
            throwLastError("menu addAction");
        return &action;
    }
    Action* addAction(Action* action) { return action ? addAction(*action) : nullptr; }

    // Add a visual separator line
    void addSeparator() { heliosview_menu_add_separator(m_menu); }

    // Add a submenu under `text`; the submenu is owned by this menu. Returns
    // the submenu (for adding items to it). Throws std::runtime_error on
    // failure. The submenu's C-layer handle is owned by this menu's C layer
    // (destroying this menu destroys the submenu's handle), so this wrapper
    // marks itself m_owned = false to avoid a double destroy — but keeps the
    // handle so addItem()/addSeparator() on the submenu keep working.
    Menu* addSubmenu(const char* text)
    {
        auto submenu = std::make_unique<Menu>();
        if (!submenu->valid() ||
            heliosview_menu_add_submenu(m_menu, text, submenu->m_menu) != 0)
            throwLastError("menu addSubmenu");
        auto* raw = submenu.get();
        submenu->m_owned = false; /* parent's C layer owns the handle (freed with parent) */
        m_submenus.push_back(std::move(submenu));
        return raw;
    }

    // Make `action` the menu's default item (bold; Enter / double-click
    // activates it; one per menu). The action must already be in this menu.
    void setDefaultAction(Action& action)
    {
        if (heliosview_menu_set_default_action(m_menu, action.handle()) != 0)
            throwLastError("menu setDefaultAction");
    }

    // Show the popup at the current cursor position, owned by `window`
    // (dispatches the MenuSelect event). `window` may be nullptr: the library
    // then uses a hidden owner window.
    void show(heliosview_window_t* window) { heliosview_menu_show(m_menu, window); }

    // Deprecated: use helios::MenuBar::setAppMenu() instead.
    [[deprecated("Use helios::MenuBar::setAppMenu() instead")]]
    void setAppMenu()
    {
        if (heliosview_menu_set_app_menu(m_menu) != 0)
            throwLastError("menu setAppMenu");
    }

    // Deprecated: use helios::MenuBar::clearAppMenu() instead.
    [[deprecated("Use helios::MenuBar::clearAppMenu() instead")]]
    static void clearAppMenu() { heliosview_menu_set_app_menu(nullptr); }

    // Mark this submenu as a standard menu (MenuKind::App / Services / Window /
    // Help): macOS wires the corresponding system menu with it; other platforms
    // treat it as a hint.
    void setKind(MenuKind kind)
    {
        if (heliosview_menu_set_kind(m_menu, static_cast<heliosview_menu_kind_t>(kind)) != 0)
            throwLastError("menu setKind");
    }
    MenuKind kind() const
    {
        return static_cast<MenuKind>(heliosview_menu_kind(m_menu));
    }

    // Populate the menu lazily: the callback runs every time this menu (or this
    // submenu) is about to open, before item state is refreshed, and may add
    // actions/submenus (recent files, state-dependent items). Pass nullptr to
    // clear it.
    using OpenCallback = std::function<void(Menu&)>;
    void setOpenCallback(OpenCallback callback)
    {
        m_openCallback = std::move(callback);
        heliosview_menu_set_open_callback(m_menu, m_openCallback ? &Menu::openTrampoline : nullptr,
                                          m_openCallback ? this : nullptr);
    }

private:
    static void openTrampoline(heliosview_menu_t* /*menu*/, void* userdata)
    {
        auto* self = static_cast<Menu*>(userdata);
        if (self && self->m_openCallback)
            self->m_openCallback(*self);
    }

    heliosview_menu_t* m_menu = nullptr;
    bool m_owned = true; /* false = submenu (the parent menu's C layer owns the handle) */
    OpenCallback m_openCallback;                          /* lazy population */
    std::vector<std::unique_ptr<Action>> m_owned_actions; /* actions created by addItem */
    std::vector<std::unique_ptr<Menu>> m_submenus;        /* owned submenus */
};

} // namespace helios

#include <HeliosViewCore/MenuBar.h>

