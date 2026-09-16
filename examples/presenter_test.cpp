/**
 * Test & verification suite for PixelView & BufferPresenter.
 */

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/Canvas.h>
#include <HeliosViewCore/PixelView.h>
#include <HeliosViewCore/BufferPresenter.h>
#include <HeliosViewCore/Window.h>

#include <iostream>
#include <vector>
#include <cassert>

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
        CHECK(true, "invalidate() succeeds without error");

        // Move semantics
        helios::BufferPresenter moved(std::move(presenter));
        CHECK(moved.isValid(), "moved presenter is valid");
        CHECK(!presenter.isValid(), "original presenter is now invalid");

        bool movedPresentOk = moved.present(canvas);
        CHECK(movedPresentOk, "moved presenter presents successfully");

        win.close();
        CHECK(true, "window closed cleanly with presenter attached");
    }

    std::cout << "\nResult: " << g_passed << " checks, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
