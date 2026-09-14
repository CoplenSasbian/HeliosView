// HeliosView example + self-test: the canvas / painter / image API, headless.
//
// This program needs no window at all: a canvas is memory. It exercises the whole
// drawing surface -- creation, formats, pixel access, shapes, paths, transforms,
// clipping, text and images -- asserts the results at the pixel level, saves what it
// drew as PNG files, loads them back and composites them onto a second canvas.
//
// It is both the effect preview (open examples/out/*.png afterwards) and the
// regression test for the paint layer: the asserts run with no display, no message
// loop and no window, which is exactly the property the canvas abstraction is for.
//
// Usage:
//   HeliosViewCanvasTest            run the checks and write examples/out/*.png
//   HeliosViewCanvasTest --engine=gdi|gdi+|native|auto|d2d
//                                   build the canvases on a specific paint engine
//
// Exit code 0 = every check passed, 1 = at least one failed.

#include <HeliosView/heliosview.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h> /* _mkdir */
#else
#include <sys/stat.h> /* mkdir */
#endif
namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what)
{
    ++g_checks;
    if (condition) {
        std::printf("  ok   %s\n", what);
        return;
    }
    ++g_failures;
    std::printf("  FAIL %s\n", what);
}

/* Straight-ARGB comparison on the channels that matter. Antialiased edges blend, so
 * the checks below sample well inside a shape (where the color is exact) or compare
 * with a tolerance, never on a boundary pixel. */
bool color_near(uint32_t a, uint32_t b, int tolerance = 2)
{
    for (int shift = 0; shift <= 24; shift += 8) {
        const int ca = static_cast<int>((a >> shift) & 0xFF);
        const int cb = static_cast<int>((b >> shift) & 0xFF);
        if (std::abs(ca - cb) > tolerance)
            return false;
    }
    return true;
}

uint32_t pixel(const heliosview_canvas_t* canvas, int32_t x, int32_t y)
{
    uint32_t argb = 0;
    if (heliosview_canvas_get_pixel(canvas, x, y, &argb) != 0)
        return 0xDEADBEEFu;
    return argb;
}

std::string engine_arg(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* prefix = "--engine=";
        const size_t len = std::strlen(prefix);
        if (std::strncmp(arg, prefix, len) == 0)
            return arg + len;
    }
    return {};
}

heliosview_paint_engine_t engine_from_name(const std::string& name)
{
    if (name.empty() || name == "auto")
        return HELIOSVIEW_ENGINE_AUTO;
    if (name == "native")
        return HELIOSVIEW_ENGINE_NATIVE;
    if (name == "accelerated")
        return HELIOSVIEW_ENGINE_ACCELERATED;
    if (name == "software")
        return HELIOSVIEW_ENGINE_SOFTWARE;
    if (name == "gdi")
        return HELIOSVIEW_ENGINE_GDI;
    if (name == "gdi+" || name == "gdiplus")
        return HELIOSVIEW_ENGINE_GDI_PLUS;
    if (name == "d2d" || name == "direct2d")
        return HELIOSVIEW_ENGINE_D2D;
    std::printf("unknown engine '%s'\n", name.c_str());
    return HELIOSVIEW_ENGINE_AUTO;
}

void describe_engines()
{
    std::printf("paint engines: %d registered\n", heliosview_engine_count());
    for (int i = 0; i < heliosview_engine_count(); ++i) {
        const heliosview_paint_engine_t id = heliosview_engine_at(i);
        std::printf("  [%d] %-14s compiled=%d probe=%d\n", i, heliosview_engine_name(id),
                    heliosview_engine_compiled(id), heliosview_engine_probe(id));
    }
    /* The concept names answer for whatever this platform provides, so a portable
     * caller never has to know that "native" means GDI+ here. */
    std::printf("  concept: native=%d accelerated=%d software=%d\n",
                heliosview_engine_probe(HELIOSVIEW_ENGINE_NATIVE),
                heliosview_engine_probe(HELIOSVIEW_ENGINE_ACCELERATED),
                heliosview_engine_probe(HELIOSVIEW_ENGINE_SOFTWARE));
    std::printf("  features of the resolved engine: antialias=%d alpha=%d transform=%d text=%d "
                "image=%d direct_pixels=%d\n",
                heliosview_engine_supports_feature(HELIOSVIEW_ENGINE_AUTO, HELIOSVIEW_FEATURE_ANTIALIAS),
                heliosview_engine_supports_feature(HELIOSVIEW_ENGINE_AUTO, HELIOSVIEW_FEATURE_ALPHA_BLEND),
                heliosview_engine_supports_feature(HELIOSVIEW_ENGINE_AUTO, HELIOSVIEW_FEATURE_TRANSFORM),
                heliosview_engine_supports_feature(HELIOSVIEW_ENGINE_AUTO, HELIOSVIEW_FEATURE_TEXT),
                heliosview_engine_supports_feature(HELIOSVIEW_ENGINE_AUTO, HELIOSVIEW_FEATURE_IMAGE_DRAW),
                heliosview_engine_supports_feature(HELIOSVIEW_ENGINE_AUTO,
                                                   HELIOSVIEW_FEATURE_DIRECT_PIXELS));
}

/* ---------- the checks ---------- */

void check_canvas_basics(heliosview_paint_engine_t engine)
{
    std::printf("\n[canvas]\n");
    heliosview_canvas_t* canvas = heliosview_canvas_create(64, 48, HELIOSVIEW_FORMAT_AUTO, engine);
    check(canvas != nullptr, "canvas_create returns a canvas");
    if (!canvas) {
        char message[256] = {};
        heliosview_last_error_string(message, sizeof(message));
        std::printf("     last error: %s (code %d)\n", message, heliosview_last_error());
        return;
    }

    int32_t width = 0, height = 0;
    check(heliosview_canvas_size(canvas, &width, &height) == 0 && width == 64 && height == 48,
          "canvas_size reports 64x48");
    check(heliosview_canvas_stride(canvas) >= 64 * 4, "stride is at least width * 4");
    check(heliosview_canvas_format(canvas) != HELIOSVIEW_FORMAT_AUTO, "format resolved to a concrete one");
    check(heliosview_canvas_engine(canvas) != HELIOSVIEW_ENGINE_AUTO, "engine resolved to a concrete one");
    std::printf("     format=%d engine=%s stride=%d\n", static_cast<int>(heliosview_canvas_format(canvas)),
                heliosview_engine_name(heliosview_canvas_engine(canvas)), heliosview_canvas_stride(canvas));

    /* A fresh canvas is fully transparent (zero), and stays that way after a
     * format change: premultiplied storage must not carry garbage. */
    check(pixel(canvas, 0, 0) == 0 && pixel(canvas, 63, 47) == 0, "a new canvas is fully transparent");
    check(heliosview_canvas_data(canvas) != nullptr, "pixels are directly addressable");

    /* Pixel access round trip, including the premultiply path. */
    check(heliosview_canvas_set_pixel(canvas, 10, 10, 0xFF123456u) == 0, "set_pixel succeeds");
    check(color_near(pixel(canvas, 10, 10), 0xFF123456u, 1), "get_pixel returns what set_pixel wrote");
    check(heliosview_canvas_set_pixel(canvas, 10, 10, 0x80FF0000u) == 0, "set_pixel accepts alpha");
    check(color_near(pixel(canvas, 10, 10), 0x80FF0000u, 2), "a 50% red survives the premultiply round trip");
    check(heliosview_canvas_set_pixel(canvas, 64, 0, 0xFFFFFFFFu) != 0, "out-of-range set_pixel fails");
    check(heliosview_canvas_get_pixel(canvas, -1, 0, nullptr) != 0, "get_pixel validates its arguments");

    /* fill covers everything, corners included */
    check(heliosview_canvas_fill(canvas, 0xFF102030u) == 0, "canvas_fill succeeds");
    check(color_near(pixel(canvas, 0, 0), 0xFF102030u, 1) && color_near(pixel(canvas, 63, 47), 0xFF102030u, 1),
          "canvas_fill covers all four corners");

    /* resize reallocates and clears */
    check(heliosview_canvas_resize(canvas, 32, 32) == 0, "canvas_resize succeeds");
    check(heliosview_canvas_size(canvas, &width, &height) == 0 && width == 32 && height == 32,
          "the size follows the resize");
    check(pixel(canvas, 31, 31) == 0, "resize clears the content");

    heliosview_canvas_destroy(canvas);
}

void check_clone_and_formats(heliosview_paint_engine_t engine)
{
    std::printf("\n[clone / formats]\n");
    heliosview_canvas_t* source = heliosview_canvas_create(16, 16, HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    if (!source) {
        check(false, "source canvas created");
        return;
    }
    heliosview_canvas_fill(source, 0xFF204060u);
    heliosview_canvas_set_pixel(source, 5, 5, 0xFFFF00FFu);

    heliosview_canvas_t* gray = heliosview_canvas_clone(source, HELIOSVIEW_FORMAT_GRAY8, HELIOSVIEW_ENGINE_AUTO);
    check(gray != nullptr, "clone into GRAY8 succeeds");
    if (gray) {
        check(heliosview_canvas_format(gray) == HELIOSVIEW_FORMAT_GRAY8, "the clone reports the requested format");
        check(heliosview_canvas_stride(gray) >= 16, "a GRAY8 stride is at least the width");
        const uint32_t g = pixel(gray, 0, 0);
        check(((g >> 16) & 0xFF) == ((g >> 8) & 0xFF) && ((g >> 8) & 0xFF) == (g & 0xFF),
              "a GRAY8 pixel reads back as a neutral gray");
        heliosview_canvas_destroy(gray);
    }

    heliosview_canvas_t* rgba = heliosview_canvas_clone(source, HELIOSVIEW_FORMAT_RGBA8, HELIOSVIEW_ENGINE_AUTO);
    check(rgba != nullptr, "clone into RGBA8 succeeds");
    if (rgba) {
        check(color_near(pixel(rgba, 5, 5), 0xFFFF00FFu, 2), "the clone kept the pixel values");
        heliosview_canvas_destroy(rgba);
    }
    heliosview_canvas_destroy(source);
}

void check_drawing(heliosview_paint_engine_t engine, const std::string& out_dir)
{
    std::printf("\n[drawing]\n");
    const int32_t W = 640, H = 420;
    heliosview_canvas_t* canvas = heliosview_canvas_create(W, H, HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    if (!canvas) {
        check(false, "canvas created for the drawing checks");
        return;
    }

    heliosview_painter_t* p = heliosview_painter_begin(canvas);
    check(p != nullptr, "painter_begin succeeds");
    if (!p) {
        heliosview_canvas_destroy(canvas);
        return;
    }

    /* Background */
    heliosview_painter_clear(p, 0xFF1E2430u);
    check(color_near(pixel(canvas, 2, 2), 0xFF1E2430u, 1), "painter_clear fills the canvas");

    /* A solid filled rectangle: the interior must be exactly the requested color. */
    heliosview_painter_fill_rect(p, 20, 20, 200, 120, 0xFF2D7FF9u);
    check(color_near(pixel(canvas, 120, 80), 0xFF2D7FF9u, 1), "fill_rect fills its interior");
    check(color_near(pixel(canvas, 10, 10), 0xFF1E2430u, 1), "fill_rect does not paint outside itself");

    /* Stroke only: a rectangle outline leaves its interior alone. */
    heliosview_painter_set_stroke(p, 0xFFFFC857u, 3.0f);
    heliosview_painter_draw_rect(p, 240, 20, 160, 120);
    check(color_near(pixel(canvas, 320, 80), 0xFF1E2430u, 1), "a stroked rect leaves its interior untouched");
    check(!color_near(pixel(canvas, 240, 20), 0xFF1E2430u, 12), "the outline is drawn on its edge");

    /* Filled + stroked ellipse with a distinct outline. */
    heliosview_painter_state_t state{};
    heliosview_painter_get_state(p, &state);
    state.fill_color = 0xFF34C759u;
    state.stroke_color = 0xFF0B6B2Eu;
    state.stroke_width = 4.0f;
    heliosview_painter_set_state(p, &state);
    heliosview_painter_draw_ellipse(p, 60, 180, 220, 140);
    check(color_near(pixel(canvas, 170, 250), 0xFF34C759u, 1), "the ellipse interior takes fill_color");
    check(!color_near(pixel(canvas, 170, 180), 0xFF34C759u, 40), "the ellipse outline is not the fill color");

    /* Rounded rectangle */
    heliosview_painter_set_fill(p, 0xFFAF52DEu);
    heliosview_painter_set_stroke(p, 0, 1.0f);
    heliosview_painter_draw_round_rect(p, 330, 180, 240, 120, 24);
    check(color_near(pixel(canvas, 450, 240), 0xFFAF52DEu, 1), "draw_round_rect fills its center");
    check(color_near(pixel(canvas, 332, 182), 0xFF1E2430u, 30), "the rounded corner is empty at its tip");

    /* Polygon through the points array */
    const float triangle[6] = {120, 350, 220, 350, 170, 400};
    heliosview_painter_set_fill(p, 0xFFFF9500u);
    heliosview_painter_draw_polygon(p, triangle, 3);
    check(color_near(pixel(canvas, 170, 370), 0xFFFF9500u, 1), "draw_polygon fills the triangle");

    /* ---- paths ---- */
    heliosview_path_t* path = heliosview_path_create();
    check(path != nullptr, "path_create succeeds");
    if (path) {
        /* A heart-ish shape built from curves, then filled and stroked. */
        heliosview_path_move_to(path, 60, 80);
        heliosview_path_cubic_to(path, 60, 40, 140, 40, 140, 80);
        heliosview_path_cubic_to(path, 140, 120, 100, 140, 100, 160);
        heliosview_path_cubic_to(path, 100, 140, 60, 120, 60, 80);
        heliosview_path_close(path);
        heliosview_painter_set_fill(p, 0xFFFF375Fu);
        heliosview_painter_set_stroke(p, 0xFFFFFFFFu, 2.0f);
        const uint32_t before = pixel(canvas, 100, 100);
        check(heliosview_painter_draw_path(p, path) == 0, "draw_path succeeds");
        check(pixel(canvas, 100, 100) != before, "draw_path changed the canvas");
        heliosview_path_destroy(path);
    }

    /* ---- transforms ---- */
    heliosview_painter_save(p);
    heliosview_painter_translate(p, 480.0f, 320.0f);
    heliosview_painter_rotate(p, 0.35f);
    heliosview_painter_set_fill(p, 0xFF64D2FFu);
    heliosview_painter_set_stroke(p, 0, 1.0f);
    heliosview_painter_fill_rect(p, -40, -20, 90, 40, 0xFF64D2FFu);
    check(color_near(pixel(canvas, 480, 320), 0xFF64D2FFu, 1), "a rotated fill lands at the transformed origin");
    heliosview_painter_restore(p);

    /* ---- clipping ---- */
    heliosview_painter_set_clip_rect(p, 0, 0, 40, 40);
    heliosview_painter_fill_rect(p, 0, 0, 200, 200, 0xFFFF3B30u);
    check(color_near(pixel(canvas, 20, 20), 0xFFFF3B30u, 1), "clipped fill paints inside the clip");
    /* Sample well clear of the clip and of everything drawn earlier (the ellipse and
     * the heart path both pass near the middle of the canvas). */
    check(color_near(pixel(canvas, 620, 400), 0xFF1E2430u, 1), "clipped fill paints nothing outside the clip");
    heliosview_painter_reset_clip(p);
    check(color_near(pixel(canvas, 620, 400), 0xFF1E2430u, 1), "reset_clip does not repaint");

    /* ---- text ---- */
    heliosview_font_desc_t font{};
    font.family = "Segoe UI";
    font.size = 28.0f;
    font.flags = HELIOSVIEW_FONT_BOLD;
    check(heliosview_painter_set_font(p, &font) == 0, "set_font succeeds");

    heliosview_text_metrics_t metrics{};
    check(heliosview_painter_measure_text(p, "HeliosView canvas", &metrics) == 0, "measure_text succeeds");
    check(metrics.width > 0.0f && metrics.height > 0.0f, "the measured text has a size");
    check(metrics.line_height > 0.0f, "the line height is positive");
    std::printf("     \"HeliosView canvas\" -> %.1f x %.1f, line height %.1f, ascent %.1f\n", metrics.width,
                metrics.height, metrics.line_height, metrics.ascent);

    heliosview_painter_set_fill(p, 0xFFF2F2F7u);
    const uint32_t text_before = pixel(canvas, 60 + static_cast<int32_t>(metrics.width / 2.0f),
                                       28 + static_cast<int32_t>(metrics.height / 2.0f));
    check(heliosview_painter_draw_text(p, "HeliosView canvas", 60, 28) == 0, "draw_text succeeds");
    const uint32_t text_after = pixel(canvas, 60 + static_cast<int32_t>(metrics.width / 2.0f),
                                      28 + static_cast<int32_t>(metrics.height / 2.0f));
    check(text_before != text_after, "draw_text wrote pixels");

    /* Center-aligned text in a box */
    heliosview_painter_draw_text_ex(p, "centered in a box", 320, 380, 300, 32,
                                    HELIOSVIEW_ALIGN_HCENTER | HELIOSVIEW_ALIGN_VCENTER);

    check(heliosview_painter_end(p) == 0, "painter_end succeeds");

    /* Every paint operation above went through one engine; save the result so the
     * effect can be looked at. */
    const std::string path_png = out_dir + "/canvas_shapes.png";
    check(heliosview_canvas_save(canvas, path_png.c_str(), nullptr, 0, 0) == 0, "canvas_save writes a PNG");
    std::printf("     wrote %s\n", path_png.c_str());

    /* Encode into memory and decode it back: the byte-level round trip. */
    void* bytes = nullptr;
    size_t size = 0;
    const int encoded = heliosview_canvas_encode(canvas, "png", 0, 0, &bytes, &size);
    check(encoded > 0 && bytes != nullptr && size > 0, "canvas_encode returns bytes");
    if (encoded > 0 && bytes) {
        heliosview_canvas_t* reloaded =
            heliosview_canvas_load_memory(bytes, size, HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
        check(reloaded != nullptr, "load_memory decodes the encoded bytes");
        if (reloaded) {
            int32_t rw = 0, rh = 0;
            heliosview_canvas_size(reloaded, &rw, &rh);
            check(rw == W && rh == H, "the decoded canvas has the original size");
            /* Sample empty canvas away from every shape: the pixel checks above have
             * already proven the drawn colors, this proves the encode/decode path
             * carries them (and the alpha) through. */
            check(color_near(pixel(reloaded, 620, 400), 0xFF1E2430u, 2),
                  "a decoded pixel matches what was drawn");
            heliosview_canvas_destroy(reloaded);
        }
        heliosview_free(bytes);
    }

    /* The two-phase encode: ask for the size without asking for the buffer. */
    size_t size_only = 0;
    check(heliosview_canvas_encode(canvas, "png", 0, 0, nullptr, &size_only) > 0 && size_only == size,
          "asking only for the size gives the same byte count");

    heliosview_canvas_destroy(canvas);
}

void check_images(heliosview_paint_engine_t engine, const std::string& out_dir)
{
    std::printf("\n[images]\n");

    /* Build a small "sprite sheet": three solid colored tiles in one canvas. No text
     * on top: the checks below sample exact colors, and a glyph edge is antialiased. */
    heliosview_canvas_t* sheet = heliosview_canvas_create(96, 32, HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    if (!sheet) {
        check(false, "sprite sheet canvas created");
        return;
    }
    heliosview_canvas_fill(sheet, 0x00000000u);
    heliosview_painter_t* p = heliosview_painter_begin(sheet);
    const uint32_t colors[3] = {0xFFFF3B30u, 0xFF34C759u, 0xFF0A84FFu};
    for (int i = 0; i < 3; ++i)
        heliosview_painter_fill_rect(p, static_cast<float>(i * 32), 0.0f, 32.0f, 32.0f, colors[i]);
    heliosview_painter_end(p);
    const std::string sheet_path = out_dir + "/canvas_sheet.png";
    check(heliosview_canvas_save(sheet, sheet_path.c_str(), nullptr, 0, 0) == 0, "the sprite sheet saves");
    std::printf("     wrote %s\n", sheet_path.c_str());

    /* A destination canvas, plus the sheet drawn as three sprites and scaled up. */
    heliosview_canvas_t* canvas = heliosview_canvas_create(320, 160, HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    if (!canvas) {
        check(false, "destination canvas created");
        heliosview_canvas_destroy(sheet);
        return;
    }
    heliosview_canvas_fill(canvas, 0xFF101418u);

    p = heliosview_painter_begin(canvas);
    for (int i = 0; i < 3; ++i) {
        heliosview_rect_t source{static_cast<int32_t>(i * 32), 0, 32, 32};
        heliosview_painter_draw_image(p, sheet, static_cast<float>(20 + i * 100), 20.0f, 64.0f, 64.0f,
                                      &source, 1.0f);
    }
    /* The whole sheet, scaled and drawn with opacity. */
    heliosview_painter_draw_image(p, sheet, 20.0f, 100.0f, 288.0f, 48.0f, nullptr, 0.45f);
    heliosview_painter_end(p);

    check(color_near(pixel(canvas, 50, 50), colors[0], 40), "the first sprite drew its red tile");
    check(color_near(pixel(canvas, 150, 50), colors[1], 40), "the second sprite drew its green tile");
    check(color_near(pixel(canvas, 250, 50), colors[2], 40), "the third sprite drew its blue tile");

    /* A 45% copy over the dark background must land between the two colors. */
    const uint32_t blended = pixel(canvas, 50, 124);
    const int blended_red = static_cast<int>((blended >> 16) & 0xFF);
    check(blended_red > 0x40 && blended_red < 0xFF, "an alpha-scaled draw_image blends with the background");

    const std::string composite_path = out_dir + "/canvas_images.png";
    check(heliosview_canvas_save(canvas, composite_path.c_str(), nullptr, 0, 0) == 0, "the composite saves");
    std::printf("     wrote %s\n", composite_path.c_str());

    /* A canvas cannot draw itself. */
    p = heliosview_painter_begin(canvas);
    check(heliosview_painter_draw_image(p, canvas, 0, 0, 10, 10, nullptr, 1.0f) != 0,
          "drawing a canvas into itself is rejected");
    heliosview_painter_end(p);

    heliosview_canvas_destroy(canvas);
    heliosview_canvas_destroy(sheet);
}

void check_image_io(heliosview_paint_engine_t engine, const std::string& out_dir)
{
    std::printf("\n[image formats]\n");
    std::printf("     png read/write=%d/%d  jpeg=%d/%d  bmp=%d/%d  gif(read)=%d  webp(read)=%d\n",
                heliosview_format_supported("png", 0), heliosview_format_supported("png", 1),
                heliosview_format_supported("jpeg", 0), heliosview_format_supported("jpeg", 1),
                heliosview_format_supported("bmp", 0), heliosview_format_supported("bmp", 1),
                heliosview_format_supported("gif", 0), heliosview_format_supported("webp", 0));
    check(heliosview_format_supported("png", 1) == 1, "the codec writes PNG");
    check(heliosview_format_supported("jpeg", 1) == 1, "the codec writes JPEG");
    check(heliosview_format_supported("webp", 0) == 0, "the codec reports WebP as unsupported");

    heliosview_canvas_t* canvas = heliosview_canvas_create(120, 80, HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    if (!canvas) {
        check(false, "canvas for the format checks");
        return;
    }
    heliosview_canvas_fill(canvas, 0x00000000u); /* transparent, to test flattening */
    heliosview_painter_t* p = heliosview_painter_begin(canvas);
    heliosview_painter_fill_rect(p, 20, 20, 80, 40, 0xFF00A0FFu);
    heliosview_painter_end(p);

    /* PNG keeps alpha and the colors exactly. */
    const std::string png = out_dir + "/canvas_io.png";
    check(heliosview_canvas_save(canvas, png.c_str(), nullptr, 0, 0) == 0, "save infers PNG from the extension");
    heliosview_canvas_t* loaded = heliosview_canvas_load(png.c_str(), HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    check(loaded != nullptr, "the written PNG loads back");
    if (loaded) {
        check(color_near(pixel(loaded, 60, 40), 0xFF00A0FFu, 2), "the reloaded pixel keeps its color");
        check(((pixel(loaded, 5, 5) >> 24) & 0xFF) == 0, "PNG preserved transparency");
        heliosview_canvas_destroy(loaded);
    }

    /* JPEG has no alpha: the transparent area must become the background color we
     * asked for, not black and not garbage. */
    const std::string jpeg = out_dir + "/canvas_io.jpg";
    check(heliosview_canvas_save(canvas, jpeg.c_str(), "jpeg", 95, 0xFFFF00FFu) == 0,
          "save writes JPEG with a flattening color");
    loaded = heliosview_canvas_load(jpeg.c_str(), HELIOSVIEW_FORMAT_BGRA8_PREMUL, engine);
    check(loaded != nullptr, "the written JPEG loads back");
    if (loaded) {
        const uint32_t corner = pixel(loaded, 3, 3);
        check(color_near(corner, 0xFFFF00FFu, 24), "JPEG flattened transparency onto the background color");
        check(color_near(pixel(loaded, 60, 40), 0xFF00A0FFu, 40), "JPEG kept the drawn color (lossy)");
        heliosview_canvas_destroy(loaded);
    }

    /* A missing file and an unsupported format both fail with a recorded reason. */
    check(heliosview_canvas_load((out_dir + "/does_not_exist.png").c_str(), HELIOSVIEW_FORMAT_AUTO,
                                 HELIOSVIEW_ENGINE_AUTO) == nullptr,
          "loading a missing file fails");
    check(heliosview_canvas_save(canvas, (out_dir + "/canvas_io.webp").c_str(), nullptr, 0, 0) != 0,
          "saving an unsupported extension fails");
    heliosview_canvas_destroy(canvas);
}

} // namespace

int main(int argc, char** argv)
{
    std::printf("HeliosView %s -- canvas / painter / image checks\n", heliosview_version());
    std::printf("backend: %s\n", heliosview_backend_name());

    const std::string engine_name = engine_arg(argc, argv);
    const heliosview_paint_engine_t engine = engine_from_name(engine_name);
    if (!engine_name.empty())
        heliosview_set_default_paint_engine(engine);

    describe_engines();

    /* Where the preview images go. The path is relative to the working directory this
     * runs from, so running it from the repository root puts them in examples/out.
     * Windows has no mkdir -p, so create the intermediate directory too. */
    const std::string out_dir = "examples/out";
#if defined(_WIN32)
    _mkdir("examples");
    _mkdir(out_dir.c_str()); /* EEXIST is fine: the saves below report any real problem */
#else
    mkdir("examples", 0755);
    mkdir(out_dir.c_str(), 0755);
#endif

    check_canvas_basics(engine);
    check_clone_and_formats(engine);
    check_drawing(engine, out_dir);
    check_images(engine, out_dir);
    check_image_io(engine, out_dir);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
