#pragma once

/**
 * HeliosView.Core — event types and event structures.
 *
 * These mirror one-to-one the enums/structs of HeliosView.dll's C interface
 * (heliosview.h), providing type-safe C++ enums and event objects
 * (with C ←→ C++ conversion).
 */

#include <HeliosView/heliosview.h>

#include <cstdint>

namespace helios {

/* ---------- types mapping 1:1 to the C enums ---------- */

// Event category (mirrors heliosview_event_type_t)
enum class EventType : int32_t {
    Quit = HELIOSVIEW_EVENT_QUIT,
    WindowClose = HELIOSVIEW_EVENT_WINDOW_CLOSE,
    WindowResize = HELIOSVIEW_EVENT_WINDOW_RESIZE,
    KeyDown = HELIOSVIEW_EVENT_KEY_DOWN,
    KeyUp = HELIOSVIEW_EVENT_KEY_UP,
    MouseMove = HELIOSVIEW_EVENT_MOUSE_MOVE,
    MouseButtonDown = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN,
    MouseButtonUp = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP,
    TrayLeftClick = HELIOSVIEW_EVENT_TRAY_LEFT_CLICK,
    TrayLeftDoubleClick = HELIOSVIEW_EVENT_TRAY_LEFT_DOUBLE_CLICK,
    TrayRightClick = HELIOSVIEW_EVENT_TRAY_RIGHT_CLICK,
    TrayMiddleClick = HELIOSVIEW_EVENT_TRAY_MIDDLE_CLICK,
    MenuSelect = HELIOSVIEW_EVENT_MENU_SELECT,
};

// True for the tray-icon mouse event types (see Tray.h)
inline bool isTrayEvent(EventType type)
{
    switch (type) {
    case EventType::TrayLeftClick:
    case EventType::TrayLeftDoubleClick:
    case EventType::TrayRightClick:
    case EventType::TrayMiddleClick:
        return true;
    default:
        return false;
    }
}

// Platform-independent keycode (native keycodes are mapped by the C layer)
enum class KeyCode : int32_t {
    Unknown = HELIOSVIEW_KEY_UNKNOWN,
    Escape = HELIOSVIEW_KEY_ESCAPE,
    Return = HELIOSVIEW_KEY_RETURN,
    Space = HELIOSVIEW_KEY_SPACE,
    Left = HELIOSVIEW_KEY_LEFT,
    Right = HELIOSVIEW_KEY_RIGHT,
    Up = HELIOSVIEW_KEY_UP,
    Down = HELIOSVIEW_KEY_DOWN,
    Key0 = HELIOSVIEW_KEY_0,
    Key1 = HELIOSVIEW_KEY_1,
    Key2 = HELIOSVIEW_KEY_2,
    Key3 = HELIOSVIEW_KEY_3,
    Key4 = HELIOSVIEW_KEY_4,
    Key5 = HELIOSVIEW_KEY_5,
    Key6 = HELIOSVIEW_KEY_6,
    Key7 = HELIOSVIEW_KEY_7,
    Key8 = HELIOSVIEW_KEY_8,
    Key9 = HELIOSVIEW_KEY_9,
    A = HELIOSVIEW_KEY_A,
    B = HELIOSVIEW_KEY_B,
    C = HELIOSVIEW_KEY_C,
    D = HELIOSVIEW_KEY_D,
    E = HELIOSVIEW_KEY_E,
    F = HELIOSVIEW_KEY_F,
    G = HELIOSVIEW_KEY_G,
    H = HELIOSVIEW_KEY_H,
    I = HELIOSVIEW_KEY_I,
    J = HELIOSVIEW_KEY_J,
    K = HELIOSVIEW_KEY_K,
    L = HELIOSVIEW_KEY_L,
    M = HELIOSVIEW_KEY_M,
    N = HELIOSVIEW_KEY_N,
    O = HELIOSVIEW_KEY_O,
    P = HELIOSVIEW_KEY_P,
    Q = HELIOSVIEW_KEY_Q,
    R = HELIOSVIEW_KEY_R,
    S = HELIOSVIEW_KEY_S,
    T = HELIOSVIEW_KEY_T,
    U = HELIOSVIEW_KEY_U,
    V = HELIOSVIEW_KEY_V,
    W = HELIOSVIEW_KEY_W,
    X = HELIOSVIEW_KEY_X,
    Y = HELIOSVIEW_KEY_Y,
    Z = HELIOSVIEW_KEY_Z,
    F1 = HELIOSVIEW_KEY_F1,
    F2 = HELIOSVIEW_KEY_F2,
    F3 = HELIOSVIEW_KEY_F3,
    F4 = HELIOSVIEW_KEY_F4,
    F5 = HELIOSVIEW_KEY_F5,
    F6 = HELIOSVIEW_KEY_F6,
    F7 = HELIOSVIEW_KEY_F7,
    F8 = HELIOSVIEW_KEY_F8,
    F9 = HELIOSVIEW_KEY_F9,
    F10 = HELIOSVIEW_KEY_F10,
    F11 = HELIOSVIEW_KEY_F11,
    F12 = HELIOSVIEW_KEY_F12,
};

// Mouse button identifier (mirrors heliosview_mouse_button_t)
enum class MouseButton : int32_t {
    Left = HELIOSVIEW_MOUSE_LEFT,
    Right = HELIOSVIEW_MOUSE_RIGHT,
    Middle = HELIOSVIEW_MOUSE_MIDDLE,
};

/* predefined window styles */
enum class WindowStyle : int32_t {
    Normal = HELIOSVIEW_WINDOW_NORMAL,   /* normal window: title bar + border + system menu */
    Borderless = HELIOSVIEW_WINDOW_BORDERLESS, /* borderless (fully custom drawn) */
    Frameless = HELIOSVIEW_WINDOW_FRAMELESS,   /* bordered, no title bar (custom title-bar style) */
};

/* window show state */
// Window show state (mirrors heliosview_show_state_t)
enum class ShowState : int32_t {
    Normal = HELIOSVIEW_SHOW_NORMAL,     /* normal (restores minimized/maximized) */
    Minimized = HELIOSVIEW_SHOW_MINIMIZED,
    Maximized = HELIOSVIEW_SHOW_MAXIMIZED,
};

/* ---------- events ---------- */

// A queued event, mirroring heliosview_event_t with type-safe C++ enums.
// Fields are meaningful only for the event types listed next to them.
struct Event {
    EventType type = EventType::Quit;         /* event type */
    int32_t windowId = 0;            /* window that produced the event (0 = none) */
    int64_t timestampMs = 0;         /* milliseconds since library initialization */
    int32_t x = 0;                   /* mouse X (MouseMove / MouseButton*) */
    int32_t y = 0;                   /* mouse Y */
    int32_t width = 0;               /* new width (WindowResize) */
    int32_t height = 0;              /* new height (WindowResize) */
    KeyCode key = KeyCode::Unknown;  /* key code (KeyDown / KeyUp) */
    MouseButton mouseButton = MouseButton::Left; /* button (MouseButton*) */
    uint32_t menuItem = 0;           /* menu item id (MenuSelect) */
    void* userdata = nullptr;        /* owning Tray/Menu object (Tray* / MenuSelect) */

    // Convert a C-layer event (heliosview_event_t) to its C++ Event form
    static Event fromC(const heliosview_event_t& c)
    {
        Event e;
        e.type = static_cast<EventType>(c.type);
        e.windowId = c.window_id;
        e.timestampMs = c.timestamp_ms;
        e.x = c.x;
        e.y = c.y;
        e.width = c.width;
        e.height = c.height;
        e.key = static_cast<KeyCode>(c.key);
        e.mouseButton = static_cast<MouseButton>(c.mouse_button);
        e.menuItem = c.menu_item;
        e.userdata = c.userdata;
        return e;
    }

    // Convert this Event to the C-layer form (heliosview_event_t)
    heliosview_event_t toC() const
    {
        heliosview_event_t c{};
        c.type = static_cast<heliosview_event_type_t>(type);
        c.window_id = windowId;
        c.timestamp_ms = timestampMs;
        c.x = x;
        c.y = y;
        c.width = width;
        c.height = height;
        c.key = static_cast<heliosview_keycode_t>(key);
        c.mouse_button = static_cast<heliosview_mouse_button_t>(mouseButton);
        c.menu_item = menuItem;
        c.userdata = userdata;
        return c;
    }
};

} // namespace helios
