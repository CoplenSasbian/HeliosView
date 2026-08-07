#ifndef HELIOSVIEW_HELIOSVIEW_H
#define HELIOSVIEW_HELIOSVIEW_H

/**
 * HeliosView C API（HeliosView.dll 对外唯一接口）。
 *
 * 本头文件是纯 C 接口，保证 ABI 稳定：
 *   - 只使用 C 兼容类型（POD），不跨 DLL 边界传递 C++ 对象/异常
 *   - 函数均为 extern "C" 链接
 *   - 错误通过返回值/错误码报告，不用异常
 *
 * 平台细节（Win32 消息、HWND 等）不出现在本接口中：
 *   - 原生消息以不透明指针 void* 传给注册的转换委托，仅回调期间有效
 *   - 窗口为不透明句柄 heliosview_window_t*
 *   - 键码/鼠标键位统一映射为平台无关枚举
 *
 * 事件模型（类似 SDL）：
 *   - heliosview_run 进行消息循环，把原生消息经（可注册的）转换委托
 *     转成 heliosview_event_t 事件入队
 *   - heliosview_poll / heliosview_wait 从队列取事件
 *   - heliosview_post_event 任意线程投递事件
 *
 * C++ 使用者请包含 <HeliosViewCore/HeliosView.h>（HeliosView.Core 封装层）。
 */

#include <stdint.h>

#include <HeliosView/heliosview_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 版本 ================= */

HELIOSVIEW_API const char* heliosview_version(void);

/* ================= 事件 ================= */

typedef enum heliosview_event_type {
    HELIOSVIEW_EVENT_QUIT = 1,          /* 退出请求（heliosview_post_event 投递） */
    HELIOSVIEW_EVENT_WINDOW_CLOSE,      /* 窗口关闭请求（用户点击 X） */
    HELIOSVIEW_EVENT_WINDOW_RESIZE,     /* 窗口尺寸变化 */
    HELIOSVIEW_EVENT_KEY_DOWN,
    HELIOSVIEW_EVENT_KEY_UP,
    HELIOSVIEW_EVENT_MOUSE_MOVE,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_DOWN,
    HELIOSVIEW_EVENT_MOUSE_BUTTON_UP
} heliosview_event_type_t;

/* 与平台无关的通用键码（原生键码在 C 层映射） */
typedef enum heliosview_keycode {
    HELIOSVIEW_KEY_UNKNOWN = 0,
    HELIOSVIEW_KEY_ESCAPE,
    HELIOSVIEW_KEY_RETURN,
    HELIOSVIEW_KEY_SPACE,
    HELIOSVIEW_KEY_LEFT,
    HELIOSVIEW_KEY_RIGHT,
    HELIOSVIEW_KEY_UP,
    HELIOSVIEW_KEY_DOWN,
    HELIOSVIEW_KEY_0,
    HELIOSVIEW_KEY_1,
    HELIOSVIEW_KEY_2,
    HELIOSVIEW_KEY_3,
    HELIOSVIEW_KEY_4,
    HELIOSVIEW_KEY_5,
    HELIOSVIEW_KEY_6,
    HELIOSVIEW_KEY_7,
    HELIOSVIEW_KEY_8,
    HELIOSVIEW_KEY_9,
    HELIOSVIEW_KEY_A,
    HELIOSVIEW_KEY_B,
    HELIOSVIEW_KEY_C,
    HELIOSVIEW_KEY_D,
    HELIOSVIEW_KEY_E,
    HELIOSVIEW_KEY_F,
    HELIOSVIEW_KEY_G,
    HELIOSVIEW_KEY_H,
    HELIOSVIEW_KEY_I,
    HELIOSVIEW_KEY_J,
    HELIOSVIEW_KEY_K,
    HELIOSVIEW_KEY_L,
    HELIOSVIEW_KEY_M,
    HELIOSVIEW_KEY_N,
    HELIOSVIEW_KEY_O,
    HELIOSVIEW_KEY_P,
    HELIOSVIEW_KEY_Q,
    HELIOSVIEW_KEY_R,
    HELIOSVIEW_KEY_S,
    HELIOSVIEW_KEY_T,
    HELIOSVIEW_KEY_U,
    HELIOSVIEW_KEY_V,
    HELIOSVIEW_KEY_W,
    HELIOSVIEW_KEY_X,
    HELIOSVIEW_KEY_Y,
    HELIOSVIEW_KEY_Z,
    HELIOSVIEW_KEY_F1,
    HELIOSVIEW_KEY_F2,
    HELIOSVIEW_KEY_F3,
    HELIOSVIEW_KEY_F4,
    HELIOSVIEW_KEY_F5,
    HELIOSVIEW_KEY_F6,
    HELIOSVIEW_KEY_F7,
    HELIOSVIEW_KEY_F8,
    HELIOSVIEW_KEY_F9,
    HELIOSVIEW_KEY_F10,
    HELIOSVIEW_KEY_F11,
    HELIOSVIEW_KEY_F12
} heliosview_keycode_t;

typedef enum heliosview_mouse_button {
    HELIOSVIEW_MOUSE_LEFT = 1,
    HELIOSVIEW_MOUSE_RIGHT,
    HELIOSVIEW_MOUSE_MIDDLE
} heliosview_mouse_button_t;

/* 事件：扁平 POD，跨 DLL 边界安全传递 */
typedef struct heliosview_event {
    heliosview_event_type_t type;            /* 事件类型 */
    int32_t window_id;                       /* 产生事件的窗口 id（0 = 与窗口无关） */
    int64_t timestamp_ms;                    /* 距库初始化的毫秒数 */
    int32_t x;                               /* 鼠标坐标 X（窗口客户区） */
    int32_t y;                               /* 鼠标坐标 Y */
    int32_t width;                           /* 窗口宽（WINDOW_RESIZE） */
    int32_t height;                          /* 窗口高（WINDOW_RESIZE） */
    heliosview_keycode_t key;                /* 键码（KEY_DOWN / KEY_UP） */
    heliosview_mouse_button_t mouse_button;  /* 键位（MOUSE_BUTTON_*） */
} heliosview_event_t;

/* ================= 事件队列 ================= */

/* 非阻塞取事件：1 = 取到并写入 out，0 = 队列为空 */
HELIOSVIEW_API int heliosview_poll(heliosview_event_t* out_event);

/* 阻塞取事件：1 = 取到，-1 = 退出请求（heliosview_quit），0 = 其他错误。
 * 会阻塞当前线程直到事件到达或退出请求。 */
HELIOSVIEW_API int heliosview_wait(heliosview_event_t* out_event);

/* 任意线程投递事件。timestamp_ms 为 0 时自动填充。 */
HELIOSVIEW_API void heliosview_post_event(const heliosview_event_t* event);

/* 请求退出消息循环（heliosview_run 返回） */
HELIOSVIEW_API void heliosview_quit(void);

/* ================= 原生消息 → 事件转换 ================= */

/* 转换委托：native_msg 为平台原生消息指针，仅回调期间有效，
 * Windows 下为 const MSG*（回调运行在消息派发线程）。返回值：
 *   1  -> 已转换为事件写入 out_event（入队）
 *   0  -> 已消费该消息，不入队
 *  -1  -> 不处理，回落到库内默认转换（标准消息库内置转换）
 * 未注册委托时全部走库内默认转换。 */
typedef int (*heliosview_native_handler_fn)(void* native_msg, heliosview_event_t* out_event);

HELIOSVIEW_API void heliosview_set_native_handler(heliosview_native_handler_fn handler);

/* ================= 消息循环 ================= */

/* 每轮消息泵空后调用一次；返回非 0 退出循环 */
typedef int (*heliosview_loop_callback)(void* userdata);

/* 单次泵取全部待处理原生消息并转换为事件入队（非阻塞） */
HELIOSVIEW_API void heliosview_pump_events(void);

/* 消息循环：泵取原生消息 → 转换入队 → 回调 frame_callback（应用帧逻辑）。
 * 0 = 正常退出（heliosview_quit / WM_QUIT / 回调返回非 0） */
HELIOSVIEW_API int heliosview_run(heliosview_loop_callback frame_callback, void* userdata);

/* ================= 窗口 ================= */

typedef struct heliosview_window heliosview_window_t;

/* 创建窗口（仅登记参数，原生窗口在 show() 时创建）。失败返回 NULL。 */
HELIOSVIEW_API heliosview_window_t* heliosview_window_create(int width, int height, const char* title);

/* 销毁窗口并释放资源 */
HELIOSVIEW_API void heliosview_window_destroy(heliosview_window_t* window);

/* 创建并显示原生窗口：0 = 成功，负数为错误码 */
HELIOSVIEW_API int heliosview_window_show(heliosview_window_t* window);

/* 窗口 id（事件中 window_id 的来源） */
HELIOSVIEW_API int32_t heliosview_window_id(const heliosview_window_t* window);

#ifdef __cplusplus
}
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_H */
