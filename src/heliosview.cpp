#include <HeliosView/heliosview.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ================= 窗口（不透明结构，补全头文件中的前向声明） ================= */

struct heliosview_window {
    int32_t id = 0;
    int width = 0;
    int height = 0;
    std::string title; /* UTF-8 */
#ifdef _WIN32
    HWND hwnd = nullptr;
#endif
};

namespace {

/* ================= 全局状态 ================= */

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<heliosview_event_t> g_queue;
std::atomic<bool> g_quit{false};
heliosview_native_handler_fn g_native_handler = nullptr;
std::atomic<int32_t> g_next_window_id{1};

#ifdef _WIN32
/* post_event / quit 唤醒消息循环等待的事件对象 */
HANDLE g_wakeup = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif

int64_t now_ms()
{
#ifdef _WIN32
    static const int64_t base = static_cast<int64_t>(GetTickCount64());
    return static_cast<int64_t>(GetTickCount64()) - base;
#else
    return 0;
#endif
}

void queue_push(const heliosview_event_t& event)
{
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.push_back(event);
    }
    g_queue_cv.notify_one();
#ifdef _WIN32
    SetEvent(g_wakeup); /* 唤醒 MsgWaitForMultipleObjectsEx */
#endif
}

/* ================= 默认原生消息 → 事件转换（Win32 MSG → 事件） ================= */

heliosview_keycode_t map_vk(UINT vk)
{
    switch (vk) {
    case VK_ESCAPE: return HELIOSVIEW_KEY_ESCAPE;
    case VK_RETURN: return HELIOSVIEW_KEY_RETURN;
    case VK_SPACE:  return HELIOSVIEW_KEY_SPACE;
    case VK_LEFT:   return HELIOSVIEW_KEY_LEFT;
    case VK_RIGHT:  return HELIOSVIEW_KEY_RIGHT;
    case VK_UP:     return HELIOSVIEW_KEY_UP;
    case VK_DOWN:   return HELIOSVIEW_KEY_DOWN;
    default:
        if (vk >= 'A' && vk <= 'Z')
            return static_cast<heliosview_keycode_t>(HELIOSVIEW_KEY_A + (vk - 'A'));
        if (vk >= '0' && vk <= '9')
            return static_cast<heliosview_keycode_t>(HELIOSVIEW_KEY_0 + (vk - '0'));
        if (vk >= VK_F1 && vk <= VK_F12)
            return static_cast<heliosview_keycode_t>(HELIOSVIEW_KEY_F1 + (vk - VK_F1));
        return HELIOSVIEW_KEY_UNKNOWN;
    }
}

int default_native_convert(void* native_msg, heliosview_event_t* out)
{
#ifdef _WIN32
    const MSG* msg = static_cast<const MSG*>(native_msg);
    const int64_t ts = now_ms();

    switch (msg->message) {
    case WM_CLOSE:
        out->type = HELIOSVIEW_EVENT_WINDOW_CLOSE;
        out->timestamp_ms = ts;
        return 1;
    case WM_SIZE:
        out->type = HELIOSVIEW_EVENT_WINDOW_RESIZE;
        out->width = static_cast<int32_t>(LOWORD(msg->lParam));
        out->height = static_cast<int32_t>(HIWORD(msg->lParam));
        out->timestamp_ms = ts;
        return 1;
    case WM_KEYDOWN:
        if ((msg->lParam & 0x40000000) != 0)
            return 0; /* 过滤键盘自动重复 */
        out->type = HELIOSVIEW_EVENT_KEY_DOWN;
        out->key = map_vk(static_cast<UINT>(msg->wParam));
        out->timestamp_ms = ts;
        return 1;
    case WM_KEYUP:
        out->type = HELIOSVIEW_EVENT_KEY_UP;
        out->key = map_vk(static_cast<UINT>(msg->wParam));
        out->timestamp_ms = ts;
        return 1;
    case WM_MOUSEMOVE:
        out->type = HELIOSVIEW_EVENT_MOUSE_MOVE;
        break;
    case WM_LBUTTONDOWN: out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; out->mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONDOWN: out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; out->mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONDOWN: out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN; out->mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    case WM_LBUTTONUP:   out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   out->mouse_button = HELIOSVIEW_MOUSE_LEFT; break;
    case WM_RBUTTONUP:   out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   out->mouse_button = HELIOSVIEW_MOUSE_RIGHT; break;
    case WM_MBUTTONUP:   out->type = HELIOSVIEW_EVENT_MOUSE_BUTTON_UP;   out->mouse_button = HELIOSVIEW_MOUSE_MIDDLE; break;
    default:
        return -1; /* 未处理 → 交给 DefWindowProc */
    }

    out->x = static_cast<int32_t>(static_cast<int16_t>(LOWORD(msg->lParam)));
    out->y = static_cast<int32_t>(static_cast<int16_t>(HIWORD(msg->lParam)));
    out->timestamp_ms = ts;
    return 1;
#else
    (void)native_msg;
    (void)out;
    return -1;
#endif
}

#ifdef _WIN32
std::map<HWND, heliosview_window_t*> g_windows_by_hwnd;

LRESULT CALLBACK heliosview_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    const auto window_id_of = [hwnd] {
        auto it = g_windows_by_hwnd.find(hwnd);
        return it != g_windows_by_hwnd.end() ? it->second->id : 0;
    };

    MSG native{};
    native.hwnd = hwnd;
    native.message = message;
    native.wParam = wparam;
    native.lParam = lparam;

    /* 1. 注册的转换委托（可选） */
    heliosview_event_t event{};
    if (g_native_handler) {
        const int handled = g_native_handler(&native, &event);
        if (handled == 1) {
            event.window_id = window_id_of();
            queue_push(event);
            return 0;
        }
        if (handled == 0)
            return 0;
    }

    /* 2. 库默认转换（WM_CLOSE/WM_SIZE/键盘/鼠标等） */
    const int def = default_native_convert(&native, &event);
    if (def == 1) {
        event.window_id = window_id_of();
        queue_push(event);
        return 0; /* 消费消息：窗口销毁与否由应用决定 */
    }
    if (def == 0)
        return 0;

    return DefWindowProc(hwnd, message, wparam, lparam);
}
#endif

} // namespace

/* ================= 版本 ================= */

const char* heliosview_version(void)
{
    return HELIOSVIEW_VERSION_STR;
}

/* ================= 事件队列 ================= */

int heliosview_poll(heliosview_event_t* out_event)
{
    if (!out_event)
        return 0;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    if (g_queue.empty())
        return 0;
    *out_event = g_queue.front();
    g_queue.pop_front();
    return 1;
}

int heliosview_wait(heliosview_event_t* out_event)
{
    if (!out_event)
        return 0;
    std::unique_lock<std::mutex> lock(g_queue_mutex);
    g_queue_cv.wait(lock, [] { return !g_queue.empty() || g_quit.load(); });
    if (g_queue.empty())
        return -1; /* 退出请求且队列已空 */
    *out_event = g_queue.front();
    g_queue.pop_front();
    return 1;
}

void heliosview_post_event(const heliosview_event_t* event)
{
    if (!event)
        return;
    heliosview_event_t copy = *event;
    if (copy.timestamp_ms == 0)
        copy.timestamp_ms = now_ms();
    queue_push(copy);
}

void heliosview_quit(void)
{
    g_quit = true;
#ifdef _WIN32
    SetEvent(g_wakeup);
#endif
    g_queue_cv.notify_all();
}

/* ================= 转换委托 ================= */

void heliosview_set_native_handler(heliosview_native_handler_fn handler)
{
    g_native_handler = handler;
}

/* ================= 消息循环 ================= */

void heliosview_pump_events(void)
{
#ifdef _WIN32
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            heliosview_quit();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#else
    /* 无原生消息平台 */
#endif
}

int heliosview_run(heliosview_loop_callback frame_callback, void* userdata)
{
    g_quit = false;
#ifdef _WIN32
    while (!g_quit.load()) {
        heliosview_pump_events();
        if (g_quit.load())
            break;
        if (frame_callback && frame_callback(userdata) != 0) {
            g_quit = true;
            break;
        }
        /* 等待：新原生消息或 post_event/quit 唤醒 */
        ResetEvent(g_wakeup);
        const DWORD result = MsgWaitForMultipleObjectsEx(1, &g_wakeup, INFINITE,
                                                         QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED)
            break;
    }
#else
    while (!g_quit.load()) {
        if (frame_callback && frame_callback(userdata) != 0) {
            g_quit = true;
            break;
        }
        std::unique_lock<std::mutex> lock(g_queue_mutex);
        g_queue_cv.wait_for(lock, std::chrono::milliseconds(10), [] { return g_quit.load(); });
    }
#endif
    return 0;
}

/* ================= 窗口 ================= */

heliosview_window_t* heliosview_window_create(int width, int height, const char* title)
{
    if (!title)
        return nullptr;
    auto* window = new heliosview_window;
    window->id = g_next_window_id.fetch_add(1);
    window->width = width;
    window->height = height;
    window->title = title;
    return window;
}

void heliosview_window_destroy(heliosview_window_t* window)
{
    if (!window)
        return;
#ifdef _WIN32
    if (window->hwnd) {
        g_windows_by_hwnd.erase(window->hwnd);
        DestroyWindow(window->hwnd); /* 触发 WM_DESTROY → PostQuitMessage → 消息循环退出 */
    }
#endif
    delete window;
}

int heliosview_window_show(heliosview_window_t* window)
{
#ifdef _WIN32
    if (!window)
        return -1;
    if (window->hwnd) {
        ShowWindow(window->hwnd, SW_SHOW);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = heliosview_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"HeliosViewWindow";
    RegisterClassExW(&wc); /* 重复注册无害（类已存在时静默失败） */

    /* 标题 UTF-8 → UTF-16 */
    const int n = MultiByteToWideChar(CP_UTF8, 0, window->title.c_str(), -1, nullptr, 0);
    std::wstring title(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, window->title.c_str(), -1, title.data(), n);

    RECT rect{0, 0, window->width, window->height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    window->hwnd = CreateWindowExW(0, L"HeliosViewWindow", title.c_str(), WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   rect.right - rect.left, rect.bottom - rect.top,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window->hwnd)
        return -2;

    g_windows_by_hwnd[window->hwnd] = window;
    ShowWindow(window->hwnd, SW_SHOW);
    return 0;
#else
    (void)window;
    return -1; /* 未支持平台 */
#endif
}

int32_t heliosview_window_id(const heliosview_window_t* window)
{
    return window ? window->id : 0;
}
