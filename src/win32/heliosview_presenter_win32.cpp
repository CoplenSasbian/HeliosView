#include <HeliosView/heliosview_presenter.h>
#include "../heliosview_internal.h"
#include "heliosview_win32_internal.h"

#include <vector>
#include <cstring>
#include <algorithm>

namespace {

constexpr UINT_PTR kPresenterSubclassId = 0x48565052; // 'HVPR'

} // namespace

struct heliosview_buffer_presenter {
    heliosview_window_t* window = nullptr;
    HWND hwnd = NULL;

    heliosview_presenter_resize_cb resize_cb = nullptr;
    void* resize_user_data = nullptr;

    heliosview_presenter_paint_cb paint_cb = nullptr;
    void* paint_user_data = nullptr;

    // Retained frame buffer for instant redraws and expose handling
    std::vector<uint8_t> backbuffer;
    int32_t buffer_width = 0;
    int32_t buffer_height = 0;
    int32_t buffer_stride = 0;
    heliosview_pixel_format_t buffer_format = HELIOSVIEW_FORMAT_AUTO;
    int32_t dst_x = 0;
    int32_t dst_y = 0;
    bool has_frame = false;

    void copy_frame(const heliosview_pixel_view_t* view, int32_t x, int32_t y) {
        if (!view || !view->pixels || view->width <= 0 || view->height <= 0) {
            has_frame = false;
            return;
        }

        dst_x = x;
        dst_y = y;
        buffer_width = view->width;
        buffer_height = view->height;

        const int min_stride = view->width * 4; // Always convert/normalize to 32bpp BGRA for GDI
        const int src_stride = view->stride > 0 ? view->stride : min_stride;
        buffer_stride = (min_stride + 3) & ~3;

        const size_t total_bytes = static_cast<size_t>(buffer_stride) * buffer_height;
        backbuffer.resize(total_bytes);

        const uint8_t* src = static_cast<const uint8_t*>(view->pixels);
        uint8_t* dst = backbuffer.data();

        if (view->format == HELIOSVIEW_FORMAT_RGBA8) {
            // Swap R and B channels into BGRA for GDI 32bpp DIB
            for (int r = 0; r < view->height; ++r) {
                const uint8_t* row_src = src + static_cast<size_t>(r) * src_stride;
                uint8_t* row_dst = dst + static_cast<size_t>(r) * buffer_stride;
                for (int c = 0; c < view->width; ++c) {
                    row_dst[c * 4 + 0] = row_src[c * 4 + 2]; // B
                    row_dst[c * 4 + 1] = row_src[c * 4 + 1]; // G
                    row_dst[c * 4 + 2] = row_src[c * 4 + 0]; // R
                    row_dst[c * 4 + 3] = row_src[c * 4 + 3]; // A
                }
            }
            buffer_format = HELIOSVIEW_FORMAT_BGRA8;
        } else if (view->format == HELIOSVIEW_FORMAT_GRAY8) {
            const int gray_src_stride = view->stride > 0 ? view->stride : view->width;
            for (int r = 0; r < view->height; ++r) {
                const uint8_t* row_src = src + static_cast<size_t>(r) * gray_src_stride;
                uint8_t* row_dst = dst + static_cast<size_t>(r) * buffer_stride;
                for (int c = 0; c < view->width; ++c) {
                    const uint8_t g = row_src[c];
                    row_dst[c * 4 + 0] = g;
                    row_dst[c * 4 + 1] = g;
                    row_dst[c * 4 + 2] = g;
                    row_dst[c * 4 + 3] = 255;
                }
            }
            buffer_format = HELIOSVIEW_FORMAT_BGRA8;
        } else {
            // Native 32-bit BGRA (straight or premultiplied)
            for (int r = 0; r < view->height; ++r) {
                std::memcpy(dst + static_cast<size_t>(r) * buffer_stride,
                            src + static_cast<size_t>(r) * src_stride,
                            min_stride);
            }
            buffer_format = view->format;
        }

        has_frame = true;
    }

    void render_to_dc(HDC hdc, const RECT& clip_box) {
        if (!has_frame || backbuffer.empty() || buffer_width <= 0 || buffer_height <= 0) {
            return;
        }

        if (clip_box.right > clip_box.left && clip_box.bottom > clip_box.top) {
            RECT dst_rect{dst_x, dst_y, dst_x + buffer_width, dst_y + buffer_height};
            RECT intersect{};
            if (!IntersectRect(&intersect, &clip_box, &dst_rect))
                return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = buffer_width;
        bmi.bmiHeader.biHeight = -buffer_height; // Top-down

        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        if (buffer_format == HELIOSVIEW_FORMAT_BGRA8_PREMUL) {
            HDC mem_dc = CreateCompatibleDC(hdc);
            if (mem_dc) {
                void* bits = nullptr;
                HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (hbm && bits) {
                    std::memcpy(bits, backbuffer.data(), backbuffer.size());
                    HGDIOBJ old_bm = SelectObject(mem_dc, hbm);
                    BLENDFUNCTION bf{};
                    bf.BlendOp = AC_SRC_OVER;
                    bf.BlendFlags = 0;
                    bf.SourceConstantAlpha = 255;
                    bf.AlphaFormat = AC_SRC_ALPHA;
                    AlphaBlend(
                        hdc,
                        dst_x, dst_y,
                        buffer_width, buffer_height,
                        mem_dc,
                        0, 0,
                        buffer_width, buffer_height,
                        bf
                    );
                    SelectObject(mem_dc, old_bm);
                    DeleteObject(hbm);
                    DeleteDC(mem_dc);
                    return;
                }
                if (hbm) DeleteObject(hbm);
                DeleteDC(mem_dc);
            }
        }

        SetDIBitsToDevice(
            hdc,
            dst_x, dst_y,
            buffer_width, buffer_height,
            0, 0,
            0, buffer_height,
            backbuffer.data(),
            &bmi,
            DIB_RGB_COLORS
        );
    }
};

static LRESULT CALLBACK hv_presenter_subclass_proc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
    UINT_PTR /*id*/, DWORD_PTR ref_data)

{
    auto* p = reinterpret_cast<heliosview_buffer_presenter_t*>(ref_data);
    if (!p) {
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_ERASEBKGND:
        // Intercept background erase: we paint the full client area, preventing white flash
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            if (p->paint_cb) {
                p->paint_cb(p, p->paint_user_data);
            }
            p->render_to_dc(hdc, ps.rcPaint);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        break;

    case WM_SIZE: {
        const int32_t w = LOWORD(lparam);
        const int32_t h = HIWORD(lparam);
        if (p->resize_cb) {
            p->resize_cb(p, w, h, p->resize_user_data);
        }
        break;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, hv_presenter_subclass_proc, kPresenterSubclassId);
        p->hwnd = NULL;
        p->window = nullptr;
        break;

    default:
        break;
    }


    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

extern "C" {

heliosview_buffer_presenter_t* heliosview_buffer_presenter_create_for_hwnd(void* native_hwnd)
{
    HWND hwnd = static_cast<HWND>(native_hwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "invalid window HWND");
        return nullptr;
    }

    heliosview_buffer_presenter* presenter = nullptr;
    try {
        presenter = new heliosview_buffer_presenter();
    } catch (const std::bad_alloc&) {
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory creating buffer presenter");
        return nullptr;
    }
    presenter->window = nullptr;
    presenter->hwnd = hwnd;

    if (!SetWindowSubclass(hwnd, hv_presenter_subclass_proc, kPresenterSubclassId, reinterpret_cast<DWORD_PTR>(presenter))) {
        delete presenter;
        hv_fail_win32(GetLastError(), "SetWindowSubclass failed for buffer presenter");
        return nullptr;
    }

    return presenter;
}

heliosview_buffer_presenter_t* heliosview_buffer_presenter_create(heliosview_window_t* window)
{
    if (!window) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "window is NULL");
        return nullptr;
    }

    HWND hwnd = hv_window_hwnd(window);
    auto* presenter = heliosview_buffer_presenter_create_for_hwnd(hwnd);
    if (presenter) {
        presenter->window = window;
    }
    return presenter;
}

void heliosview_buffer_presenter_destroy(heliosview_buffer_presenter_t* presenter)
{
    if (!presenter) return;

    if (presenter->hwnd && IsWindow(presenter->hwnd)) {
        RemoveWindowSubclass(presenter->hwnd, hv_presenter_subclass_proc, kPresenterSubclassId);
        presenter->hwnd = NULL;
    }

    delete presenter;
}

int heliosview_buffer_presenter_present(heliosview_buffer_presenter_t* presenter,
                                      const heliosview_pixel_view_t* view)
{
    return heliosview_buffer_presenter_present_rect(presenter, view, 0, 0);
}

int heliosview_buffer_presenter_present_rect(heliosview_buffer_presenter_t* presenter,
                                           const heliosview_pixel_view_t* view,
                                           int32_t dst_x, int32_t dst_y)
{
    if (!presenter) {
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "presenter is NULL");
    }
    if (!presenter->hwnd || !IsWindow(presenter->hwnd)) {
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "presenter window is destroyed or invalid");
    }
    if (!view || !view->pixels || view->width <= 0 || view->height <= 0) {
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "invalid pixel view");
    }

    presenter->copy_frame(view, dst_x, dst_y);

    // Direct blit to client DC: immediate display without pumping redundant WM_PAINT events
    HDC hdc = GetDC(presenter->hwnd);
    if (hdc) {
        RECT rc{dst_x, dst_y, dst_x + view->width, dst_y + view->height};
        presenter->render_to_dc(hdc, rc);
        ReleaseDC(presenter->hwnd, hdc);
    }

    return HELIOSVIEW_SUCCESS;
}


int heliosview_buffer_presenter_invalidate(heliosview_buffer_presenter_t* presenter)
{
    if (!presenter || !presenter->hwnd || !IsWindow(presenter->hwnd)) {
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "presenter or window is invalid");
    }
    InvalidateRect(presenter->hwnd, nullptr, FALSE);
    UpdateWindow(presenter->hwnd);
    return HELIOSVIEW_SUCCESS;
}

void heliosview_buffer_presenter_set_resize_callback(heliosview_buffer_presenter_t* presenter,
                                                   heliosview_presenter_resize_cb cb,
                                                   void* user_data)
{
    if (!presenter) return;
    presenter->resize_cb = cb;
    presenter->resize_user_data = user_data;
}

void heliosview_buffer_presenter_set_paint_callback(heliosview_buffer_presenter_t* presenter,
                                                  heliosview_presenter_paint_cb cb,
                                                  void* user_data)
{
    if (!presenter) return;
    presenter->paint_cb = cb;
    presenter->paint_user_data = user_data;
}

heliosview_window_t* heliosview_buffer_presenter_get_window(const heliosview_buffer_presenter_t* presenter)
{
    return presenter ? presenter->window : nullptr;
}

int heliosview_buffer_presenter_render_to_dc(heliosview_buffer_presenter_t* presenter,
                                            void* hdc,
                                            int32_t clip_left, int32_t clip_top,
                                            int32_t clip_right, int32_t clip_bottom)
{
    if (!presenter) return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "presenter is NULL");
    if (!hdc) return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "hdc is NULL");
    RECT rc{clip_left, clip_top, clip_right, clip_bottom};
    presenter->render_to_dc(static_cast<HDC>(hdc), rc);
    return HELIOSVIEW_SUCCESS;
}

} // extern "C"
