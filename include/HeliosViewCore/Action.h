#pragma once

/**
 * HeliosView.Core -- Action: a shareable menu command.
 *
 * An Action is a command with identity and state (label, enabled, checkable /
 * checked); a Menu only *displays* actions. The same Action can be added to any
 * number of menus (an Edit menu, a context menu, ...): its triggered signal
 * fires once, its state is one object, its label is one string. Menus refresh
 * item state from the action whenever they pop up, so changing an action updates
 * every menu showing it.
 *
 * Ownership: an Action owns its C handle; a Menu that displays it holds its own
 * reference (see heliosview_action_destroy), so the action may be destroyed in
 * any order relative to the menus — the object lives until both are gone.
 *
 * Usage:
 *   helios::Action copy("Copy");                 // app-owned, shareable
 *   copy.triggered.connect(&onCopy, this);
 *   editMenu->addAction(copy);
 *   contextMenu->addAction(copy);                // same command, two menus
 *   copy.setEnabled(false);                      // both menus gray out
 *
 *   helios::Menu menu;
 *   helios::Menu::Item* item = menu.addItem("Quit");  // menu-owned action
 *   item->triggered.connect([&] { app.quit(); });
 */

#include <HeliosViewCore/App.h>
#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/Signal.h>
#include <HeliosViewCore/Types.h>

#include <cstdint>

namespace helios {

class Action {
public:
    // Fires when the action is chosen in any menu showing it (UI thread).
    Signal<> triggered;

    // Create an action with the given label (UTF-8; nullptr = empty) and an
    // optional portable shortcut ("Primary+S", "Ctrl+Shift+Z", "F11").
    explicit Action(const char* text = nullptr, const char* shortcut = nullptr)
        : m_action(heliosview_action_create(text, this))
    {
        registerSelf();
        if (shortcut)
            heliosview_action_set_shortcut(m_action, shortcut);
    }

    // Create a standard role action (MenuRole::Quit, ::Copy, ...): the library
    // supplies the platform's label and shortcut and performs the action where
    // the application cannot. `text` overrides the default label.
    explicit Action(MenuRole role, const char* text = nullptr)
        : m_action(heliosview_action_create_role(static_cast<heliosview_menu_role_t>(role),
                                                 text, this))
    {
        registerSelf();
    }

    ~Action()
    {
        if (m_sink != 0)
            App::instance()->removeSink(m_sink);
        heliosview_action_destroy(m_action);
    }

    Action(const Action&) = delete;
    Action& operator=(const Action&) = delete;

    // True when the action was created successfully
    bool valid() const { return m_action != nullptr; }

    // The underlying C handle (for the C API / Menu::addAction)
    heliosview_action_t* handle() const { return m_action; }

    // Process-unique id: what a MENU_SELECT event carries in menuItem
    uint32_t id() const { return heliosview_action_id(m_action); }

    // Label (UTF-8, library-owned; valid until the label is changed)
    const char* text() const { return heliosview_action_text(m_action); }
    void setText(const char* text) { heliosview_action_set_text(m_action, text); }

    // Portable shortcut string ("Primary+S"), or nullptr when unset. Returns
    // false from setShortcut when the string cannot be parsed.
    const char* shortcut() const { return heliosview_action_shortcut(m_action); }
    bool setShortcut(const char* shortcut)
    {
        return heliosview_action_set_shortcut(m_action, shortcut) == 0;
    }

    // The standard role this action was created with (MenuRole::None = plain)
    MenuRole role() const
    {
        return static_cast<MenuRole>(heliosview_action_role(m_action));
    }

    // Enabled state: a disabled action is grayed out and not selectable in every
    // menu that shows it.
    bool enabled() const { return heliosview_action_is_enabled(m_action) != 0; }
    void setEnabled(bool on) { heliosview_action_set_enabled(m_action, on ? 1 : 0); }

    // Checkable actions draw a checkmark (or a radio bullet) while checked.
    bool checkable() const { return heliosview_action_is_checkable(m_action) != 0; }
    void setCheckable(bool on) { heliosview_action_set_checkable(m_action, on ? 1 : 0); }
    bool checked() const { return heliosview_action_is_checked(m_action) != 0; }
    void setChecked(bool on) { heliosview_action_set_checked(m_action, on ? 1 : 0); }
    bool radioStyle() const { return heliosview_action_is_radio_style(m_action) != 0; }
    void setRadioStyle(bool on) { heliosview_action_set_radio_style(m_action, on ? 1 : 0); }

private:
    void registerSelf()
    {
        if (m_action)
            m_sink = App::instance()->addSink([this](const Event& ev) { return handleEvent(ev); });
    }

    // Route MENU_SELECT events for this action to its triggered signal
    bool handleEvent(const Event& ev)
    {
        if (ev.type != EventType::MenuSelect || ev.userdata != this)
            return false;
        triggered();
        // Quit is the application's decision, so the library only posts the
        // event. When nothing is connected, provide the obvious default.
        if (role() == MenuRole::Quit && triggered.empty())
            App::instance()->quit();
        return true;
    }

    heliosview_action_t* m_action = nullptr;
    App::SinkId m_sink = 0;
};

} // namespace helios
