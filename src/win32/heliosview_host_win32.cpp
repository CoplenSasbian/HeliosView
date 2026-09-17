// HeliosView.dll -- Win32 Host and UI Subclass implementation
// Implements heliosview_host_* functions from <HeliosView/heliosview_host.h>.

#include <HeliosView/heliosview_host.h>
#include <HeliosView/heliosview_presenter.h>
#include <HeliosView/heliosview_ui.h>
#include "../heliosview_internal.h"
#include "heliosview_win32_internal.h"

#include <commctrl.h>
#include <imm.h>
#include <cstdarg>
#include <cstdio>
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
    bool ime_caret_set = false;
};

/* ---------- text input ----------
 *
 * The host child window takes focus on click (SetFocus in WM_LBUTTONDOWN), so the
 * keyboard and IME messages arrive HERE rather than at the parent window: WM_CHAR is
 * posted to the focused window, and an IME only composes into a focused window. That
 * is why the host dispatches text itself instead of leaving it to the parent's event
 * converter, and why heliosview_host_ui_set_ime_caret exists at all.
 */

/* One UTF-16 code unit (or a buffered high surrogate) -> UTF-8. Returns the byte
 * count written, 0 when the unit was buffered as the high half of a pair. */
static int hv_host_utf8_from_unit(wchar_t unit, wchar_t* pending_high, char* out) {
    auto encode = [](uint32_t cp, char* dst) -> int {
        if (cp < 0x80) {
            dst[0] = static_cast<char>(cp);
            return 1;
        }
        if (cp < 0x800) {
            dst[0] = static_cast<char>(0xC0 | (cp >> 6));
            dst[1] = static_cast<char>(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp < 0x10000) {
            dst[0] = static_cast<char>(0xE0 | (cp >> 12));
            dst[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            dst[2] = static_cast<char>(0x80 | (cp & 0x3F));
            return 3;
        }
        dst[0] = static_cast<char>(0xF0 | (cp >> 18));
        dst[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    };

    if (*pending_high != 0) {
        const wchar_t high = *pending_high;
        *pending_high = 0;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            const uint32_t cp = 0x10000u + ((static_cast<uint32_t>(high) - 0xD800u) << 10)
                                          + (static_cast<uint32_t>(unit) - 0xDC00u);
            return encode(cp, out);
        }
        encode(0xFFFD, out); /* orphan high surrogate */
        return 0;            /* caller falls through for the unit that arrived instead */
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        *pending_high = unit;
        return 0; /* wait for the low half */
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF)
        return encode(0xFFFD, out); /* lone low surrogate */
    return encode(static_cast<uint32_t>(unit), out);
}

/* One committed character -> the focused widget.
 *
 * Two messages carry committed text and an IME may send either or both:
 *   - WM_CHAR, which TranslateMessage derives from a key press, and
 *   - WM_IME_CHAR, which an IME posts for the character it produced.
 * A duplicate guard keeps a character from landing twice when both arrive.
 */
static bool hv_host_accepts_char(wchar_t unit) {
    return unit >= 0x20 && unit != 0x7F;
}

static void hv_host_deliver_char(heliosview_host_t* host, wchar_t unit) {
    static thread_local wchar_t pending_high = 0;
    static thread_local wchar_t last_char = 0;
    static thread_local DWORD last_tick = 0;

    if (unit == last_char) {
        const DWORD now = GetTickCount();
        if (now - last_tick < 60) return; /* same character again within a tick: the
                                           * other channel for it already delivered */
        last_tick = now;
    } else {
        last_char = unit;
        last_tick = GetTickCount();
    }

    char utf8[8] = {};
    const int len = hv_host_utf8_from_unit(unit, &pending_high, utf8);
    if (len > 0) {
        utf8[len] = '\0';
        heliosview_host_ui_dispatch_text(host, utf8);
        return;
    }
    if (pending_high == 0 && unit >= 0xDC00 && unit <= 0xDFFF) {
        /* lone low surrogate: report a replacement character */
        heliosview_host_ui_dispatch_text(host, "\xEF\xBF\xBD");
    }
}

/* Pin the IME composition window and candidate list to the caret: the app reports the
 * caret in host client coordinates. */
static void hv_host_place_ime(HWND hwnd, int x, int y, int height) {
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return;

    if (height <= 0) height = 18;
    const int base = (y >= 0 && height > 0) ? y + height : 0;

    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = x;
    cf.ptCurrentPos.y = base;
    ImmSetCompositionWindow(imc, &cf);

    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_EXCLUDE; /* the candidate list may sit over the caret column */
    cand.ptCurrentPos.x = x;
    cand.ptCurrentPos.y = base;
    cand.rcArea.left = x;
    cand.rcArea.top = 0;
    cand.rcArea.right = x;
    cand.rcArea.bottom = base;
    ImmSetCandidateWindow(imc, &cand);

    ImmReleaseContext(hwnd, imc);
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

    /* ---- text input (this window is the focused one, see the note above) ---- */
    case WM_CHAR: {
        const wchar_t unit = static_cast<wchar_t>(wp);
        if (!hv_host_accepts_char(unit))
            return 0; /* backspace/tab/CR/LF/ESC are key events, not text */
        hv_host_deliver_char(host, unit);
        return 0;
    }

    /* ---- IME: composition text and its window position ---- */

    /* An IME associates itself with a window through this message: answering it is what
     * keeps the IME locked to this window.
     *
     * ISC_SHOWUICOMPOSITIONWINDOW is cleared because the widget draws the pre-edit
     * string itself, underlined, at the caret -- letting the IME draw its own
     * composition window as well would show the pinyin twice.
     *
     * ISC_SHOWUICANDIDATEWINDOW is deliberately left alone: the candidate list is the
     * IME's own UI, the user picks from it, and hiding it makes the IME look broken.
     * Its position comes from heliosview_host_ui_set_ime_caret (ImmSetCandidateWindow). */
    case WM_IME_SETCONTEXT:
        ime_log("WM_IME_SETCONTEXT active=%d", (int)wp);
        if (wp) {
            lp &= ~ISC_SHOWUICOMPOSITIONWINDOW;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);

    case WM_IME_STARTCOMPOSITION:
        ime_log("WM_IME_STARTCOMPOSITION");
        ui->ime_caret_set = true;
        return 0;

    case WM_IME_COMPOSITION: {
        ime_log("WM_IME_COMPOSITION flags=0x%04X", (unsigned)lp);

        HIMC imc = ImmGetContext(hwnd);
        if (!imc)
            return 0;

        /* The result string is the text the IME committed. It must be taken from the
         * context BEFORE the composition ends, which is why it is read here rather than
         * waiting for a WM_CHAR/WM_IME_CHAR that some IMEs do not send. */
        bool committed = false;
        if ((lp & GCS_RESULTSTR) != 0) {
            const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
            if (bytes > 0) {
                std::wstring wide(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                ImmGetCompositionStringW(imc, GCS_RESULTSTR, wide.data(), static_cast<DWORD>(bytes));
                for (wchar_t unit : wide) {
                    hv_host_deliver_char(host, unit); /* surrogate pairs pair up inside */
                }
                committed = true;
            }
        }

        /* The composition (pre-edit) string, previewed underlined. A message that
         * committed text does not also preview: the widget already inserted the commit,
         * and leaving the pre-edit behind would make the NEXT commit replace it. */
        if (committed) {
            /* Drop any preview still held from before this commit */
            heliosview_host_ui_dispatch_composition(host, "");
        } else if ((lp & GCS_COMPSTR) != 0) {
            const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
            if (bytes >= 0) {
                std::wstring wide(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                if (bytes > 0)
                    ImmGetCompositionStringW(imc, GCS_COMPSTR, wide.data(), static_cast<DWORD>(bytes));

                std::string utf8;
                utf8.reserve(wide.size() * 3 + 1);
                for (size_t i = 0; i < wide.size(); ++i) {
                    char buf[8] = {};
                    wchar_t high = 0;
                    int n = hv_host_utf8_from_unit(wide[i], &high, buf);
                    if (n == 0 && high != 0 && i + 1 < wide.size()) {
                        n = hv_host_utf8_from_unit(wide[++i], &high, buf);
                    }
                    if (n > 0) utf8.append(buf, static_cast<size_t>(n));
                }
                heliosview_host_ui_dispatch_composition(host, utf8.c_str());
            }
        }

        ImmReleaseContext(hwnd, imc);
        return 0;
    }

    case WM_IME_ENDCOMPOSITION:
        ime_log("WM_IME_ENDCOMPOSITION");
        /* Composition over: drop the preview. Any committed text was already taken from
         * the context in WM_IME_COMPOSITION. */
        heliosview_host_ui_dispatch_composition(host, "");
        ui->ime_caret_set = false;
        return 0;

    case WM_IME_CHAR:
        /* The character the IME produced. Some IMEs send this instead of WM_CHAR, so
         * deliver it rather than passing it along (which would only loop back as
         * WM_CHAR and land twice). */
        ime_log("WM_IME_CHAR 0x%04X", (unsigned)wp);
        if (hv_host_accepts_char(static_cast<wchar_t>(wp)))
            hv_host_deliver_char(host, static_cast<wchar_t>(wp));
        return 0;

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
    if (!host || !host->hwnd)
        return;
    hv_host_place_ime(host->hwnd, x, y, height);
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
