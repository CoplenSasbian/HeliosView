// HeliosView example + self-test: the canvas / painter / image API, headless.
//
// This program needs no window at all: a canvas is memory. It exercises the whole
// drawing surface -- creation, formats, pixel access, shapes, paths, transforms,
// clipping, text and images -- asserts the results at the pixel level, saves what it
// drew as PNG files, loads them back and composites them onto a second canvas.
//
// It is both the effect preview (open examples/out/*.png afterwards) and the
// regression test for the canvas layer: the asserts run with no display, no message
// loop and no window, which is exactly the property the canvas abstraction is for.
//
// It goes through the C++ wrapper (<HeliosViewCore/Canvas.h>) rather than the C API,
// so it doubles as that wrapper's test: every check below is written the way an
// application would write it (owning objects, bool results, one scope per painter),
// and the C API stays the ABI underneath.
//
// Usage:
//   HeliosViewCanvasTest            run the checks and write examples/out/*.png
//   HeliosViewCanvasTest --engine=gdi|gdi+|native|auto|d2d
//                                   build the canvases on a specific canvas engine
//
// Exit code 0 = every check passed, 1 = at least one failed.

#include <HeliosViewCore/Canvas.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using helios::Canvas;
using helios::FontDesc;
using helios::FontFlag;
using helios::LineCap;
using helios::LineJoin;
using helios::Matrix;
using helios::PaintEngine;
using helios::PaintFeature;
using helios::Painter;
using helios::PainterState;
using helios::Path;
using helios::PathWinding;
using helios::PixelFormat;
using helios::Rect;
using helios::TextAlign;
using helios::TextMetrics;

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

/* The pixel at (x, y) as straight ARGB, or the 0xDEADBEEF sentinel when the read
 * fails -- a wrong coordinate shows up as a loud mismatch instead of a silent zero. */
uint32_t pixel(Canvas& canvas, int32_t x, int32_t y)
{
    const std::optional<uint32_t> argb = canvas.getPixel(x, y);
    return argb ? *argb : 0xDEADBEEFu;
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

PaintEngine engine_from_name(const std::string& name)
{
    if (name.empty() || name == "auto")
        return PaintEngine::Auto;
    if (name == "builtin")
        return PaintEngine::Builtin;
    if (name == "native")
        return PaintEngine::Native;
    if (name == "accelerated")
        return PaintEngine::Accelerated;
    if (name == "software")
        return PaintEngine::Software;
    if (name == "gdi")
        return PaintEngine::Gdi;
    if (name == "gdi+" || name == "gdiplus")
        return PaintEngine::GdiPlus;
    if (name == "d2d" || name == "direct2d")
        return PaintEngine::D2D;
    if (name == "blend2d")
        return PaintEngine::Blend2D;
    std::printf("unknown engine '%s'\n", name.c_str());
    return PaintEngine::Auto;
}

void describe_engines()
{
    const std::vector<PaintEngine> available = helios::engines();
    std::printf("canvas engines: %d registered\n", static_cast<int>(available.size()));
    for (size_t i = 0; i < available.size(); ++i) {
        const PaintEngine id = available[i];
        std::printf("  [%d] %-14s compiled=%d probe=%d\n", static_cast<int>(i), helios::engineName(id).c_str(),
                    helios::engineCompiled(id) ? 1 : 0, helios::engineProbe(id) ? 1 : 0);
    }
    /* The concept names answer for whatever this platform provides: Builtin is the
     * consistent cross-platform engine, Native is the OS native renderer. */
    std::printf("  concept: builtin=%d native=%d (legacy: accelerated=%d software=%d)\n",
                helios::engineProbe(PaintEngine::Builtin) ? 1 : 0,
                helios::engineProbe(PaintEngine::Native) ? 1 : 0,
                helios::engineProbe(PaintEngine::Accelerated) ? 1 : 0,
                helios::engineProbe(PaintEngine::Software) ? 1 : 0);
    std::printf("  features of the resolved engine: antialias=%d alpha=%d transform=%d text=%d "
                "image=%d direct_pixels=%d\n",
                helios::engineSupportsFeature(PaintEngine::Auto, PaintFeature::Antialias) ? 1 : 0,
                helios::engineSupportsFeature(PaintEngine::Auto, PaintFeature::AlphaBlend) ? 1 : 0,
                helios::engineSupportsFeature(PaintEngine::Auto, PaintFeature::Transform) ? 1 : 0,
                helios::engineSupportsFeature(PaintEngine::Auto, PaintFeature::Text) ? 1 : 0,
                helios::engineSupportsFeature(PaintEngine::Auto, PaintFeature::ImageDraw) ? 1 : 0,
                helios::engineSupportsFeature(PaintEngine::Auto, PaintFeature::DirectPixels) ? 1 : 0);
}

/* ---------- the checks ---------- */

void check_canvas_basics(PaintEngine engine)
{
    std::printf("\n[canvas]\n");
    Canvas canvas(64, 48, PixelFormat::Auto, engine);
    check(canvas.valid(), "canvas_create returns a canvas");
    if (!canvas.valid()) {
        std::printf("     last error: %s\n", helios::lastErrorDescription("canvas_create").c_str());
        return;
    }

    check(canvas.width() == 64 && canvas.height() == 48, "canvas_size reports 64x48");
    check(canvas.stride() >= 64 * 4, "stride is at least width * 4");
    check(canvas.format() != PixelFormat::Auto, "format resolved to a concrete one");
    check(canvas.engine() != PaintEngine::Auto, "engine resolved to a concrete one");
    std::printf("     format=%d engine=%s stride=%d\n", static_cast<int>(canvas.format()),
                helios::engineName(canvas.engine()).c_str(), canvas.stride());

    /* A fresh canvas is fully transparent (zero), and stays that way after a
     * format change: premultiplied storage must not carry garbage. */
    check(pixel(canvas, 0, 0) == 0 && pixel(canvas, 63, 47) == 0, "a new canvas is fully transparent");
    check(canvas.data() != nullptr, "pixels are directly addressable");

    /* Pixel access round trip, including the premultiply path. */
    check(canvas.setPixel(10, 10, 0xFF123456u), "set_pixel succeeds");
    check(color_near(pixel(canvas, 10, 10), 0xFF123456u, 1), "get_pixel returns what set_pixel wrote");
    check(canvas.setPixel(10, 10, 0x80FF0000u), "set_pixel accepts alpha");
    check(color_near(pixel(canvas, 10, 10), 0x80FF0000u, 2), "a 50% red survives the premultiply round trip");
    check(!canvas.setPixel(64, 0, 0xFFFFFFFFu), "out-of-range set_pixel fails");
    check(!canvas.getPixel(-1, 0).has_value(), "get_pixel validates its arguments");

    /* fill covers everything, corners included */
    check(canvas.fill(0xFF102030u), "canvas_fill succeeds");
    check(color_near(pixel(canvas, 0, 0), 0xFF102030u, 1) && color_near(pixel(canvas, 63, 47), 0xFF102030u, 1),
          "canvas_fill covers all four corners");

    /* resize reallocates and clears */
    check(canvas.resize(32, 32), "canvas_resize succeeds");
    check(canvas.width() == 32 && canvas.height() == 32, "the size follows the resize");
    check(pixel(canvas, 31, 31) == 0, "resize clears the content");
}

void check_clone_and_formats(PaintEngine engine)
{
    std::printf("\n[clone / formats]\n");
    Canvas source(16, 16, PixelFormat::Bgra8Premul, engine);
    if (!source.valid()) {
        check(false, "source canvas created");
        return;
    }
    source.fill(0xFF204060u);
    source.setPixel(5, 5, 0xFFFF00FFu);

    Canvas gray = source.clone(PixelFormat::Gray8);
    check(gray.valid(), "clone into GRAY8 succeeds");
    if (gray.valid()) {
        check(gray.format() == PixelFormat::Gray8, "the clone reports the requested format");
        check(gray.stride() >= 16, "a GRAY8 stride is at least the width");
        const uint32_t g = pixel(gray, 0, 0);
        check(((g >> 16) & 0xFF) == ((g >> 8) & 0xFF) && ((g >> 8) & 0xFF) == (g & 0xFF),
              "a GRAY8 pixel reads back as a neutral gray");
    }

    Canvas rgba = source.clone(PixelFormat::Rgba8);
    check(rgba.valid(), "clone into RGBA8 succeeds");
    if (rgba.valid())
        check(color_near(pixel(rgba, 5, 5), 0xFFFF00FFu, 2), "the clone kept the pixel values");
}

void check_drawing(PaintEngine engine, const std::string& out_dir)
{
    std::printf("\n[drawing]\n");
    const int32_t W = 640, H = 420;
    Canvas canvas(W, H, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas created for the drawing checks");
        return;
    }

    Painter p(canvas);
    check(p.valid(), "painter_begin succeeds");
    if (!p.valid())
        return;

    /* Background */
    p.clear(0xFF1E2430u);
    check(color_near(pixel(canvas, 2, 2), 0xFF1E2430u, 1), "painter_clear fills the canvas");

    /* A solid filled rectangle: the interior must be exactly the requested color. */
    p.fillRect(20, 20, 200, 120, 0xFF2D7FF9u);
    check(color_near(pixel(canvas, 120, 80), 0xFF2D7FF9u, 1), "fill_rect fills its interior");
    check(color_near(pixel(canvas, 10, 10), 0xFF1E2430u, 1), "fill_rect does not draw outside itself");

    /* Stroke only: a rectangle outline leaves its interior alone. */
    p.setStroke(0xFFFFC857u, 3.0f);
    p.drawRect(240, 20, 160, 120);
    check(color_near(pixel(canvas, 320, 80), 0xFF1E2430u, 1), "a stroked rect leaves its interior untouched");
    check(!color_near(pixel(canvas, 240, 20), 0xFF1E2430u, 12), "the outline is drawn on its edge");

    /* Filled + stroked ellipse with a distinct outline. */
    PainterState state;
    p.state(state);
    state.fillColor = 0xFF34C759u;
    state.strokeColor = 0xFF0B6B2Eu;
    state.strokeWidth = 4.0f;
    p.setState(state);
    p.drawEllipse(60, 180, 220, 140);
    check(color_near(pixel(canvas, 170, 250), 0xFF34C759u, 1), "the ellipse interior takes fill_color");
    check(!color_near(pixel(canvas, 170, 180), 0xFF34C759u, 40), "the ellipse outline is not the fill color");

    /* Rounded rectangle */
    p.setFill(0xFFAF52DEu);
    p.setStroke(0, 1.0f);
    p.drawRoundRect(330, 180, 240, 120, 24);
    check(color_near(pixel(canvas, 450, 240), 0xFFAF52DEu, 1), "draw_round_rect fills its center");
    check(color_near(pixel(canvas, 332, 182), 0xFF1E2430u, 30), "the rounded corner is empty at its tip");

    /* Polygon through the points array */
    const float triangle[6] = {120, 350, 220, 350, 170, 400};
    p.setFill(0xFFFF9500u);
    p.drawPolygon(triangle, 3);
    check(color_near(pixel(canvas, 170, 370), 0xFFFF9500u, 1), "draw_polygon fills the triangle");

    /* ---- paths ---- */
    Path path;
    check(path.valid(), "path_create succeeds");
    if (path.valid()) {
        /* A heart-ish shape built from curves, then filled and stroked. */
        path.moveTo(60, 80);
        path.cubicTo(60, 40, 140, 40, 140, 80);
        path.cubicTo(140, 120, 100, 140, 100, 160);
        path.cubicTo(100, 140, 60, 120, 60, 80);
        path.close();
        p.setFill(0xFFFF375Fu);
        p.setStroke(0xFFFFFFFFu, 2.0f);
        const uint32_t before = pixel(canvas, 100, 100);
        check(p.drawPath(path), "draw_path succeeds");
        check(pixel(canvas, 100, 100) != before, "draw_path changed the canvas");
    }

    /* ---- transforms ---- */
    p.save();
    p.translate(480.0f, 320.0f);
    p.rotate(0.35f);
    p.setFill(0xFF64D2FFu);
    p.setStroke(0, 1.0f);
    p.fillRect(-40, -20, 90, 40, 0xFF64D2FFu);
    check(color_near(pixel(canvas, 480, 320), 0xFF64D2FFu, 1), "a rotated fill lands at the transformed origin");
    p.restore();

    /* ---- clipping ---- */
    p.setClipRect(0, 0, 40, 40);
    p.fillRect(0, 0, 200, 200, 0xFFFF3B30u);
    check(color_near(pixel(canvas, 20, 20), 0xFFFF3B30u, 1), "clipped fill draws inside the clip");
    /* Sample well clear of the clip and of everything drawn earlier (the ellipse and
     * the heart path both pass near the middle of the canvas). */
    check(color_near(pixel(canvas, 620, 400), 0xFF1E2430u, 1), "clipped fill draws nothing outside the clip");
    p.resetClip();
    check(color_near(pixel(canvas, 620, 400), 0xFF1E2430u, 1), "reset_clip does not repaint");

    /* ---- text ---- */
    check(p.setFont({"Segoe UI", 28.0f, FontFlag::Bold}), "set_font succeeds");

    TextMetrics metrics;
    check(p.measureText("HeliosView canvas", metrics), "measure_text succeeds");
    check(metrics.width > 0.0f && metrics.height > 0.0f, "the measured text has a size");
    check(metrics.lineHeight > 0.0f, "the line height is positive");
    std::printf("     \"HeliosView canvas\" -> %.1f x %.1f, line height %.1f, ascent %.1f\n", metrics.width,
                metrics.height, metrics.lineHeight, metrics.ascent);

    p.setFill(0xFFF2F2F7u);
    const uint32_t text_before = pixel(canvas, 60 + static_cast<int32_t>(metrics.width / 2.0f),
                                       28 + static_cast<int32_t>(metrics.height / 2.0f));
    check(p.drawText("HeliosView canvas", 60, 28), "draw_text succeeds");
    const uint32_t text_after = pixel(canvas, 60 + static_cast<int32_t>(metrics.width / 2.0f),
                                      28 + static_cast<int32_t>(metrics.height / 2.0f));
    check(text_before != text_after, "draw_text wrote pixels");

    /* Center-aligned text in a box */
    p.drawTextEx("centered in a box", 320, 380, 300, 32, TextAlign::HCenter | TextAlign::VCenter);

    check(p.end(), "painter_end succeeds");

    /* Every canvas operation above went through one engine; save the result so the
     * effect can be looked at. */
    const std::string path_png = out_dir + "/canvas_shapes.png";
    check(canvas.save(path_png), "canvas_save writes a PNG");
    std::printf("     wrote %s\n", path_png.c_str());

    /* Encode into memory and decode it back: the byte-level round trip. */
    const std::vector<uint8_t> bytes = canvas.encode("png");
    check(!bytes.empty(), "canvas_encode returns bytes");
    if (!bytes.empty()) {
        Canvas reloaded = Canvas::decode(bytes.data(), bytes.size(), PixelFormat::Bgra8Premul, engine);
        check(reloaded.valid(), "load_memory decodes the encoded bytes");
        if (reloaded.valid()) {
            check(reloaded.width() == W && reloaded.height() == H, "the decoded canvas has the original size");
            /* Sample empty canvas away from every shape: the pixel checks above have
             * already proven the drawn colors, this proves the encode/decode path
             * carries them (and the alpha) through. */
            check(color_near(pixel(reloaded, 620, 400), 0xFF1E2430u, 2),
                  "a decoded pixel matches what was drawn");
        }
    }

    /* The two-phase encode: ask for the size without asking for the buffer. */
    const size_t size_only = canvas.encodedSize("png");
    check(size_only > 0 && size_only == bytes.size(),
          "asking only for the size gives the same byte count");
}

void check_images(PaintEngine engine, const std::string& out_dir)
{
    std::printf("\n[images]\n");

    /* Build a small "sprite sheet": three solid colored tiles in one canvas. No text
     * on top: the checks below sample exact colors, and a glyph edge is antialiased. */
    Canvas sheet(96, 32, PixelFormat::Bgra8Premul, engine);
    if (!sheet.valid()) {
        check(false, "sprite sheet canvas created");
        return;
    }
    sheet.fill(0x00000000u);
    const uint32_t colors[3] = {0xFFFF3B30u, 0xFF34C759u, 0xFF0A84FFu};
    {
        Painter p(sheet);
        for (int i = 0; i < 3; ++i)
            p.fillRect(static_cast<float>(i * 32), 0.0f, 32.0f, 32.0f, colors[i]);
    }
    const std::string sheet_path = out_dir + "/canvas_sheet.png";
    check(sheet.save(sheet_path), "the sprite sheet saves");
    std::printf("     wrote %s\n", sheet_path.c_str());

    /* A destination canvas, plus the sheet drawn as three sprites and scaled up. */
    Canvas canvas(320, 160, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "destination canvas created");
        return;
    }
    canvas.fill(0xFF101418u);

    {
        Painter p(canvas);
        for (int i = 0; i < 3; ++i) {
            const Rect source{static_cast<int32_t>(i * 32), 0, 32, 32};
            p.drawImage(sheet, static_cast<float>(20 + i * 100), 20.0f, 64.0f, 64.0f, &source, 1.0f);
        }
        /* The whole sheet, scaled and drawn with opacity. */
        p.drawImage(sheet, 20.0f, 100.0f, 288.0f, 48.0f, nullptr, 0.45f);
    }

    check(color_near(pixel(canvas, 50, 50), colors[0], 40), "the first sprite drew its red tile");
    check(color_near(pixel(canvas, 150, 50), colors[1], 40), "the second sprite drew its green tile");
    check(color_near(pixel(canvas, 250, 50), colors[2], 40), "the third sprite drew its blue tile");

    /* A 45% copy over the dark background must land between the two colors. */
    const uint32_t blended = pixel(canvas, 50, 124);
    const int blended_red = static_cast<int>((blended >> 16) & 0xFF);
    check(blended_red > 0x40 && blended_red < 0xFF, "an alpha-scaled draw_image blends with the background");

    const std::string composite_path = out_dir + "/canvas_images.png";
    check(canvas.save(composite_path), "the composite saves");
    std::printf("     wrote %s\n", composite_path.c_str());

    /* A canvas cannot draw itself. */
    {
        Painter p(canvas);
        check(!p.drawImage(canvas, 0, 0, 10, 10, nullptr, 1.0f),
              "drawing a canvas into itself is rejected");
    }
}

void check_lines_and_curves(PaintEngine engine)
{
    std::printf("\n[lines / arcs / polylines]\n");
    Canvas canvas(200, 120, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas created for line checks");
        return;
    }
    canvas.fill(0xFF101418u);
    Painter p(canvas);

    /* draw_line: a horizontal line across the middle. */
    p.setStroke(0xFFFF3B30u, 2.0f);
    p.setFill(0);
    check(p.drawLine(10, 20, 190, 20), "draw_line succeeds");
    check(color_near(pixel(canvas, 100, 20), 0xFFFF3B30u, 30), "the line drew its midpoint");
    check(color_near(pixel(canvas, 100, 40), 0xFF101418u, 1), "the line did not draw below itself");

    /* draw_arc: a quarter arc inside a box. */
    p.setStroke(0xFF34C759u, 3.0f);
    check(p.drawArc(20, 40, 60, 60, 0.0f, 90.0f), "draw_arc succeeds");
    /* The arc's midpoint (45 deg) sits at center + r/sqrt(2) on both axes. */
    const int32_t arc_x = 50 + static_cast<int32_t>(30.0 / 1.41421356);
    const int32_t arc_y = 70 + static_cast<int32_t>(30.0 / 1.41421356);
    check(!color_near(pixel(canvas, arc_x, arc_y), 0xFF101418u, 60), "the arc drew its curve");
    check(color_near(pixel(canvas, 50, 70), 0xFF101418u, 1), "the arc left the box center empty");

    /* draw_polyline, open: a zigzag. */
    const float zigzag[8] = {120, 100, 140, 60, 160, 100, 180, 60};
    p.setStroke(0xFF0A84FFu, 2.0f);
    check(p.drawPolyline(zigzag, 4), "draw_polyline succeeds");
    check(!color_near(pixel(canvas, 140, 62), 0xFF101418u, 60), "the polyline drew a vertex");
    /* open: nothing connects the last point back to the first */
    check(color_near(pixel(canvas, 150, 100), 0xFF101418u, 30), "an open polyline does not close");

    /* draw_polyline, closed: same points closed into a ring. */
    p.clear(0xFF101418u);
    check(p.drawPolyline(zigzag, 4, true), "a closed polyline succeeds");
    /* The closing edge runs from (180, 60) back to (120, 100); its midpoint is (150, 80). */
    check(!color_near(pixel(canvas, 150, 80), 0xFF101418u, 60), "a closed polyline connects the ends");
}

void check_path_verbs(PaintEngine engine)
{
    std::printf("\n[path verbs]\n");
    Canvas canvas(260, 120, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas created for path verb checks");
        return;
    }
    canvas.fill(0xFF101418u);
    Painter p(canvas);

    /* line_to / quad_to: a triangle with a curved base. */
    Path path;
    path.moveTo(10, 10);
    path.lineTo(60, 10);
    path.lineTo(35, 50);
    path.close();
    p.setFill(0xFFFF9500u);
    p.setStroke(0, 1.0f);
    check(p.fillPath(path, 0xFFFF9500u), "fill_path succeeds");
    check(color_near(pixel(canvas, 35, 20), 0xFFFF9500u, 1), "line_to triangle filled");

    path.reset();
    check(path.winding() == PathWinding::NonZero, "reset keeps the winding rule");
    path.moveTo(70, 50);
    path.quadTo(95, 10, 120, 50);
    check(p.strokePath(path, 0xFF64D2FFu, 2.0f), "stroke_path succeeds");
    /* The curve's apex (t = 0.5) is 0.25*start + 0.5*control + 0.25*end = (95, 30). */
    check(!color_near(pixel(canvas, 95, 30), 0xFF101418u, 60), "quad_to curve stroked");

    /* add_rect / add_round_rect / add_ellipse as path verbs. */
    path.reset();
    path.addRect(130, 10, 40, 30);
    check(p.fillPath(path, 0xFFAF52DEu), "fill_path of add_rect succeeds");
    check(color_near(pixel(canvas, 150, 25), 0xFFAF52DEu, 1), "add_rect filled");

    path.reset();
    path.addRoundRect(180, 10, 50, 30, 10);
    check(p.fillPath(path, 0xFF34C759u), "fill_path of add_round_rect succeeds");
    check(color_near(pixel(canvas, 205, 25), 0xFF34C759u, 1), "add_round_rect filled its center");
    check(color_near(pixel(canvas, 181, 11), 0xFF101418u, 30), "add_round_rect left the corner tip empty");

    path.reset();
    path.addEllipse(130, 60, 60, 40);
    check(p.fillPath(path, 0xFFFF375Fu), "fill_path of add_ellipse succeeds");
    check(color_near(pixel(canvas, 160, 80), 0xFFFF375Fu, 1), "add_ellipse filled");

    /* Winding rule: two nested rects, same direction. NONZERO fills the hole,
     * EVENODD punches it out. */
    path.reset();
    path.addRect(200, 60, 50, 50);
    path.addRect(215, 75, 20, 20);
    path.setWinding(PathWinding::NonZero);
    check(path.winding() == PathWinding::NonZero, "set_winding NONZERO sticks");
    check(p.fillPath(path, 0xFFFFFFFFu), "fill_path NONZERO succeeds");
    check(color_near(pixel(canvas, 225, 85), 0xFFFFFFFFu, 1), "NONZERO fills the nested hole");

    p.clear(0xFF101418u); /* clear the area for a clean second pass */
    path.setWinding(PathWinding::EvenOdd);
    check(path.winding() == PathWinding::EvenOdd, "set_winding EVENODD sticks");
    check(p.fillPath(path, 0xFFFFFFFFu), "fill_path EVENODD succeeds");
    check(color_near(pixel(canvas, 225, 85), 0xFF101418u, 1), "EVENODD punches the nested hole");
    check(color_near(pixel(canvas, 205, 65), 0xFFFFFFFFu, 1), "EVENODD still fills the ring");
}

void check_transforms(PaintEngine engine)
{
    std::printf("\n[transforms]\n");
    Canvas canvas(200, 200, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas created for transform checks");
        return;
    }
    canvas.fill(0xFF101418u);
    Painter p(canvas);
    p.setStroke(0, 1.0f);

    /* scale: a 10x10 rect scaled x4 lands at 40x40. */
    p.save();
    check(p.scale(4.0f, 4.0f), "scale succeeds");
    p.fillRect(5, 5, 10, 10, 0xFFFF3B30u);
    p.restore();
    check(color_near(pixel(canvas, 50, 50), 0xFFFF3B30u, 1), "a scaled fill lands at the scaled size");
    check(color_near(pixel(canvas, 19, 19), 0xFF101418u, 1), "the unscaled area stays empty");

    /* skew: a rect sheared along x. */
    p.save();
    check(p.skew(0.5f, 0.0f), "skew succeeds");
    p.fillRect(20, 100, 40, 20, 0xFF34C759u);
    p.restore();
    /* x' = x + 0.5*y: the top edge (y=100) shifts 50px right, bottom (y=120) 60px. */
    check(color_near(pixel(canvas, 20 + 50 + 20, 105), 0xFF34C759u, 1), "a skewed fill lands shifted");
    check(color_near(pixel(canvas, 25, 105), 0xFF101418u, 1), "the unshifted corner stays empty");

    /* set_transform / get_transform round trip. */
    const Matrix m{1.0f, 0.0f, 0.0f, 1.0f, 150.0f, 150.0f}; /* translate(150,150) */
    check(p.setTransform(m), "set_transform succeeds");
    Matrix out{};
    check(p.transform(out), "get_transform succeeds");
    check(std::fabs(out[4] - 150.0f) < 0.01f && std::fabs(out[5] - 150.0f) < 0.01f,
          "the transform round trips");
    p.fillRect(0, 0, 20, 20, 0xFF0A84FFu);
    check(color_near(pixel(canvas, 160, 160), 0xFF0A84FFu, 1), "a set_transform fill lands translated");

    /* reset_transform restores identity. */
    check(p.resetTransform(), "reset_transform succeeds");
    p.transform(out);
    check(std::fabs(out[0] - 1.0f) < 0.01f && std::fabs(out[4]) < 0.01f, "identity after reset_transform");
    p.fillRect(5, 180, 10, 10, 0xFFFF9500u);
    check(color_near(pixel(canvas, 10, 185), 0xFFFF9500u, 1), "drawing after reset is untransformed");
}

void check_clipping(PaintEngine engine)
{
    std::printf("\n[clipping]\n");
    Canvas canvas(120, 120, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas created for clip checks");
        return;
    }
    canvas.fill(0xFF101418u);
    Painter p(canvas);

    /* set_clip_path: clip to a circle, fill the whole canvas. */
    Path circle;
    circle.addEllipse(20, 20, 60, 60);
    check(p.setClipPath(circle), "set_clip_path succeeds");
    check(p.fillRect(0, 0, 120, 120, 0xFFFF3B30u), "the clipped fill succeeds");
    check(color_near(pixel(canvas, 50, 50), 0xFFFF3B30u, 1), "clip_path draws inside the path");
    check(color_near(pixel(canvas, 22, 22), 0xFF101418u, 30), "clip_path draws nothing outside the path");

    /* clip_bounds reports the current clip's bounding box. */
    Rect bounds;
    check(p.clipBounds(bounds), "clip_bounds succeeds");
    check(bounds.x <= 20 && bounds.y <= 20 && bounds.x + bounds.width >= 80 && bounds.y + bounds.height >= 80,
          "clip_bounds encloses the circle");

    /* intersect_clip_rect narrows the clip further. */
    check(p.intersectClipRect(50, 20, 60, 60), "intersect_clip_rect succeeds");
    p.clear(0xFF101418u);
    check(p.fillRect(0, 0, 120, 120, 0xFF34C759u), "the intersecting fill succeeds");
    check(color_near(pixel(canvas, 60, 50), 0xFF34C759u, 1), "the intersection draws inside both");
    check(color_near(pixel(canvas, 30, 50), 0xFF101418u, 1), "the intersection excludes the rect-only part");

    /* Clip geometry is in the transform's own space: the clip lands where the transform
     * puts it, and the drawing under the same transform meets it there. */
    const Matrix shift{1.0f, 0.0f, 0.0f, 1.0f, 40.0f, 40.0f}; /* translate(40,40) */
    check(p.setTransform(shift), "set_transform under a clip succeeds");
    check(p.setClipRect(0, 0, 40, 40), "set_clip_rect under a transform succeeds");
    p.clear(0xFF101418u);
    check(p.fillRect(-40, -40, 200, 200, 0xFFFF9500u), "the transformed fill succeeds");
    check(color_near(pixel(canvas, 60, 60), 0xFFFF9500u, 1), "a clip under a transform draws inside it");
    check(color_near(pixel(canvas, 20, 20), 0xFF101418u, 1), "a clip under a transform stops at its edge");
    p.resetTransform();
    p.resetClip();
}

void check_state_and_style(PaintEngine engine)
{
    std::printf("\n[state / style]\n");
    Canvas canvas(200, 120, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas created for state checks");
        return;
    }
    canvas.fill(0xFF101418u);
    Painter p(canvas);

    /* set_alpha: a 50% global alpha multiplies into the fill. The canvas here is
     * opaque, so the destination stays opaque and only the color blends -- and GDI+
     * blends in the gamma-corrected compositing space it is set up with
     * (CompositingQualityHighQuality), so that channel value is an engine detail.
     * The alpha itself is checked where it shows: on a canvas that starts transparent. */
    check(p.setAlpha(0.5f), "set_alpha succeeds");
    check(p.fillRect(10, 10, 40, 40, 0xFFFF0000u), "the half-alpha fill succeeds");
    const int half_red = static_cast<int>((pixel(canvas, 30, 30) >> 16) & 0xFF);
    check(half_red > 0x10 && half_red < 0xFF, "set_alpha blended the fill with the background");
    Canvas fresh(40, 40, PixelFormat::Bgra8Premul, engine);
    {
        Painter q(fresh);
        q.setAlpha(0.5f);
        q.fillRect(0, 0, 40, 40, 0xFFFF0000u);
    }
    check(color_near(pixel(fresh, 20, 20), 0x7FFF0000u, 2), "set_alpha halves the fill's alpha");
    check(p.setAlpha(1.0f), "set_alpha back to 1 succeeds");

    /* set_antialias off: edges become binary (no blended fringe). */
    check(p.setAntialias(false), "set_antialias off succeeds");
    p.setFill(0xFFFFFFFFu);
    p.setStroke(0, 1.0f);
    p.drawEllipse(60, 10, 40, 40);
    check(p.setAntialias(true), "set_antialias on succeeds");

    /* Line caps: a thick line with a round cap extends past its endpoint. */
    PainterState st;
    p.state(st);
    st.strokeColor = 0xFF0A84FFu;
    st.strokeWidth = 10.0f;
    st.fillColor = 0;
    st.lineCap = LineCap::Round;
    p.setState(st);
    check(p.drawLine(30, 80, 90, 80), "a round-capped line succeeds");
    check(!color_near(pixel(canvas, 26, 80), 0xFF101418u, 60), "a round cap extends past the endpoint");

    st.lineCap = LineCap::Butt;
    p.setState(st);
    p.clear(0xFF101418u); /* the round cap from above must not linger */
    check(p.drawLine(30, 80, 90, 80), "a butt-capped line succeeds");
    /* The stroke is centred on y = 80 and ends at x = 30, so x = 26 is past the end. */
    check(color_near(pixel(canvas, 26, 80), 0xFF101418u, 1), "a butt cap stops at the endpoint");

    /* Line joins: miter vs bevel on a sharp corner. */
    const float corner[6] = {130, 100, 150, 60, 170, 100};
    st.strokeWidth = 8.0f;
    st.lineCap = LineCap::Butt;
    st.lineJoin = LineJoin::Miter;
    p.setState(st);
    check(p.drawPolyline(corner, 3), "a miter-joined polyline succeeds");
    st.lineJoin = LineJoin::Bevel;
    p.setState(st);
    check(p.drawPolyline(corner, 3), "a bevel-joined polyline succeeds");

    /* Font styles: italic / underline / strikeout flags are accepted. */
    check(p.setFont({"Segoe UI", 16.0f, FontFlag::Italic | FontFlag::Underline | FontFlag::Strikeout}),
          "set_font with style flags succeeds");
    const std::optional<float> line_height = p.lineHeight();
    check(line_height.has_value() && *line_height > 0.0f, "line_height succeeds");
}

void check_blit_and_direct_write(PaintEngine engine)
{
    std::printf("\n[blit / direct write]\n");
    Canvas src(32, 32, PixelFormat::Bgra8Premul, engine);
    Canvas dst(64, 64, PixelFormat::Bgra8Premul, engine);
    if (!src.valid() || !dst.valid()) {
        check(false, "canvases created for blit checks");
        return;
    }
    src.fill(0xFFFF3B30u);
    dst.fill(0xFF101418u);

    /* blit the whole src into dst at (16, 16). */
    check(src.blitTo(dst, 16, 16), "canvas_blit succeeds");
    check(color_near(pixel(dst, 32, 32), 0xFFFF3B30u, 1), "the blit landed at the offset");
    check(color_near(pixel(dst, 8, 8), 0xFF101418u, 1), "the blit did not draw outside itself");

    /* blit a sub-rect with alpha: src (8,8)-(23,23) lands at dst (0,0)-(15,15). */
    dst.fill(0xFF101418u);
    const Rect sub{8, 8, 16, 16};
    check(src.blitTo(dst, 0, 0, &sub, 0.5f), "a sub-rect blit with alpha succeeds");
    const uint32_t blended = pixel(dst, 8, 8);
    const int red = static_cast<int>((blended >> 16) & 0xFF);
    check(red > 0x60 && red < 0xFF, "the alpha blit blended with the background");
    check(color_near(pixel(dst, 20, 20), 0xFF101418u, 1), "the sub-rect blit stayed inside the sub-rect");

    /* Direct pixel write through canvas_data + end_write. */
    void* data = src.data();
    check(data != nullptr, "canvas_data hands out the buffer");
    if (data != nullptr) {
        /* Write the first pixel raw (BGRA premul of opaque blue), then announce it. */
        auto* bytes = static_cast<unsigned char*>(data);
        bytes[0] = 0xFF; bytes[1] = 0x00; bytes[2] = 0x00; bytes[3] = 0xFF;
        check(src.endWrite(), "end_write succeeds");
        check(color_near(pixel(src, 0, 0), 0xFF0000FFu, 1), "the direct write is visible after end_write");
    }
}

void check_image_io(PaintEngine engine, const std::string& out_dir)
{
    std::printf("\n[image formats]\n");
    std::printf("     png read/write=%d/%d  jpeg=%d/%d  bmp=%d/%d  gif(read)=%d  webp(read)=%d\n",
                helios::formatSupported("png") ? 1 : 0, helios::formatSupported("png", true) ? 1 : 0,
                helios::formatSupported("jpeg") ? 1 : 0, helios::formatSupported("jpeg", true) ? 1 : 0,
                helios::formatSupported("bmp") ? 1 : 0, helios::formatSupported("bmp", true) ? 1 : 0,
                helios::formatSupported("gif") ? 1 : 0, helios::formatSupported("webp") ? 1 : 0);
    check(helios::formatSupported("png", true), "the codec writes PNG");
    check(helios::formatSupported("jpeg", true), "the codec writes JPEG");
    check(!helios::formatSupported("webp"), "the codec reports WebP as unsupported");

    Canvas canvas(120, 80, PixelFormat::Bgra8Premul, engine);
    if (!canvas.valid()) {
        check(false, "canvas for the format checks");
        return;
    }
    canvas.fill(0x00000000u); /* transparent, to test flattening */
    {
        Painter p(canvas);
        p.fillRect(20, 20, 80, 40, 0xFF00A0FFu);
    }

    /* PNG keeps alpha and the colors exactly. */
    const std::string png = out_dir + "/canvas_io.png";
    check(canvas.save(png), "save infers PNG from the extension");
    Canvas loaded = Canvas::load(png, PixelFormat::Bgra8Premul, engine);
    check(loaded.valid(), "the written PNG loads back");
    if (loaded.valid()) {
        check(color_near(pixel(loaded, 60, 40), 0xFF00A0FFu, 2), "the reloaded pixel keeps its color");
        check(((pixel(loaded, 5, 5) >> 24) & 0xFF) == 0, "PNG preserved transparency");
    }

    /* JPEG has no alpha: the transparent area must become the background color we
     * asked for, not black and not garbage. */
    const std::string jpeg = out_dir + "/canvas_io.jpg";
    check(canvas.save(jpeg, "jpeg", 95, 0xFFFF00FFu), "save writes JPEG with a flattening color");
    loaded = Canvas::load(jpeg, PixelFormat::Bgra8Premul, engine);
    check(loaded.valid(), "the written JPEG loads back");
    if (loaded.valid()) {
        const uint32_t corner = pixel(loaded, 3, 3);
        check(color_near(corner, 0xFFFF00FFu, 24), "JPEG flattened transparency onto the background color");
        check(color_near(pixel(loaded, 60, 40), 0xFF00A0FFu, 40), "JPEG kept the drawn color (lossy)");
    }

    /* A missing file and an unsupported format both fail with a recorded reason. */
    check(!Canvas::load(out_dir + "/does_not_exist.png").valid(), "loading a missing file fails");
    check(!canvas.save(out_dir + "/canvas_io.webp"), "saving an unsupported extension fails");
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("HeliosView %s -- canvas / painter / image checks\n", heliosview_version());
    std::printf("backend: %s\n", heliosview_backend_name());

    const std::string engine_name = engine_arg(argc, argv);
    const PaintEngine engine = engine_from_name(engine_name);
    if (!engine_name.empty())
        helios::setDefaultPaintEngine(engine);

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
    check_lines_and_curves(engine);
    check_path_verbs(engine);
    check_transforms(engine);
    check_clipping(engine);
    check_state_and_style(engine);
    check_blit_and_direct_write(engine);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
