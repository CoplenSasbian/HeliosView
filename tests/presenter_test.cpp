/**
 * Test & verification suite for PixelView & BufferPresenter.
 */

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/Canvas.h>
#include <HeliosViewCore/PixelView.h>
#include <HeliosViewCore/BufferPresenter.h>
#include <HeliosViewCore/Window.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            std::cout << "  ok   " << msg << "\n"; \
            g_passed++; \
        } else { \
            std::cout << "  FAIL " << msg << "\n"; \
            g_failed++; \
        } \
    } while (0)

int main()
{
    std::cout << "HeliosView -- PixelView & BufferPresenter test suite\n\n";

    // 1. PixelView unit tests (zero-dependency on canvas/window)
    std::cout << "[PixelView]\n";
    {
        helios::PixelView emptyView;
        CHECK(emptyView.empty(), "default PixelView is empty");
        CHECK(!emptyView, "empty PixelView evaluates to false");
        CHECK(emptyView.bytesPerPixel() == 4, "default bytesPerPixel is 4");

        std::vector<uint32_t> buffer(100 * 50, 0xFF112233);
        helios::PixelView view(buffer.data(), 100, 50, 100 * 4, helios::PixelFormat::Bgra8Premul);
        CHECK(!view.empty(), "constructed PixelView is not empty");
        CHECK(view.width() == 100, "width is 100");
        CHECK(view.height() == 50, "height is 50");
        CHECK(view.stride() == 400, "stride is 400");
        CHECK(view.format() == helios::PixelFormat::Bgra8Premul, "format is Bgra8Premul");
        CHECK(view.row(0) == reinterpret_cast<const uint8_t*>(buffer.data()), "row(0) matches buffer pointer");
        CHECK(view.row(1) == reinterpret_cast<const uint8_t*>(buffer.data()) + 400, "row(1) matches offset by stride");

        // Subview
        auto sub = view.subview(10, 5, 20, 15);
        CHECK(!sub.empty(), "subview is valid");
        CHECK(sub.width() == 20, "subview width is 20");
        CHECK(sub.height() == 15, "subview height is 15");
        CHECK(sub.stride() == 400, "subview keeps parent stride");
        const uint8_t* expectedPtr = reinterpret_cast<const uint8_t*>(buffer.data()) + 5 * 400 + 10 * 4;
        CHECK(sub.data() == expectedPtr, "subview points to correct offset");

        // Invalid subview returns empty
        auto outOfBounds = view.subview(90, 40, 20, 20);
        CHECK(outOfBounds.empty(), "out-of-bounds subview is empty");
    }

    // 2. Canvas <-> PixelView integration
    std::cout << "\n[Canvas <-> PixelView]\n";
    {
        helios::Canvas canvas(64, 64);
        CHECK(canvas.valid(), "canvas creation succeeds");

        helios::PixelView pv = canvas.pixelView();
        CHECK(!pv.empty(), "canvas.pixelView() is not empty");
        CHECK(pv.width() == 64, "pv width matches canvas");
        CHECK(pv.height() == 64, "pv height matches canvas");
        CHECK(pv.stride() == canvas.stride(), "pv stride matches canvas");
        CHECK(pv.data() == canvas.data(), "pv data matches canvas data");

        // Test implicit conversion to PixelView
        auto takesPixelView = [](const helios::PixelView& v) {
            return v.width() == 64 && v.height() == 64;
        };
        CHECK(takesPixelView(canvas), "canvas implicitly converts to PixelView");
    }

    // 3. BufferPresenter & Window Bit-presentation tests
    std::cout << "\n[BufferPresenter]\n";
    {
        helios::Window win(320, 240, "Presenter Test Window");
        CHECK(win.nativeHandle() != nullptr, "window created successfully");

        HWND hwnd = reinterpret_cast<HWND>(win.id());
        CHECK(hwnd != nullptr && IsWindow(hwnd), "window HWND is valid");

        helios::BufferPresenter presenter(win);
        CHECK(presenter.isValid(), "BufferPresenter created and attached to window");

        bool resizeFired = false;
        int resizeW = 0, resizeH = 0;
        presenter.onResize([&](int w, int h) {
            resizeFired = true;
            resizeW = w;
            resizeH = h;
        });

        bool paintFired = false;
        presenter.onPaint([&]() {
            paintFired = true;
        });

        // Trigger WM_SIZE and verify callback
        SendMessage(hwnd, WM_SIZE, 0, MAKELPARAM(320, 240));
        CHECK(resizeFired, "onResize callback triggered by WM_SIZE");
        CHECK(resizeW == 320 && resizeH == 240, "onResize received matching dimensions");

        // Trigger WM_PAINT and verify callback
        SendMessage(hwnd, WM_PAINT, 0, 0);
        CHECK(paintFired, "onPaint callback triggered by WM_PAINT");

        // Create canvas and draw something
        helios::Canvas canvas(320, 240);
        canvas.fill(0xFF00FF00); // solid green

        // Present canvas to window (zero copy via PixelView)
        bool ok = presenter.present(canvas);
        CHECK(ok, "present(canvas) succeeds");

        // Present with offset
        bool okRect = presenter.present(canvas.pixelView().subview(10, 10, 100, 100), 20, 20);
        CHECK(okRect, "present(subview, 20, 20) succeeds");

        // Invalidate
        presenter.invalidate();
        CHECK(IsWindow(hwnd), "window remains valid after invalidate()");

        // Move semantics
        helios::BufferPresenter moved(std::move(presenter));
        CHECK(moved.isValid(), "moved presenter is valid");
        CHECK(!presenter.isValid(), "original presenter is now invalid");

        bool movedPresentOk = moved.present(canvas);
        CHECK(movedPresentOk, "moved presenter presents successfully");

#if defined(_WIN32)
        // 4. Pixel-level AlphaBlend verification with memory DC (§4 #2)
        std::cout << "\n[AlphaBlend & Memory DC Pixel Verification]\n";
        HDC memDC = CreateCompatibleDC(nullptr);
        CHECK(memDC != nullptr, "CreateCompatibleDC succeeded");

        if (memDC) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = 32;
            bmi.bmiHeader.biHeight = -32; // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            uint32_t* bits = nullptr;
            HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
            CHECK(hbm != nullptr && bits != nullptr, "CreateDIBSection succeeded for 32bpp memory surface");

            if (hbm && bits) {
                HGDIOBJ oldBm = SelectObject(memDC, hbm);

                // Pre-fill destination DC with opaque pure Red: B=0, G=0, R=255
                for (int i = 0; i < 32 * 32; ++i) {
                    bits[i] = 0x00FF0000;
                }

                // Create a 32x32 BGRA8_PREMUL buffer with 50% alpha green:
                // Alpha = 128 (0x80), Premultiplied Green = 128 (0x80), Red = 0, Blue = 0
                // Pixel uint32_t = (128 << 24) | (0 << 16) | (128 << 8) | 0 = 0x80008000
                std::vector<uint32_t> greenPremul(32 * 32, 0x80008000u);
                helios::PixelView greenView(greenPremul.data(), 32, 32, 32 * 4, helios::PixelFormat::Bgra8Premul);

                // Present to moved presenter, then render directly to memDC
                moved.present(greenView);
                bool renderOk = moved.renderToDC(memDC);
                CHECK(renderOk, "renderToDC succeeds on memory DC");

                // Sample blended pixel at (16, 16)
                // With AC_SRC_OVER + AC_SRC_ALPHA:
                // Dst.R = Src.R + (1 - A/255) * Dst.R = 0 + (127/255) * 255 ≈ 127
                // Dst.G = Src.G + (1 - A/255) * Dst.G = 128 + 0 = 128
                // Dst.B = 0
                // (If SetDIBitsToDevice were incorrectly used, R would be 0, G would be 128).
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&bits[16 * 32 + 16]);
                const uint8_t b = p[0];
                const uint8_t g = p[1];
                const uint8_t r = p[2];

                CHECK(std::abs(static_cast<int>(r) - 127) <= 2, "AlphaBlend preserved background red under 50% alpha");
                CHECK(std::abs(static_cast<int>(g) - 128) <= 2, "AlphaBlend applied source green under 50% alpha");
                CHECK(b == 0, "blue channel is zero");

                // Clip rect test: specify a clip rectangle completely outside destination
                bits[0] = 0x00FF0000;
                moved.renderToDC(memDC, 100, 100, 200, 200);
                CHECK(bits[0] == 0x00FF0000, "disjoint clip_box avoids rendering to DC");

                SelectObject(memDC, oldBm);
                DeleteObject(hbm);
            }
            DeleteDC(memDC);
        }
#endif

        win.close();
        CHECK(!win.nativeHandle() || !IsWindow(hwnd), "window closed cleanly with presenter attached");
    }

    std::cout << "\nResult: " << g_passed << " checks, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
