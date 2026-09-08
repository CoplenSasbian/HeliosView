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
#include <cstring>

namespace helios {

/* ---------- types mapping 1:1 to the C enums ---------- */

// Event category (mirrors heliosview_event_type_t)
enum class EventType : int32_t {
    Quit = HELIOSVIEW_EVENT_QUIT,
    WindowClose = HELIOSVIEW_EVENT_WINDOW_CLOSE,
    WindowResize = HELIOSVIEW_EVENT_WINDOW_RESIZE,
    WindowMoved = HELIOSVIEW_EVENT_WINDOW_MOVED,
    WindowMoving = HELIOSVIEW_EVENT_WINDOW_MOVING,
    WindowSizing = HELIOSVIEW_EVENT_WINDOW_SIZING,
    WindowFocus = HELIOSVIEW_EVENT_WINDOW_FOCUS,
    WindowBlur = HELIOSVIEW_EVENT_WINDOW_BLUR,
    WindowEnabled = HELIOSVIEW_EVENT_WINDOW_ENABLED,
    WindowDisabled = HELIOSVIEW_EVENT_WINDOW_DISABLED,
    WindowFirstShown = HELIOSVIEW_EVENT_WINDOW_FIRST_SHOWN,  // native window first displayed (fires once; Window::event maps it to the firstShown signal)
    WindowMinimized = HELIOSVIEW_EVENT_WINDOW_MINIMIZED,     // window minimized (no resize event is emitted for it)
    WindowMaximized = HELIOSVIEW_EVENT_WINDOW_MAXIMIZED,     // window maximized (a resize event follows)
    WindowRestored = HELIOSVIEW_EVENT_WINDOW_RESTORED,       // window restored to normal from minimized/maximized (a resize event follows)
    WindowShown = HELIOSVIEW_EVENT_WINDOW_SHOWN,             // window became visible (show/hide only)
    WindowHidden = HELIOSVIEW_EVENT_WINDOW_HIDDEN,           // window became hidden (minimize is NOT a hide)
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
    TextInput = HELIOSVIEW_EVENT_TEXT_INPUT,                 // text entered (keyboard / IME commit); see Event::text
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
    // editing / navigation
    Tab = HELIOSVIEW_KEY_TAB,
    Backspace = HELIOSVIEW_KEY_BACKSPACE,
    Delete = HELIOSVIEW_KEY_DELETE,
    Insert = HELIOSVIEW_KEY_INSERT,
    Home = HELIOSVIEW_KEY_HOME,
    End = HELIOSVIEW_KEY_END,
    PageUp = HELIOSVIEW_KEY_PAGE_UP,
    PageDown = HELIOSVIEW_KEY_PAGE_DOWN,
    // modifier keys
    LeftShift = HELIOSVIEW_KEY_LEFT_SHIFT,
    RightShift = HELIOSVIEW_KEY_RIGHT_SHIFT,
    LeftCtrl = HELIOSVIEW_KEY_LEFT_CTRL,
    RightCtrl = HELIOSVIEW_KEY_RIGHT_CTRL,
    LeftAlt = HELIOSVIEW_KEY_LEFT_ALT,
    RightAlt = HELIOSVIEW_KEY_RIGHT_ALT,
    LeftMeta = HELIOSVIEW_KEY_LEFT_META,
    RightMeta = HELIOSVIEW_KEY_RIGHT_META,
    // punctuation
    Minus = HELIOSVIEW_KEY_MINUS,
    Equal = HELIOSVIEW_KEY_EQUAL,
    LeftBracket = HELIOSVIEW_KEY_LEFT_BRACKET,
    RightBracket = HELIOSVIEW_KEY_RIGHT_BRACKET,
    Backslash = HELIOSVIEW_KEY_BACKSLASH,
    Semicolon = HELIOSVIEW_KEY_SEMICOLON,
    Apostrophe = HELIOSVIEW_KEY_APOSTROPHE,
    Grave = HELIOSVIEW_KEY_GRAVE,
    Comma = HELIOSVIEW_KEY_COMMA,
    Period = HELIOSVIEW_KEY_PERIOD,
    Slash = HELIOSVIEW_KEY_SLASH,
    // locks / system
    CapsLock = HELIOSVIEW_KEY_CAPS_LOCK,
    NumLock = HELIOSVIEW_KEY_NUM_LOCK,
    ScrollLock = HELIOSVIEW_KEY_SCROLL_LOCK,
    PrintScreen = HELIOSVIEW_KEY_PRINT_SCREEN,
    Pause = HELIOSVIEW_KEY_PAUSE,
    Menu = HELIOSVIEW_KEY_MENU,
    // numeric keypad
    Numpad0 = HELIOSVIEW_KEY_NUMPAD_0,
    Numpad1 = HELIOSVIEW_KEY_NUMPAD_1,
    Numpad2 = HELIOSVIEW_KEY_NUMPAD_2,
    Numpad3 = HELIOSVIEW_KEY_NUMPAD_3,
    Numpad4 = HELIOSVIEW_KEY_NUMPAD_4,
    Numpad5 = HELIOSVIEW_KEY_NUMPAD_5,
    Numpad6 = HELIOSVIEW_KEY_NUMPAD_6,
    Numpad7 = HELIOSVIEW_KEY_NUMPAD_7,
    Numpad8 = HELIOSVIEW_KEY_NUMPAD_8,
    Numpad9 = HELIOSVIEW_KEY_NUMPAD_9,
    NumpadDecimal = HELIOSVIEW_KEY_NUMPAD_DECIMAL,
    NumpadDivide = HELIOSVIEW_KEY_NUMPAD_DIVIDE,
    NumpadMultiply = HELIOSVIEW_KEY_NUMPAD_MULTIPLY,
    NumpadSubtract = HELIOSVIEW_KEY_NUMPAD_SUBTRACT,
    NumpadAdd = HELIOSVIEW_KEY_NUMPAD_ADD,
    NumpadEnter = HELIOSVIEW_KEY_NUMPAD_ENTER,
};

// Mouse button identifier (mirrors heliosview_mouse_button_t)
enum class MouseButton : int32_t {
    Left = HELIOSVIEW_MOUSE_LEFT,
    Right = HELIOSVIEW_MOUSE_RIGHT,
    Middle = HELIOSVIEW_MOUSE_MIDDLE,
    X1 = HELIOSVIEW_MOUSE_X1,   // extra button 1 (browser "back")
    X2 = HELIOSVIEW_MOUSE_X2,   // extra button 2 (browser "forward")
};

// Standard menu roles (mirrors heliosview_menu_role_t). A role action gets the
// platform's conventional label/shortcut, and the library performs the action
// where the application cannot (edit commands, window commands). See the table
// in heliosview.h.
enum class MenuRole : int32_t {
    None = HELIOSVIEW_MENU_ROLE_NONE,
    About = HELIOSVIEW_MENU_ROLE_ABOUT,
    Preferences = HELIOSVIEW_MENU_ROLE_PREFERENCES,
    Quit = HELIOSVIEW_MENU_ROLE_QUIT,
    Hide = HELIOSVIEW_MENU_ROLE_HIDE,
    HideOthers = HELIOSVIEW_MENU_ROLE_HIDE_OTHERS,
    ShowAll = HELIOSVIEW_MENU_ROLE_SHOW_ALL,
    Services = HELIOSVIEW_MENU_ROLE_SERVICES,
    Undo = HELIOSVIEW_MENU_ROLE_UNDO,
    Redo = HELIOSVIEW_MENU_ROLE_REDO,
    Cut = HELIOSVIEW_MENU_ROLE_CUT,
    Copy = HELIOSVIEW_MENU_ROLE_COPY,
    Paste = HELIOSVIEW_MENU_ROLE_PASTE,
    SelectAll = HELIOSVIEW_MENU_ROLE_SELECT_ALL,
    Delete = HELIOSVIEW_MENU_ROLE_DELETE,
    Minimize = HELIOSVIEW_MENU_ROLE_MINIMIZE,
    Zoom = HELIOSVIEW_MENU_ROLE_ZOOM,
    CloseWindow = HELIOSVIEW_MENU_ROLE_CLOSE_WINDOW,
    ToggleFullscreen = HELIOSVIEW_MENU_ROLE_TOGGLE_FULLSCREEN,
    BringAllToFront = HELIOSVIEW_MENU_ROLE_BRING_ALL_TO_FRONT,
};

// What a submenu is, beyond its title (mirrors heliosview_menu_kind_t). macOS
// wires the standard menus (App / Services / Window / Help) with it; other
// platforms treat it as a hint.
enum class MenuKind : int32_t {
    Normal = HELIOSVIEW_MENU_KIND_NORMAL,
    App = HELIOSVIEW_MENU_KIND_APP,
    Services = HELIOSVIEW_MENU_KIND_SERVICES,
    Window = HELIOSVIEW_MENU_KIND_WINDOW,
    Help = HELIOSVIEW_MENU_KIND_HELP,
};

// Why a WebView navigation failed (mirrors heliosview_webview_error_t).
// 0 = success; the engine's own code is available via lastNativeError().
enum class WebViewError : int32_t {
    Ok = HELIOSVIEW_WEBVIEW_OK,
    Cancelled = HELIOSVIEW_WEBVIEW_ERROR_CANCELLED,
    HostNotFound = HELIOSVIEW_WEBVIEW_ERROR_HOST_NOT_FOUND,
    ConnectionFailed = HELIOSVIEW_WEBVIEW_ERROR_CONNECTION_FAILED,
    Timeout = HELIOSVIEW_WEBVIEW_ERROR_TIMEOUT,
    Tls = HELIOSVIEW_WEBVIEW_ERROR_TLS,
    Http = HELIOSVIEW_WEBVIEW_ERROR_HTTP,
    Other = HELIOSVIEW_WEBVIEW_ERROR_OTHER,
};

// Modifier keys held during an input event (HELIOSVIEW_MOD_* bits)
namespace mods {
inline constexpr uint32_t Shift = HELIOSVIEW_MOD_SHIFT;
inline constexpr uint32_t Ctrl = HELIOSVIEW_MOD_CTRL;
inline constexpr uint32_t Alt = HELIOSVIEW_MOD_ALT;
inline constexpr uint32_t Meta = HELIOSVIEW_MOD_META;
inline constexpr uint32_t LeftShift = HELIOSVIEW_MOD_LEFT_SHIFT;
inline constexpr uint32_t RightShift = HELIOSVIEW_MOD_RIGHT_SHIFT;
inline constexpr uint32_t LeftCtrl = HELIOSVIEW_MOD_LEFT_CTRL;
inline constexpr uint32_t RightCtrl = HELIOSVIEW_MOD_RIGHT_CTRL;
inline constexpr uint32_t LeftAlt = HELIOSVIEW_MOD_LEFT_ALT;
inline constexpr uint32_t RightAlt = HELIOSVIEW_MOD_RIGHT_ALT;
inline constexpr uint32_t LeftMeta = HELIOSVIEW_MOD_LEFT_META;
inline constexpr uint32_t RightMeta = HELIOSVIEW_MOD_RIGHT_META;
inline constexpr uint32_t CapsLock = HELIOSVIEW_MOD_CAPS_LOCK;
inline constexpr uint32_t NumLock = HELIOSVIEW_MOD_NUM_LOCK;
} // namespace mods

// Full key state for one key event (Window::keyEvent)
struct KeyEvent {
    KeyCode key = KeyCode::Unknown;
    uint32_t modifiers = 0;   // HELIOSVIEW_MOD_* bits held when the event fired
    bool pressed = false;     // true = key down, false = key up
    bool repeat = false;      // pressed && OS auto-repeat (not the initial press)
};

/* predefined window styles */
enum class WindowStyle : int32_t {
    Normal = HELIOSVIEW_WINDOW_NORMAL,   /* normal window: title bar + border + system menu */
    Borderless = HELIOSVIEW_WINDOW_BORDERLESS, /* borderless (fully custom drawn; not resizable) */
    Frameless = HELIOSVIEW_WINDOW_FRAMELESS,   /* fully frameless: no title bar / caption buttons; resizable via the edges; the app draws all chrome (e.g. the injected <helios-window-controls> web component for the buttons) */
};

// Window style flags (mirrors heliosview_window_flag_t; OR them together).
// Refine the preset style — on macOS TitleBarHidden/TitleBarTransparent/
// FullSizeContent give the idiomatic look (real title bar, hidden title,
// content underneath, traffic lights floating over the page); on Windows they
// all mean "no native caption, the client area fills the window".
enum class WindowFlag : uint32_t {
    None = HELIOSVIEW_WINDOW_FLAG_NONE,
    TitleBarHidden = HELIOSVIEW_WINDOW_FLAG_TITLEBAR_HIDDEN,
    TitleBarTransparent = HELIOSVIEW_WINDOW_FLAG_TITLEBAR_TRANSPARENT,
    FullSizeContent = HELIOSVIEW_WINDOW_FLAG_FULL_SIZE_CONTENT,
    Closable = HELIOSVIEW_WINDOW_FLAG_CLOSABLE,
    Minimizable = HELIOSVIEW_WINDOW_FLAG_MINIMIZABLE,
    Resizable = HELIOSVIEW_WINDOW_FLAG_RESIZABLE,
    ToolWindow = HELIOSVIEW_WINDOW_FLAG_TOOLWINDOW,
};

inline constexpr WindowFlag operator|(WindowFlag a, WindowFlag b)
{
    return static_cast<WindowFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr uint32_t toUint(WindowFlag flags) { return static_cast<uint32_t>(flags); }

// Icon flags (mirrors heliosview_icon_flag_t; OR them together)
enum class IconFlag : uint32_t {
    None = HELIOSVIEW_ICON_FLAG_NONE,
    Template = HELIOSVIEW_ICON_FLAG_TEMPLATE, /* macOS: monochrome template image */
};

inline constexpr IconFlag operator|(IconFlag a, IconFlag b)
{
    return static_cast<IconFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr uint32_t toUint(IconFlag flags) { return static_cast<uint32_t>(flags); }

// How the process presents itself to the OS (mirrors
// heliosview_activation_policy_t). Accessory is the right choice for a
// tray-only/menu-bar-only app on macOS (no Dock icon).
enum class ActivationPolicy : int32_t {
    Regular = HELIOSVIEW_ACTIVATION_REGULAR,
    Accessory = HELIOSVIEW_ACTIVATION_ACCESSORY,
    Prohibited = HELIOSVIEW_ACTIVATION_PROHIBITED,
};

// OS notification permission (mirrors heliosview_notification_permission_t)
enum class NotificationPermission : int32_t {
    Unknown = HELIOSVIEW_NOTIFICATION_PERMISSION_UNKNOWN,
    Granted = HELIOSVIEW_NOTIFICATION_PERMISSION_GRANTED,
    Denied = HELIOSVIEW_NOTIFICATION_PERMISSION_DENIED,
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
    uintptr_t windowId = 0;            /* native handle of the window that produced the event (0 = none; HWND on Windows) */
    int64_t timestampMs = 0;         /* milliseconds since library initialization */
    int32_t x = 0;                   /* mouse X (MouseMove / MouseButton*) */
    int32_t y = 0;                   /* mouse Y */
    int32_t width = 0;               /* new width (WindowResize) */
    int32_t height = 0;              /* new height (WindowResize) */
    KeyCode key = KeyCode::Unknown;  /* key code (KeyDown / KeyUp) */
    MouseButton mouseButton = MouseButton::Left; /* button (MouseButton*) */
    uint32_t menuItem = 0;           /* menu item id (MenuSelect) */
    void* userdata = nullptr;        /* owning Tray/Menu object (Tray* / MenuSelect) */
    uint32_t modifiers = 0;          /* HELIOSVIEW_MOD_* bits (KeyDown/KeyUp, MouseButtonDown/Up, TextInput) */
    uint32_t flags = 0;              /* HELIOSVIEW_EVENT_FLAG_* bits (KeyDown) */
    uint32_t textLen = 0;            /* UTF-8 byte length of text (TextInput) */
    char text[48] = {};              /* UTF-8 text, NUL-terminated (TextInput; IME commits included) */

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
        e.modifiers = c.modifiers;
        e.flags = c.flags;
        e.textLen = c.text_len;
        std::memcpy(e.text, c.text, sizeof e.text);
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
        c.modifiers = modifiers;
        c.flags = flags;
        c.text_len = textLen;
        std::memcpy(c.text, text, sizeof c.text);
        return c;
    }
};

} // namespace helios
