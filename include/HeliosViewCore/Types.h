#pragma once

/**
 * HeliosView.Core —— 事件类型与事件结构。
 *
 * 与 HeliosView.dll 的 C 接口（heliosview.h）中的枚举/结构一一对应，
 * 提供类型安全的 C++ 枚举与事件对象（含 C ←→ C++ 转换）。
 */

#include <HeliosView/heliosview.h>

#include <cstdint>

namespace helios {

/* ---------- 与 C 枚举一一对应的类型 ---------- */

enum class EventType : int32_t {
    Quit = HELIOSVIEW_EVENT_QUIT,
    WindowClose = HELIOSVIEW_EVENT_WINDOW_CLOSE,
    WindowResize = HELIOSVIEW_EVENT_WINDOW_RESIZE,
    KeyDown = HELIOSVIEW_EVENT_KEY_DOWN,
    KeyUp = HELIOSVIEW_EVENT_KEY_UP,
    MouseMove = HELIOSVIEW_EVENT_MOUSE_MOVE,
    MouseButtonDown = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN,
    MouseButtonUp = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP,
};

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

enum class MouseButton : int32_t {
    Left = HELIOSVIEW_MOUSE_LEFT,
    Right = HELIOSVIEW_MOUSE_RIGHT,
    Middle = HELIOSVIEW_MOUSE_MIDDLE,
};

/* ---------- 事件 ---------- */

struct Event {
    EventType type = EventType::Quit;
    int32_t windowId = 0;            /* 产生事件的窗口 id（0 = 与窗口无关） */
    int64_t timestampMs = 0;
    int32_t x = 0;                   /* 鼠标坐标 X（窗口客户区） */
    int32_t y = 0;                   /* 鼠标坐标 Y */
    int32_t width = 0;               /* 窗口宽（WindowResize） */
    int32_t height = 0;              /* 窗口高（WindowResize） */
    KeyCode key = KeyCode::Unknown;  /* 键码（KeyDown / KeyUp） */
    MouseButton mouseButton = MouseButton::Left;

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
        return e;
    }

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
        return c;
    }
};

} // namespace helios
