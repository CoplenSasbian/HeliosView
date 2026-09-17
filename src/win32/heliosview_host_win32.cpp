// HeliosView.dll -- Win32 Host and UI Subclass implementation
// Implements heliosview_host_* functions from <HeliosView/heliosview_host.h>.

#include <HeliosView/heliosview_host.h>
#include <HeliosView/heliosview_presenter.h>
#include <HeliosView/heliosview_ui.h>
#include "../heliosview_internal.h"
#include "heliosview_win32_internal.h"
#include "heliosview_win32_input_context.h"

#include <commctrl.h>
#include <imm.h>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")

// ================= Internal Host Struct =================

struct heliosview_host {
    HWND hwnd = nullptr;
    heliosview_window_t* parent_window = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool visible = true;
    void* subclass_data = nullptr;
    void (*subclass_dtor)(heliosview_host_t* host) = nullptr;
};

namespace {

/* Temporary diagnostic: set HELIOSVIEW_IME_LOG=1 to trace the IME messages a host
 * window receives. Used to verify the composition path against a real IME. */
static bool ime_log_enabled() {
    static const bool on = [] {
        char buf[8] = {};
        return GetEnvironmentVariableA("HELIOSVIEW_IME_LOG", buf, sizeof buf) > 0 && buf[0] == '1';
    }();
    return on;
}

static void ime_log(const char* fmt, ...) {
    if (!ime_log_enabled()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "D:\\cpp\\HeliosView\\out\\ime.log", "a") != 0 || !f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

constexpr wchar_t kHostClassName[] = L"HeliosView_Host_Window";
std::atomic<bool> s_class_registered{false};

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureHostClassRegistered() {
    if (s_class_registered.load(std::memory_order_acquire))
        return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = nullptr; // No background brush to prevent flickering
    wc.lpszClassName = kHostClassName;

    RegisterClassExW(&wc);
    s_class_registered.store(true, std::memory_order_release);
}

// UI Subclass Data
struct hv_host_ui_subclass {
    heliosview_canvas_t* canvas = nullptr;
    heliosview_buffer_presenter_t* presenter = nullptr;
    heliosview_host_ui_paint_cb paint_cb = nullptr;
    void* paint_udata = nullptr;
    heliosview_host_ui_mouse_cb mouse_cb = nullptr;
    void* mouse_udata = nullptr;
    bool mouse_tracking = false;
    std::unique_ptr<hv::win32::Win32InputContext> input_context;
};


/* ---------- text input ----------
 *
 * The host child window takes focus on click (SetFocus in WM_LBUTTONDOWN), so the
 * keyboard and IME messages arrive HERE rather than at the parent window: WM_CHAR is
 * posted to the focused window, and an IME only composes into a focused window. That
 * is why the host dispatches text itself instead of leaving it to the parent's event
 * converter, and why heliosview_host_ui_set_ime_caret exists at all.
 */

/* Modifier state for a key message: the keyboard state, plus the one modifier the
 * message itself carries -- bit 29 is the context bit, i.e. Alt was held (Ctrl and
 * Shift have no bit of their own; GetKeyState is their only source, and it describes
 * the message-time state on the thread that reads it). */
static uint32_t hv_host_key_modifiers(LPARAM lp) {
    uint32_t m = map_modifiers();
    if (lp & (1 << 29)) {
        m |= HELIOSVIEW_MOD_ALT;
    }
    return m;
}

constexpr UINT_PTR kUiSubclassId = 0x48565549; // "HVUI"

LRESULT CALLBACK UiSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* host = reinterpret_cast<heliosview_host_t*>(dwRefData);
    if (!host)
        return DefSubclassProc(hwnd, msg, wp, lp);

    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    if (!ui)
        return DefSubclassProc(hwnd, msg, wp, lp);

    /* ---- input context handling (WM_CHAR, WM_IME_*, and key suppression during composition) ---- */
    if (ui->input_context) {
        LRESULT res = 0;
        if (ui->input_context->handleMessage(hwnd, msg, wp, lp, &res)) {
            return res;
        }
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // Prevent background erasing to avoid flicker


    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);

        if (ui->paint_cb && ui->canvas) {
            heliosview_painter_t* painter = heliosview_painter_begin(ui->canvas);
            if (painter) {
                ui->paint_cb(host, painter, ui->paint_udata);
                heliosview_painter_end(painter);
            }
        }

        if (ui->presenter && ui->canvas) {
            heliosview_pixel_view_t view = heliosview_canvas_pixel_view(ui->canvas);
            heliosview_buffer_presenter_present(ui->presenter, &view);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lp);
        int h = HIWORD(lp);
        if (w > 0 && h > 0 && ui->canvas) {
            heliosview_canvas_resize(ui->canvas, w, h);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!ui->mouse_tracking) {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            ui->mouse_tracking = true;
        }
        if (ui->mouse_cb) {
            heliosview_host_mouse_event_t evt{};
            evt.action = HELIOSVIEW_HOST_MOUSE_MOVE;
            evt.x = static_cast<short>(LOWORD(lp));
            evt.y = static_cast<short>(HIWORD(lp));
            evt.button = (wp & MK_LBUTTON) ? 1 : ((wp & MK_RBUTTON) ? 2 : 0);
            ui->mouse_cb(host, &evt, ui->mouse_udata);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        ui->mouse_tracking = false;
        if (ui->mouse_cb) {
            heliosview_host_mouse_event_t evt{};
            evt.action = HELIOSVIEW_HOST_MOUSE_LEAVE;
            ui->mouse_cb(host, &evt, ui->mouse_udata);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        SetFocus(hwnd);
        SetCapture(hwnd);
        if (ui->mouse_cb) {
            heliosview_host_mouse_event_t evt{};
            evt.action = HELIOSVIEW_HOST_MOUSE_DOWN;
            evt.x = static_cast<short>(LOWORD(lp));
            evt.y = static_cast<short>(HIWORD(lp));
            evt.button = (msg == WM_LBUTTONDOWN) ? 1 : 2;
            ui->mouse_cb(host, &evt, ui->mouse_udata);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        if (GetCapture() == hwnd)
            ReleaseCapture();
        if (ui->mouse_cb) {
            heliosview_host_mouse_event_t evt{};
            evt.action = HELIOSVIEW_HOST_MOUSE_UP;
            evt.x = static_cast<short>(LOWORD(lp));
            evt.y = static_cast<short>(HIWORD(lp));
            evt.button = (msg == WM_LBUTTONUP) ? 1 : 2;
            ui->mouse_cb(host, &evt, ui->mouse_udata);
        }
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, UiSubclassProc, uIdSubclass);
        return DefSubclassProc(hwnd, msg, wp, lp);

    /* ---- keyboard ----
     *
     * The host child window holds the focus (SetFocus in WM_LBUTTONDOWN), so the key
     * messages arrive HERE, not at the parent window -- the parent's event converter
     * never sees them, which is why keys are forwarded to the focused widget from this
     * procedure rather than from the window's event dispatch. Translation uses the same
     * map_vk the window converter does, so a key means the same thing on both paths.
     *
     * While an IME is composing, the keys belong to the IME: it is choosing a candidate
     * with digits and arrows, and a widget that treats them as editing keys both breaks
     * the composition and edits text the user has not committed. Composition state is
     * tracked through the WM_IME_* messages below.
     *
     * A widget that does not consume a key is also asked to leave it alone: anything it
     * reports as handled is swallowed, everything else keeps its default behaviour. */
    /* ---- keyboard ---- */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        heliosview_host_ui_dispatch_key(host, static_cast<int>(map_vk(static_cast<UINT>(wp))),
                                        hv_host_key_modifiers(lp), 1);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        heliosview_host_ui_dispatch_key(host, static_cast<int>(map_vk(static_cast<UINT>(wp))),
                                        hv_host_key_modifiers(lp), 0);
        return 0;

    case WM_IME_SETCONTEXT:
        ime_log("WM_IME_SETCONTEXT active=%d", (int)wp);
        if (wp) {
            lp &= ~ISC_SHOWUICOMPOSITIONWINDOW;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);


    default:
        break;
    }

    return DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace

// ================= Generic Host Implementation =================

heliosview_host_t* hv_host_create_raw(heliosview_window_t* parent, int x, int y, int width, int height) {
    if (!parent)
        return nullptr;

    HWND parent_hwnd = hv_window_hwnd(parent);
    if (!parent_hwnd)
        return nullptr;

    EnsureHostClassRegistered();

    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    HWND child_hwnd = CreateWindowExW(
        0,
        kHostClassName,
        nullptr,
        style,
        x, y, (width > 0 ? width : 1), (height > 0 ? height : 1),
        parent_hwnd,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (!child_hwnd)
        return nullptr;

    heliosview_host* host = nullptr;
    try {
        host = hv::hv_alloc<heliosview_host>();
    } catch (const std::bad_alloc&) {
        DestroyWindow(child_hwnd);
        return nullptr;
    }
    host->hwnd = child_hwnd;
    host->parent_window = parent;
    host->x = x;
    host->y = y;
    host->width = width;
    host->height = height;
    host->visible = true;

    return host;
}

void heliosview_host_destroy(heliosview_host_t* host) {
    if (!host)
        return;

    if (host->subclass_dtor) {
        host->subclass_dtor(host);
        host->subclass_dtor = nullptr;
    }

    if (host->hwnd && IsWindow(host->hwnd)) {
        DestroyWindow(host->hwnd);
        host->hwnd = nullptr;
    }

    hv::hv_dealloc(host);
}

void heliosview_host_set_bounds(heliosview_host_t* host, int x, int y, int width, int height) {
    if (!host || !host->hwnd)
        return;

    host->x = x;
    host->y = y;
    host->width = width;
    host->height = height;

    MoveWindow(host->hwnd, x, y, width, height, TRUE);
}

void heliosview_host_get_bounds(const heliosview_host_t* host, int* x, int* y, int* width, int* height) {
    if (!host)
        return;
    if (x) *x = host->x;
    if (y) *y = host->y;
    if (width) *width = host->width;
    if (height) *height = host->height;
}

void heliosview_host_set_visible(heliosview_host_t* host, int visible) {
    if (!host || !host->hwnd)
        return;

    host->visible = (visible != 0);
    ShowWindow(host->hwnd, host->visible ? SW_SHOW : SW_HIDE);
}

int heliosview_host_is_visible(const heliosview_host_t* host) {
    if (!host || !host->hwnd)
        return 0;
    return IsWindowVisible(host->hwnd) ? 1 : 0;
}

heliosview_window_t* heliosview_host_get_parent(const heliosview_host_t* host) {
    return host ? host->parent_window : nullptr;
}

void* heliosview_host_native_handle(heliosview_host_t* host) {
    return host ? reinterpret_cast<void*>(host->hwnd) : nullptr;
}

void* heliosview_host_get_subclass(const heliosview_host_t* host) {
    return host ? host->subclass_data : nullptr;
}

// ================= UI Subclass Implementation =================

namespace {

void DestroyUiSubclass(heliosview_host_t* host) {
    if (!host || !host->subclass_data)
        return;

    heliosview_host_ui_clear_binding(host);

    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    RemoveWindowSubclass(host->hwnd, UiSubclassProc, kUiSubclassId);

    if (ui->presenter) {
        heliosview_buffer_presenter_destroy(ui->presenter);
        ui->presenter = nullptr;
    }
    if (ui->canvas) {
        heliosview_canvas_destroy(ui->canvas);
        ui->canvas = nullptr;
    }
    ui->input_context.reset();

    hv::hv_dealloc(ui);
    host->subclass_data = nullptr;
}

} // namespace

heliosview_host_t* heliosview_host_create_ui(
    heliosview_window_t* parent, int x, int y, int width, int height, heliosview_canvas_engine_t engine) {
    
    heliosview_host_t* host = hv_host_create_raw(parent, x, y, width, height);
    if (!host)
        return nullptr;

    hv_host_ui_subclass* ui = nullptr;
    try {
        ui = hv::hv_alloc<hv_host_ui_subclass>();
    } catch (const std::bad_alloc&) {
        heliosview_host_destroy(host);
        return nullptr;
    }
    ui->canvas = heliosview_canvas_create(width > 0 ? width : 1, height > 0 ? height : 1, HELIOSVIEW_FORMAT_AUTO, engine);
    ui->presenter = heliosview_buffer_presenter_create_for_hwnd(host->hwnd);
    ui->input_context = std::make_unique<hv::win32::Win32InputContext>(host, host->hwnd);

    host->subclass_data = ui;
    host->subclass_dtor = DestroyUiSubclass;

    if (!SetWindowSubclass(host->hwnd, UiSubclassProc, kUiSubclassId, reinterpret_cast<DWORD_PTR>(host))) {
        heliosview_host_destroy(host);
        return nullptr;
    }

    return host;
}

heliosview_canvas_t* heliosview_host_ui_get_canvas(heliosview_host_t* host) {
    if (!host || !host->subclass_data)
        return nullptr;
    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    return ui->canvas;
}

void heliosview_host_ui_present(heliosview_host_t* host) {
    if (!host || !host->subclass_data)
        return;
    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    if (ui->presenter && ui->canvas) {
        heliosview_pixel_view_t view = heliosview_canvas_pixel_view(ui->canvas);
        heliosview_buffer_presenter_present(ui->presenter, &view);
    }
}

void heliosview_host_ui_request_repaint(heliosview_host_t* host) {
    if (!host || !host->hwnd)
        return;
    InvalidateRect(host->hwnd, nullptr, FALSE);
}

void heliosview_host_ui_set_paint_callback(
    heliosview_host_t* host, heliosview_host_ui_paint_cb callback, void* user_data) {
    if (!host || !host->subclass_data)
        return;
    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    ui->paint_cb = callback;
    ui->paint_udata = user_data;
}

void heliosview_host_ui_set_mouse_callback(
    heliosview_host_t* host, heliosview_host_ui_mouse_cb callback, void* user_data) {
    if (!host || !host->subclass_data)
        return;
    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    ui->mouse_cb = callback;
    ui->mouse_udata = user_data;
}

void heliosview_host_ui_set_ime_caret(heliosview_host_t* host, int x, int y, int height) {
    if (!host || !host->subclass_data)
        return;
    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    if (ui && ui->input_context) {
        ui->input_context->setFallbackCaret(x, y, height);
    }
}

void* heliosview_host_ui_get_input_context(heliosview_host_t* host) {
    if (!host || !host->subclass_data)
        return nullptr;
    auto* ui = static_cast<hv_host_ui_subclass*>(host->subclass_data);
    return ui ? ui->input_context.get() : nullptr;
}

HWND hv_host_hwnd(heliosview_host_t* host) {
    return host ? host->hwnd : nullptr;
}

void hv_host_attach_subclass(heliosview_host_t* host, void* subclass_data, void (*subclass_dtor)(heliosview_host_t* host)) {
    if (!host)
        return;
    host->subclass_data = subclass_data;
    host->subclass_dtor = subclass_dtor;
}

