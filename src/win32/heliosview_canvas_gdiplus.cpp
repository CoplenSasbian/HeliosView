// HeliosView -- win32 canvas engine: GDI+.
//
// This is one implementation of hv::canvas::Engine (see heliosview_canvas_internal.h):
// it turns painter calls into GDI+ calls writing into the canvas' own pixel buffer.
// It wraps the caller's memory in a Gdiplus::Bitmap (no copy), owns the pens,
// brushes, fonts and paths for the duration of a drawing session, and knows nothing
// about windows, image formats or the public API's error codes beyond what the
// interface demands.
//
// GDI+ is a Windows system component (gdiplus.dll, linked through gdiplus.lib), so
// this engine costs no redistribution -- the same role Core Graphics plays on macOS
// and Cairo on Linux. It brings what the classic GDI engine cannot: antialiasing,
// per-pixel alpha, arbitrary affine transforms, path filling and text metrics.
//
// Threading: GDI+ objects are not shared across threads. This engine is used by
// whichever thread began the painter; heliosview_canvas_internal.h documents the
// canvas contract (one thread at a time), and the GDI+ startup is the only
// process-wide, once-only piece of state here.

#include "../heliosview_canvas_internal.h"
#include "../win32/heliosview_win32_internal.h" /* utf8_to_wide + hv_fail_win32 */

#include <windows.h>
#include <objidl.h> /* IStream, for the GDI+ <-> memory paths */
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* Every GDI+ type is spelled Gdiplus::<name> rather than pulled in with
 * `using namespace Gdiplus`: hv::canvas has a Color and a Rect of its own, and an
 * unqualified lookup would silently pick the wrong one. */

namespace hv::canvas {
namespace {

/* ================= GDI+ lifetime =================
 *
 * GdiplusStartup/GdiplusShutdown are process-wide. The library never calls
 * GdiplusShutdown: a DLL that shuts GDI+ down can race a host that also uses it,
 * and the process teardown reclaims everything anyway. Startup happens on the first
 * real use, not from a static initializer (the loader must not run GDI+ during DLL
 * load). */
bool gdiplus_ready()
{
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        /* GdiplusInit.h is included inside the Gdiplus namespace, so the startup entry
         * points are Gdiplus:: names too. */
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        ready = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    });
    return ready;
}

/* ================= Colors ================= */

inline BYTE channel(uint32_t argb, int shift)
{
    return static_cast<BYTE>((argb >> shift) & 0xFF);
}

inline BYTE scale_alpha(BYTE alpha, float factor)
{
    const float value = std::clamp(static_cast<float>(alpha) * factor, 0.0f, 255.0f);
    return static_cast<BYTE>(value + 0.5f);
}

/* The public API's straight 0xAARRGGBB becomes the GDI+ color GDI+ expects, with the
 * painter's global alpha multiplied in. (HVColor is qualified because Gdiplus::Color
 * is pulled in by `using namespace Gdiplus`.) */
inline Gdiplus::Color to_color(uint32_t argb, float alpha_factor)
{
    const BYTE a = scale_alpha(channel(argb, 24), alpha_factor);
    return Gdiplus::Color(a, channel(argb, 16), channel(argb, 8), channel(argb, 0));
}

/* GDI+ works in float rectangles; a negative width/height means the shape is mirrored
 * about that corner, which normalized() flips back. */
inline Gdiplus::RectF to_rectf(const hv::canvas::Rect& r)
{
    return Gdiplus::RectF(r.x, r.y, r.w, r.h);
}

/* Gdiplus::RectF has no Normalize() (only Rect does); a negative size means "mirrored about
 * the origin corner", so flip it in place. */
inline Gdiplus::RectF normalized(const hv::canvas::Rect& r)
{
    Gdiplus::RectF rect = to_rectf(r);
    if (rect.Width < 0.0f) {
        rect.X += rect.Width;
        rect.Width = -rect.Width;
    }
    if (rect.Height < 0.0f) {
        rect.Y += rect.Height;
        rect.Height = -rect.Height;
    }
    return rect;
}

inline Gdiplus::RectF normalized(Gdiplus::RectF rect)
{
    if (rect.Width < 0.0f) {
        rect.X += rect.Width;
        rect.Width = -rect.Width;
    }
    if (rect.Height < 0.0f) {
        rect.Y += rect.Height;
        rect.Height = -rect.Height;
    }
    return rect;
}

/* ================= Path conversion =================
 *
 * The engine-neutral PathData (heliosview_canvas_internal.h) becomes a Gdiplus::GraphicsPath.
 * Whole-shape verbs map to the native Add* so an engine rasterizes them exactly like
 * the equivalent direct call. */
void build_graphics_path(const PathData& data, Gdiplus::GraphicsPath& out)
{
    out.Reset();
    out.SetFillMode(data.winding == HELIOSVIEW_WINDING_EVENODD ? Gdiplus::FillModeAlternate
                                                              : Gdiplus::FillModeWinding);
    /* GDI+ has no move-to verb and no explicit current point: a figure is started by
     * its first segment. `current` is where the next segment must begin, and `seeded`
     * says whether the open figure already holds that point. A figure that is not
     * seeded yet is seeded with a zero-length line at `current`, which GDI+ does not
     * draw; without it the first segment would be dropped or start from the origin. */
    bool has_current = false;
    bool seeded = false;
    Gdiplus::PointF current(0.0f, 0.0f);

    auto seed_figure = [&] {
        if (!seeded) {
            out.StartFigure();
            out.AddLine(current, current);
            seeded = true;
        }
    };

    for (const PathEntry& entry : data.entries) {
        switch (entry.verb) {
        case PathVerbStartNew:
            current = Gdiplus::PointF(entry.pts[0], entry.pts[1]);
            has_current = true;
            seeded = false;
            break;
        case PathVerbLineTo: {
            const Gdiplus::PointF to(entry.pts[0], entry.pts[1]);
            if (has_current) {
                seed_figure();
                out.AddLine(current, to);
            }
            current = to;
            has_current = true;
            break;
        }
        case PathVerbQuadTo: {
            const Gdiplus::PointF control(entry.pts[0], entry.pts[1]);
            const Gdiplus::PointF end(entry.pts[2], entry.pts[3]);
            if (!has_current)
                current = control; /* nothing to continue from: begin at the control point */
            seed_figure();
            const Gdiplus::PointF start = current;
            /* A quadratic Bezier as a cubic with the control points weighted 2/3. */
            const Gdiplus::PointF c1(start.X + (control.X - start.X) * 2.0f / 3.0f,
                            start.Y + (control.Y - start.Y) * 2.0f / 3.0f);
            const Gdiplus::PointF c2(end.X + (control.X - end.X) * 2.0f / 3.0f,
                            end.Y + (control.Y - end.Y) * 2.0f / 3.0f);
            out.AddBezier(start, c1, c2, end);
            current = end;
            has_current = true;
            break;
        }
        case PathVerbCubicTo: {
            if (!has_current)
                current = Gdiplus::PointF(entry.pts[0], entry.pts[1]);
            seed_figure();
            out.AddBezier(current, Gdiplus::PointF(entry.pts[0], entry.pts[1]), Gdiplus::PointF(entry.pts[2], entry.pts[3]),
                          Gdiplus::PointF(entry.pts[4], entry.pts[5]));
            current = Gdiplus::PointF(entry.pts[4], entry.pts[5]);
            has_current = true;
            break;
        }
        case PathVerbClose:
            out.CloseFigure();
            has_current = false;
            seeded = false;
            break;
        case PathVerbRect:
            out.AddRectangle(Gdiplus::RectF(entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3]));
            has_current = false;
            seeded = false;
            break;
        case PathVerbRoundRect: {
            /* GDI+ has no native rounded rectangle: four arcs plus the connecting
             * edges, which is what its own Gdiplus::GraphicsPath::AddRoundedRectangle does. */
            const Gdiplus::RectF rect = normalized(Gdiplus::RectF(entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3]));
            const float limit = (rect.Width < rect.Height ? rect.Width : rect.Height) / 2.0f;
            const float radius = std::clamp(entry.pts[4], 0.0f, limit);
            if (radius <= 0.0f) {
                out.AddRectangle(rect);
            } else {
                const float d = radius * 2.0f;
                out.StartFigure();
                out.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
                out.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
                out.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
                out.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
                out.CloseFigure();
            }
            has_current = false;
            seeded = false;
            break;
        }
        case PathVerbEllipse:
            out.AddEllipse(normalized(Gdiplus::RectF(entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3])));
            has_current = false;
            seeded = false;
            break;
        default:
            break;
        }
    }
}

/* ================= Canvas adapter ================= */

class CanvasAdapterImpl final : public CanvasAdapter {
public:
    CanvasAdapterImpl(void* pixels, int32_t width, int32_t height, int32_t stride,
                      Gdiplus::PixelFormat bitmap_format)
        : m_pixels(pixels), m_bitmap(nullptr)
    {
        if (m_pixels)
            m_bitmap.reset(new Gdiplus::Bitmap(width, height, stride, bitmap_format, static_cast<BYTE*>(m_pixels)));
        if (!m_bitmap || m_bitmap->GetLastStatus() != Gdiplus::Ok)
            m_bitmap.reset();
    }

    bool valid() const { return m_bitmap != nullptr; }
    Gdiplus::Bitmap* bitmap() const { return m_bitmap.get(); }

    bool direct_pixels() const override { return true; }
    void* native_bitmap() override { return m_bitmap.get(); }

private:
    void* m_pixels;
    std::unique_ptr<Gdiplus::Bitmap> m_bitmap;
};

/* The GDI+ pixel format a canvas format maps to. The two straight-alpha formats have
 * no GDI+ equivalent that a Gdiplus::Bitmap can wrap directly, so they are served as
 * premultiplied and converted by the core's canvas-to-canvas copy. */
Gdiplus::PixelFormat gdiplus_format(heliosview_pixel_format_t format)
{
    switch (format) {
    case HELIOSVIEW_FORMAT_GRAY8:
        return PixelFormat8bppIndexed;
    default:
        return PixelFormat32bppPARGB;
    }
}

/* ================= Drawing session ================= */

/* The device-space bounding box of a rectangle given in the transform's own space. */
inline Rect device_bounds(const Gdiplus::RectF& box, const float m[6])
{
    const float xs[4] = {box.X, box.GetRight(), box.X, box.GetRight()};
    const float ys[4] = {box.Y, box.Y, box.GetBottom(), box.GetBottom()};
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = xs[i] * m[0] + ys[i] * m[2] + m[4];
        const float y = ys[i] * m[3] + xs[i] * m[1] + m[5];
        if (i == 0) {
            left = right = x;
            top = bottom = y;
        } else {
            if (x < left)
                left = x;
            if (x > right)
                right = x;
            if (y < top)
                top = y;
            if (y > bottom)
                bottom = y;
        }
    }
    return Rect{left, top, right - left, bottom - top};
}

inline Rect intersect_rect(const Rect& a, const Rect& b)
{
    const float x = a.x > b.x ? a.x : b.x;
    const float y = a.y > b.y ? a.y : b.y;
    const float right = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    const float bottom = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (right <= x || bottom <= y)
        return Rect{x, y, 0.0f, 0.0f};
    return Rect{x, y, right - x, bottom - y};
}

class ContextImpl final : public Context {
public:
    ContextImpl(CanvasAdapterImpl& canvas, const CanvasData& data)
        : m_graphics(canvas.valid() ? canvas.bitmap() : nullptr), m_data(data)
    {
        if (canvas.valid()) {
            m_graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            m_graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            m_graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            m_graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
            m_graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        }
    }

    ~ContextImpl() override
    {
        if (m_graphics.GetLastStatus() == Gdiplus::Ok) {
            m_graphics.Flush(Gdiplus::FlushIntentionSync);
        }
    }

    bool valid() const { return m_graphics.GetLastStatus() == Gdiplus::Ok; }


    /* ---- state ---- */
    void set_state(const heliosview_painter_state_t& state) override
    {
        m_state = state;
        m_state_dirty = true;
        m_graphics.SetSmoothingMode(state.antialias ? Gdiplus::SmoothingModeAntiAlias : Gdiplus::SmoothingModeNone);
        m_graphics.SetTextRenderingHint(state.antialias ? Gdiplus::TextRenderingHintAntiAlias
                                                        : Gdiplus::TextRenderingHintSingleBitPerPixel);
    }

    void set_transform(const float m[6]) override
    {
        /* Graphics::SetTransform only takes a Matrix*, and Gdiplus::Matrix is not
         * assignable -- so the six floats are kept and the matrix is rebuilt where it
         * is needed (here and in reset_clip). */
        std::copy(m, m + 6, m_transform);
        apply_transform();
    }

    void set_clip_rect(const Rect& r) override
    {
        const Gdiplus::RectF rect = to_rectf(r);
        apply_clip(rect, Gdiplus::CombineModeReplace);
        m_clip_bounds = intersect_rect(canvas_bounds(), device_bounds(rect, m_transform));
    }

    void intersect_clip_rect(const Rect& r) override
    {
        const Gdiplus::RectF rect = to_rectf(r);
        apply_clip(rect, Gdiplus::CombineModeIntersect);
        m_clip_bounds = intersect_rect(m_clip_bounds, device_bounds(rect, m_transform));
    }

    void set_clip_path(const PathData& path) override
    {
        Gdiplus::GraphicsPath graphics_path;
        build_graphics_path(path, graphics_path);
        /* The clip is recorded in device space using whatever transform is in effect
         * when SetClip is called, so the painter's transform goes in first and the
         * geometry is passed in its own (untransformed) space. */
        apply_transform();
        m_graphics.SetClip(&graphics_path, Gdiplus::CombineModeReplace);
        Gdiplus::RectF box;
        graphics_path.GetBounds(&box);
        m_clip_bounds = intersect_rect(canvas_bounds(), device_bounds(box, m_transform));
    }

    void reset_clip() override
    {
        m_graphics.ResetClip();
        m_clip_bounds = canvas_bounds();
    }

    void clip_bounds(Rect& out) const override { out = m_clip_bounds; }

    void save() override
    {
        m_saved_gdi_states.push_back(SavedGdiState{m_graphics.Save(), m_clip_bounds});
    }

    void restore() override
    {
        if (!m_saved_gdi_states.empty()) {
            m_graphics.Restore(m_saved_gdi_states.back().gstate);
            m_clip_bounds = m_saved_gdi_states.back().clip_bounds;
            m_saved_gdi_states.pop_back();
        }
    }

    /* Pushes m_transform into the Graphics. A named local, because SetTransform takes
     * a pointer and a temporary would not bind. */
    void apply_transform()
    {
        Gdiplus::Matrix matrix(m_transform[0], m_transform[1], m_transform[2], m_transform[3],
                               m_transform[4], m_transform[5]);
        m_graphics.SetTransform(&matrix);
    }

    /* Installs a clip. Save/Restore must not be used around SetClip here: Restore puts
     * back the state as it was at Save, which drops the new clip again. */
    void apply_clip(const Gdiplus::RectF& rect, Gdiplus::CombineMode mode)
    {
        apply_transform();
        m_graphics.SetClip(rect, mode);
    }

    Rect canvas_bounds() const { return Rect{0.0f, 0.0f, static_cast<float>(m_data.width), static_cast<float>(m_data.height)}; }

    /* ---- shapes ---- */
    void clear(uint32_t argb) override
    {
        /* clear() ignores transform and clip by contract. */
        Gdiplus::GraphicsState state = m_graphics.Save();
        m_graphics.ResetTransform();
        m_graphics.ResetClip();
        m_graphics.Clear(to_color(argb, 1.0f));
        m_graphics.Restore(state);
    }

    void draw_line(float x1, float y1, float x2, float y2) override
    {
        if (Gdiplus::Pen* pen = current_pen())
            m_graphics.DrawLine(pen, x1, y1, x2, y2);
    }

    void draw_rect(const Rect& r) override
    {
        const Gdiplus::RectF rect = normalized(r);
        if (Gdiplus::SolidBrush* brush = current_brush())
            m_graphics.FillRectangle(brush, rect);
        if (Gdiplus::Pen* pen = current_pen())
            m_graphics.DrawRectangle(pen, rect);
    }

    void draw_round_rect(const Rect& r, float radius) override
    {
        Gdiplus::GraphicsPath path;
        PathData data;
        PathEntry entry;
        entry.verb = PathVerbRoundRect;
        entry.pts[0] = r.x;
        entry.pts[1] = r.y;
        entry.pts[2] = r.w;
        entry.pts[3] = r.h;
        entry.pts[4] = radius;
        data.entries.push_back(entry);
        build_graphics_path(data, path);
        if (Gdiplus::SolidBrush* brush = current_brush())
            m_graphics.FillPath(brush, &path);
        if (Gdiplus::Pen* pen = current_pen())
            m_graphics.DrawPath(pen, &path);
    }

    void draw_ellipse(const Rect& r) override
    {
        const Gdiplus::RectF rect = normalized(r);
        if (Gdiplus::SolidBrush* brush = current_brush())
            m_graphics.FillEllipse(brush, rect);
        if (Gdiplus::Pen* pen = current_pen())
            m_graphics.DrawEllipse(pen, rect);
    }

    void draw_arc(const Rect& r, float start_deg, float sweep_deg) override
    {
        const Gdiplus::RectF rect = normalized(r);
        if (Gdiplus::Pen* pen = current_pen())
            m_graphics.DrawArc(pen, rect, start_deg, sweep_deg);
    }

    void draw_polyline(const float* points, size_t count, bool closed) override
    {
        if (count < 2 || !points)
            return;
        std::vector<Gdiplus::PointF> pts;
        pts.reserve(count);
        for (size_t i = 0; i < count; ++i)
            pts.emplace_back(points[i * 2], points[i * 2 + 1]);
        if (Gdiplus::SolidBrush* brush = current_brush()) {
            if (closed)
                m_graphics.FillPolygon(brush, pts.data(), static_cast<INT>(count), Gdiplus::FillModeWinding);
        }
        if (Gdiplus::Pen* pen = current_pen()) {
            if (closed)
                m_graphics.DrawPolygon(pen, pts.data(), static_cast<INT>(count));
            else
                m_graphics.DrawLines(pen, pts.data(), static_cast<INT>(count));
        }
    }

    void draw_path(const PathData& path, bool fill, bool stroke) override
    {
        if (path.empty())
            return;
        Gdiplus::GraphicsPath graphics_path;
        build_graphics_path(path, graphics_path);
        if (fill) {
            if (Gdiplus::SolidBrush* brush = current_brush())
                m_graphics.FillPath(brush, &graphics_path);
        }
        if (stroke) {
            if (Gdiplus::Pen* pen = current_pen())
                m_graphics.DrawPath(pen, &graphics_path);
        }
    }

    /* ---- text ---- */
    void draw_text(const char* utf8, const Rect& box, uint32_t align) override
    {
        if (!utf8 || !*utf8)
            return;
        Gdiplus::Font* font = current_font();
        if (!font || !m_fill)
            return;

        const std::wstring wide = utf8_to_wide(utf8);
        const bool baseline = (align & HELIOSVIEW_ALIGN_BASELINE) != 0;
        const bool has_box = box.w > 0.0f || box.h > 0.0f;

        if (has_box) {
            Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoWrap);
            apply_alignment(format, align);
            Gdiplus::RectF layout(box.x, box.y, box.w, box.h);
            if (baseline) {
                /* The box's top plus the ascent is the baseline. */
                heliosview_text_metrics_t metrics{};
                measure_text(utf8, &metrics);
                layout.Y += metrics.ascent;
                layout.Height -= metrics.ascent;
            }
            m_graphics.DrawString(wide.c_str(), static_cast<INT>(wide.size()), font, layout, &format,
                                  m_fill.get());
        } else {
            Gdiplus::PointF origin(box.x, box.y);
            if (baseline) {
                heliosview_text_metrics_t metrics{};
                measure_text(utf8, &metrics);
                origin.Y += metrics.ascent;
            }
            m_graphics.DrawString(wide.c_str(), static_cast<INT>(wide.size()), font, origin, m_fill.get());
        }
    }

    void measure_text(const char* utf8, heliosview_text_metrics_t* out) override
    {
        if (!out)
            return;
        *out = heliosview_text_metrics_t{};
        Gdiplus::Font* font = current_font();
        if (!font)
            return;

        /* Line height (the recommended distance between lines) comes from the
         * family, not from the string: an empty string has no advance width but the
         * font still has a line box, which is what a multi-line caller advances by. */
        Gdiplus::FontFamily family;
        Gdiplus::REAL line_spacing = 0.0f;
        if (font->GetFamily(&family) == Gdiplus::Ok && family.GetLastStatus() == Gdiplus::Ok) {
            line_spacing = family.GetLineSpacing(Gdiplus::FontStyleRegular);
            if (line_spacing <= 0.0f)
                line_spacing = static_cast<Gdiplus::REAL>(font->GetSize()) * 1.2f;
            out->ascent = family.GetCellAscent(Gdiplus::FontStyleRegular) * font->GetSize() / family.GetEmHeight(Gdiplus::FontStyleRegular);
            out->descent = family.GetCellDescent(Gdiplus::FontStyleRegular) * font->GetSize() / family.GetEmHeight(Gdiplus::FontStyleRegular);
            out->line_height = line_spacing * font->GetSize() / family.GetEmHeight(Gdiplus::FontStyleRegular);
        }

        if (utf8 && *utf8) {
            const std::wstring wide = utf8_to_wide(utf8);
            /* A layout box far larger than any text, measured without wrapping: the
             * result is the string's own extent, not the box's. */
            Gdiplus::RectF layout(0.0f, 0.0f, 1.0e6f, 1.0e6f);
            Gdiplus::RectF bounds;
            if (m_graphics.MeasureString(wide.c_str(), static_cast<INT>(wide.size()), font, layout,
                                         &bounds) == Gdiplus::Ok) {
                out->width = bounds.Width;
                out->height = bounds.Height;
            }
        }
        if (out->height <= 0.0f) {
            out->height = out->ascent + out->descent;
            if (out->height <= 0.0f)
                out->height = static_cast<float>(font->GetSize()) * 1.2f;
        }
        if (out->line_height <= 0.0f)
            out->line_height = out->height;
    }

    /* ---- images ---- */
    void draw_image(CanvasAdapter* src_adapter, const CanvasData& src, const Rect& src_rect,
                    const Rect& dst_rect, float alpha) override
    {
        if (!src.pixels)
            return;
        Gdiplus::Bitmap* source = src_adapter ? static_cast<Gdiplus::Bitmap*>(src_adapter->native_bitmap()) : nullptr;
        std::unique_ptr<Gdiplus::Bitmap> wrapper;
        Gdiplus::PixelFormat format = gdiplus_format(src.format);
        if (!source) {
            wrapper.reset(new Gdiplus::Bitmap(src.width, src.height, src.stride, format,
                                     static_cast<BYTE*>(src.pixels)));
            if (!wrapper || wrapper->GetLastStatus() != Gdiplus::Ok)
                return;
            source = wrapper.get();
        }

        Gdiplus::RectF destination(dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h);
        if (alpha >= 1.0f) {
            m_graphics.DrawImage(source, destination, src_rect.x, src_rect.y, src_rect.w, src_rect.h,
                                 Gdiplus::UnitPixel);
            return;
        }
        /* A per-draw opacity: scaling the alpha row of the color matrix multiplies it
         * into every pixel. A zero-initialized matrix is the identity. */
        Gdiplus::ColorMatrix matrix = {};
        matrix.m[0][0] = 1.0f;
        matrix.m[1][1] = 1.0f;
        matrix.m[2][2] = 1.0f;
        matrix.m[3][3] = std::clamp(alpha, 0.0f, 1.0f);
        matrix.m[4][4] = 1.0f;
        Gdiplus::ImageAttributes attributes;
        attributes.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        m_graphics.DrawImage(source, destination, src_rect.x, src_rect.y, src_rect.w, src_rect.h,
                             Gdiplus::UnitPixel, &attributes);
    }

    void flush() override { m_graphics.Flush(Gdiplus::FlushIntentionSync); }

private:
    /* ---- state translation ---- */
    void apply_state()
    {
        if (!m_state_dirty)
            return;
        m_state_dirty = false;

        m_pen.reset();
        m_brush.reset();
        m_font.reset();
        m_family.reset();

        if (channel(m_state.stroke_color, 24) != 0) {
            auto pen = std::make_unique<Gdiplus::Pen>(to_color(m_state.stroke_color, m_state.alpha),
                                             m_state.stroke_width > 0.0f ? m_state.stroke_width : 1.0f);
            pen->SetLineCap(gdiplus_cap(m_state.line_cap), gdiplus_cap(m_state.line_cap),
                           Gdiplus::DashCapFlat);
            pen->SetLineJoin(gdiplus_join(m_state.line_join));
            if (pen->GetLastStatus() == Gdiplus::Ok)
                m_pen = std::move(pen);
        }
        if (channel(m_state.fill_color, 24) != 0) {
            auto brush = std::make_unique<Gdiplus::SolidBrush>(to_color(m_state.fill_color, m_state.alpha));
            if (brush->GetLastStatus() == Gdiplus::Ok)
                m_brush = std::move(brush);
        }
        /* The text color is the fill color, so keep a brush for it even while a
         * shape fill is not requested. Cheap: one Gdiplus::SolidBrush. */
        {
            auto text_brush = std::make_unique<Gdiplus::SolidBrush>(to_color(m_state.fill_color, m_state.alpha));
            if (text_brush->GetLastStatus() == Gdiplus::Ok)
                m_fill = std::move(text_brush);
        }
        font_from_state();
    }

    void font_from_state()
    {
        m_font.reset();
        m_family.reset();
        const float size = m_state.font.size > 0.0f ? m_state.font.size : 12.0f;
        const std::string family_name = m_state.font.family ? m_state.font.family : "";
        const std::wstring family_wide = family_name.empty() ? std::wstring() : utf8_to_wide(family_name);

        auto make_font = [&](const wchar_t* name) -> std::unique_ptr<Gdiplus::Font> {
            /* The Gdiplus::FontFamily constructor takes a family NAME, so the unknown-family
             * case is simply a failed status here -- no assignment to a Gdiplus::FontFamily
             * (which GDI+ keeps non-assignable). */
            auto family = std::make_unique<Gdiplus::FontFamily>(name);
            if (!family || family->GetLastStatus() != Gdiplus::Ok)
                return nullptr;
            auto font = std::make_unique<Gdiplus::Font>(family.get(), size, compute_style(), Gdiplus::UnitPixel);
            if (font->GetLastStatus() != Gdiplus::Ok)
                return nullptr;
            m_family = std::move(family); /* outlives the Gdiplus::Font, for the metrics path */
            return font;
        };

        /* An unknown family falls back to the platform's default sans-serif rather
         * than failing: the documented contract for heliosview_font_desc::family. */
        if (!family_wide.empty())
            m_font = make_font(family_wide.c_str());
        if (!m_font) {
            /* GenericSansSerif is GDI+'s own "system default UI font" family. */
            const Gdiplus::FontFamily* generic = Gdiplus::FontFamily::GenericSansSerif();
            if (generic && generic->GetLastStatus() == Gdiplus::Ok) {
                WCHAR name[LF_FACESIZE] = {};
                if (generic->GetFamilyName(name) == Gdiplus::Ok)
                    m_font = make_font(name);
            }
        }
    }

    INT compute_style() const
    {
        INT style = Gdiplus::FontStyleRegular;
        if (m_state.font.flags & HELIOSVIEW_FONT_BOLD)
            style |= Gdiplus::FontStyleBold;
        if (m_state.font.flags & HELIOSVIEW_FONT_ITALIC)
            style |= Gdiplus::FontStyleItalic;
        if (m_state.font.flags & HELIOSVIEW_FONT_UNDERLINE)
            style |= Gdiplus::FontStyleUnderline;
        if (m_state.font.flags & HELIOSVIEW_FONT_STRIKEOUT)
            style |= Gdiplus::FontStyleStrikeout;
        return style;
    }

    static Gdiplus::LineCap gdiplus_cap(heliosview_line_cap_t cap)
    {
        switch (cap) {
        case HELIOSVIEW_CAP_ROUND: return Gdiplus::LineCapRound;
        case HELIOSVIEW_CAP_SQUARE: return Gdiplus::LineCapSquare;
        default: return Gdiplus::LineCapFlat; /* butt */
        }
    }

    static Gdiplus::LineJoin gdiplus_join(heliosview_line_join_t join)
    {
        switch (join) {
        case HELIOSVIEW_JOIN_ROUND: return Gdiplus::LineJoinRound;
        case HELIOSVIEW_JOIN_BEVEL: return Gdiplus::LineJoinBevel;
        default: return Gdiplus::LineJoinMiter;
        }
    }

    static void apply_alignment(Gdiplus::StringFormat& format, uint32_t align)
    {
        if (align & HELIOSVIEW_ALIGN_HCENTER)
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
        else if (align & HELIOSVIEW_ALIGN_RIGHT)
            format.SetAlignment(Gdiplus::StringAlignmentFar);
        else
            format.SetAlignment(Gdiplus::StringAlignmentNear);

        if (align & HELIOSVIEW_ALIGN_VCENTER)
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        else if (align & HELIOSVIEW_ALIGN_BOTTOM)
            format.SetLineAlignment(Gdiplus::StringAlignmentFar);
        else
            format.SetLineAlignment(Gdiplus::StringAlignmentNear);
    }

    Gdiplus::Pen* current_pen()
    {
        apply_state();
        return m_pen.get();
    }
    Gdiplus::SolidBrush* current_brush()
    {
        apply_state();
        return m_brush.get();
    }
    Gdiplus::Font* current_font()
    {
        apply_state();
        return m_font.get();
    }

    Gdiplus::Graphics m_graphics;
    const CanvasData& m_data;
    heliosview_painter_state_t m_state{};
    bool m_state_dirty = true;
    float m_transform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    /* The clip as a device-space bounding box: GDI+ can only report the bounds of the
     * clip region, and clip_bounds must work for path clips too. */
    Rect m_clip_bounds{0.0f, 0.0f, static_cast<float>(m_data.width), static_cast<float>(m_data.height)};
    std::unique_ptr<Gdiplus::Pen> m_pen;
    std::unique_ptr<Gdiplus::SolidBrush> m_brush;
    std::unique_ptr<Gdiplus::SolidBrush> m_fill;
    std::unique_ptr<Gdiplus::Font> m_font;
    std::unique_ptr<Gdiplus::FontFamily> m_family;

    struct SavedGdiState {
        Gdiplus::GraphicsState gstate;
        Rect clip_bounds;
    };
    std::vector<SavedGdiState> m_saved_gdi_states;
};

/* ================= Engine ================= */

class GdiPlusEngine final : public Engine {
public:
    heliosview_canvas_engine_t id() const override { return HELIOSVIEW_ENGINE_GDI_PLUS; }
    const char* name() const override { return "gdi+"; }
    bool probe() override { return gdiplus_ready(); }
    heliosview_pixel_format_t preferred_format() const override
    {
        return HELIOSVIEW_FORMAT_BGRA8_PREMUL;
    }
    bool supports_format(heliosview_pixel_format_t format) const override
    {
        switch (format) {
        case HELIOSVIEW_FORMAT_BGRA8_PREMUL:
        case HELIOSVIEW_FORMAT_BGRA8:
        case HELIOSVIEW_FORMAT_RGBA8:
        case HELIOSVIEW_FORMAT_GRAY8:
            return true;
        default:
            return false;
        }
    }
    bool supports_feature(int feature) const override
    {
        switch (static_cast<heliosview_canvas_feature_t>(feature)) {
        case HELIOSVIEW_FEATURE_ANTIALIAS:
        case HELIOSVIEW_FEATURE_ALPHA_BLEND:
        case HELIOSVIEW_FEATURE_TRANSFORM:
        case HELIOSVIEW_FEATURE_CLIP_PATH:
        case HELIOSVIEW_FEATURE_PATH_FILL:
        case HELIOSVIEW_FEATURE_TEXT:
        case HELIOSVIEW_FEATURE_TEXT_AA:
        case HELIOSVIEW_FEATURE_IMAGE_DRAW:
        case HELIOSVIEW_FEATURE_DIRECT_PIXELS:
            return true;
        default:
            return false;
        }
    }

    std::unique_ptr<CanvasAdapter> create_canvas(const CanvasData& data) override
    {
        if (!gdiplus_ready() || !data.pixels)
            return nullptr;
        auto adapter = std::make_unique<CanvasAdapterImpl>(data.pixels, data.width, data.height,
                                                           data.stride, gdiplus_format(data.format));
        if (!adapter->valid())
            return nullptr;
        return adapter;
    }

    std::unique_ptr<Context> create_context(CanvasAdapter& canvas, const CanvasData& data) override
    {
        auto* adapter = dynamic_cast<CanvasAdapterImpl*>(&canvas);
        if (!adapter || !adapter->valid())
            return nullptr;
        auto context = std::make_unique<ContextImpl>(*adapter, data);
        if (!context->valid())
            return nullptr;
        return context;
    }
};

GdiPlusEngine g_engine;

/* Registered from a static initializer: the engine table is filled before main, and
 * the id doubles as the primary vendor name. NATIVE is the concept name, which is
 * what portable code asks for -- on Windows that resolves here. */
const EngineRegistration g_registration(&g_engine, HELIOSVIEW_ENGINE_GDI_PLUS,
                                        HELIOSVIEW_ENGINE_NATIVE);

} // namespace
} // namespace hv::canvas
