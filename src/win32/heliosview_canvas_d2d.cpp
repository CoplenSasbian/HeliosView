/* Direct2D canvas engine (Windows).
 *
 * Direct2D cannot wrap a caller-owned buffer the way Gdiplus::Bitmap can, so this
 * engine keeps a WIC bitmap as the device-side twin of the canvas and moves the
 * pixels across that boundary once per drawing call. Everything in between is real
 * Direct2D: paths become ID2D1PathGeometry, clipping is PushAxisAlignedClip and
 * PushLayer, text is DirectWrite, images are ID2D1Bitmap. The observable contract is
 * the same as the GDI+ engine's zero-copy one -- the canvas buffer is authoritative
 * and current after every call -- so this file pays for that with a read-back instead
 * of sharing memory. See the note by the registration at the end of the file.
 */

#include "../heliosview_canvas_internal.h"
#include "../win32/heliosview_win32_internal.h" /* utf8_to_wide */

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace hv::canvas {
namespace {

using Microsoft::WRL::ComPtr;

/* ================= WIC, D2D and DirectWrite factories =================
 *
 * One process-wide set. ID2D1Factory and IDWriteFactory are meant to be shared and
 * the WIC factory is a COM singleton. They are deliberately never released: the canvas
 * layer has no teardown hook, and the process exit reclaims them -- the same reasoning
 * the GDI+ engine uses for the GdiplusStartup token it never hands back.
 */

ComPtr<IWICImagingFactory> create_wic_factory()
{
    ComPtr<IWICImagingFactory> factory;
    IWICImagingFactory* raw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&raw));
    if (FAILED(hr)) {
        /* WIC is COM: a host that has not set up an apartment for this thread gets one
         * here. If one is already in place (any model) COM is ready and the retry
         * succeeds -- a model mismatch reports RPC_E_CHANGED_MODE, which is not a
         * failure for us and is deliberately not treated as one. */
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&raw));
    }
    if (SUCCEEDED(hr))
        factory.Attach(raw);
    return factory;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
};

Factories& factories()
{
    static Factories* instance = []() -> Factories* {
        auto* created = new Factories();
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options,
                                     created->d2d.GetAddressOf())))
            created->d2d.Reset();
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(created->dwrite.GetAddressOf()))))
            created->dwrite.Reset();
        created->wic = create_wic_factory();
        return created;
    }();
    return *instance;
}

/* Definitive: all three are needed, so a missing one means create_canvas must not be
 * attempted at all (probe() is a promise, not a guess -- see Engine::probe). D2D1 and
 * DirectWrite ship with Windows 7 on, WIC with Vista. */
bool d2d_ready() { return factories().d2d && factories().dwrite && factories().wic; }

/* ================= small translations ================= */

inline float channel(uint32_t argb, int shift)
{
    return static_cast<float>((argb >> shift) & 0xFFu) / 255.0f;
}

/* The state carries the painter's alpha separately from the color's own alpha; the
 * product is what the device draws with. D2D1_COLOR_F is straight (not premultiplied)
 * alpha, exactly like the public API. */
inline D2D1_COLOR_F to_color(uint32_t argb, float alpha)
{
    return D2D1::ColorF(channel(argb, 16), channel(argb, 8), channel(argb, 0),
                        channel(argb, 24) * std::clamp(alpha, 0.0f, 1.0f));
}

inline bool visible(uint32_t argb) { return (argb >> 24) != 0; }

/* The 2x3 matrix laid out the way the public API documents it ([a b c d e f]) is the
 * same six numbers D2D1 uses; only the member names differ. */
inline D2D1_MATRIX_3X2_F to_matrix(const float m[6])
{
    return D2D1::Matrix3x2F(m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* Negative width/height are legal in the public API and mean the rectangle flipped
 * about its origin (GDI+ and Core Graphics read them the same way). Direct2D has no
 * negative extents, so every rectangle is taken through this first. */
inline D2D1_RECT_F normalized_rectf(const Rect& r)
{
    const float left = r.w < 0.0f ? r.x + r.w : r.x;
    const float top = r.h < 0.0f ? r.y + r.h : r.y;
    return D2D1::RectF(left, top, left + std::fabs(r.w), top + std::fabs(r.h));
}

inline float box_width(const D2D1_RECT_F& r) { return r.right - r.left; }
inline float box_height(const D2D1_RECT_F& r) { return r.bottom - r.top; }

/* The device-space box of a rectangle in the transform's own space. All four corners
 * are mapped rather than just the origin, because a rotation or a skew pulls them
 * apart; the result encloses the shape, which is all clip_bounds promises. */
inline Rect device_bounds(const D2D1_RECT_F& box, const float m[6])
{
    const float xs[4] = {box.left, box.right, box.left, box.right};
    const float ys[4] = {box.top, box.top, box.bottom, box.bottom};
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = xs[i] * m[0] + ys[i] * m[2] + m[4];
        const float y = xs[i] * m[1] + ys[i] * m[3] + m[5];
        if (i == 0) {
            left = right = x;
            top = bottom = y;
        } else {
            left = x < left ? x : left;
            right = x > right ? x : right;
            top = y < top ? y : top;
            bottom = y > bottom ? y : bottom;
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

/* The bounding box of a path's own points. Curves are bounded by their control points
 * and shapes by their rectangle, both conservative -- which a clip box may be. */
Rect path_bounds(const PathData& path)
{
    bool any = false;
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    auto take = [&](float x, float y) {
        if (!any) {
            left = right = x;
            top = bottom = y;
            any = true;
            return;
        }
        left = x < left ? x : left;
        right = x > right ? x : right;
        top = y < top ? y : top;
        bottom = y > bottom ? y : bottom;
    };
    for (const PathEntry& entry : path.entries) {
        switch (entry.verb) {
        case PathVerbRect:
        case PathVerbRoundRect:
        case PathVerbEllipse:
            take(entry.pts[0], entry.pts[1]);
            take(entry.pts[0] + entry.pts[2], entry.pts[1] + entry.pts[3]);
            break;
        default: {
            const int count = PathEntry::point_count(entry.verb);
            for (int i = 0; i + 1 < count; i += 2)
                take(entry.pts[i], entry.pts[i + 1]);
            break;
        }
        }
    }
    return any ? Rect{left, top, right - left, bottom - top} : Rect{0.0f, 0.0f, 0.0f, 0.0f};
}

/* A rectangle as the path form of itself. A clip rectangle takes this route rather than
 * ID2D1RenderTarget::PushAxisAlignedClip, because only a geometry can be transformed
 * into device space and a rotated or skewed clip rectangle has to stay one. */
PathData rect_path(const Rect& r)
{
    PathData shape;
    PathEntry entry;
    entry.verb = PathVerbRect;
    entry.pts[0] = r.x;
    entry.pts[1] = r.y;
    entry.pts[2] = r.w;
    entry.pts[3] = r.h;
    shape.entries.push_back(entry);
    return shape;
}

/* ================= path translation =================
 *
 * PathData is a verb list; Direct2D wants figures on an ID2D1GeometrySink. The one
 * structural difference is that D2D has no move-to: a subpath starts at BeginFigure,
 * which is exactly what PathVerbStartNew means. */
class SinkWriter {
public:
    explicit SinkWriter(ID2D1GeometrySink* sink) : m_sink(sink) {}

    void start(float x, float y)
    {
        /* A subpath starting while one is still open ends the previous figure the way
         * an unclosed path would be filled: implicitly closed by the fill rule. */
        end_figure(D2D1_FIGURE_END_OPEN);
        m_sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        m_open = true;
        m_current = D2D1::Point2F(x, y);
    }

    void line_to(float x, float y)
    {
        if (!m_open)
            start(x, y); /* no leading StartNew: the point becomes the subpath's start */
        else
            m_sink->AddLine(D2D1::Point2F(x, y));
        m_current = D2D1::Point2F(x, y);
    }

    /* Direct2D has no quadratic segment, so it is raised to the equivalent cubic: both
     * control points at 2/3 of the way toward the quadratic control point, the exact
     * conversion (and the one the GDI+ engine performs as well). */
    void quad_to(float cx, float cy, float x, float y)
    {
        if (!m_open)
            start(cx, cy);
        const D2D1_POINT_2F c1 = D2D1::Point2F(m_current.x + (2.0f / 3.0f) * (cx - m_current.x),
                                               m_current.y + (2.0f / 3.0f) * (cy - m_current.y));
        const D2D1_POINT_2F c2 =
            D2D1::Point2F(x + (2.0f / 3.0f) * (cx - x), y + (2.0f / 3.0f) * (cy - y));
        add_bezier(c1, c2, D2D1::Point2F(x, y));
    }

    void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y)
    {
        if (!m_open)
            start(c1x, c1y);
        add_bezier(D2D1::Point2F(c1x, c1y), D2D1::Point2F(c2x, c2y), D2D1::Point2F(x, y));
    }

    void close()
    {
        if (m_open) {
            m_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            m_open = false;
        }
    }

    /* A whole shape is its own subpath: a shape verb between two line verbs (legal in
     * a verb list) closes whatever figure was open and starts a fresh one, matching
     * GDI+'s AddRectangle/AddEllipse. */
    void rect(const Rect& r)
    {
        const D2D1_RECT_F box = normalized_rectf(r);
        if (box_width(box) <= 0.0f || box_height(box) <= 0.0f)
            return;
        end_figure(D2D1_FIGURE_END_OPEN);
        m_sink->BeginFigure(D2D1::Point2F(box.left, box.top), D2D1_FIGURE_BEGIN_FILLED);
        m_sink->AddLine(D2D1::Point2F(box.right, box.top));
        m_sink->AddLine(D2D1::Point2F(box.right, box.bottom));
        m_sink->AddLine(D2D1::Point2F(box.left, box.bottom));
        m_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }

    /* Four straight sides and four quarter arcs, the construction the GDI+ engine
     * builds too, with the radius clamped to half the shorter side. */
    void round_rect(const Rect& r, float radius)
    {
        const D2D1_RECT_F box = normalized_rectf(r);
        const float w = box_width(box);
        const float h = box_height(box);
        if (w <= 0.0f || h <= 0.0f)
            return;
        const float limit = (w < h ? w : h) * 0.5f;
        const float rad = std::clamp(radius, 0.0f, limit);
        if (rad <= 0.0f) {
            rect(r);
            return;
        }
        end_figure(D2D1_FIGURE_END_OPEN);
        const D2D1_SIZE_F size = D2D1::SizeF(rad, rad);
        m_sink->BeginFigure(D2D1::Point2F(box.left + rad, box.top), D2D1_FIGURE_BEGIN_FILLED);
        m_sink->AddLine(D2D1::Point2F(box.right - rad, box.top));
        quarter_arc(D2D1::Point2F(box.right, box.top + rad), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        m_sink->AddLine(D2D1::Point2F(box.right, box.bottom - rad));
        quarter_arc(D2D1::Point2F(box.right - rad, box.bottom), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        m_sink->AddLine(D2D1::Point2F(box.left + rad, box.bottom));
        quarter_arc(D2D1::Point2F(box.left, box.bottom - rad), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        m_sink->AddLine(D2D1::Point2F(box.left, box.top + rad));
        quarter_arc(D2D1::Point2F(box.left + rad, box.top), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        m_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }

    /* Four quarter arcs from the top and around, which is exact for the ellipse: an
     * arc through two axis points with half-extent radii *is* that quarter. */
    void ellipse(const Rect& r)
    {
        const D2D1_RECT_F box = normalized_rectf(r);
        const float w = box_width(box);
        const float h = box_height(box);
        if (w <= 0.0f || h <= 0.0f)
            return;
        const float rx = w * 0.5f;
        const float ry = h * 0.5f;
        const D2D1_SIZE_F size = D2D1::SizeF(rx, ry);
        end_figure(D2D1_FIGURE_END_OPEN);
        m_sink->BeginFigure(D2D1::Point2F(box.left + rx, box.top), D2D1_FIGURE_BEGIN_FILLED);
        quarter_arc(D2D1::Point2F(box.right, box.top + ry), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        quarter_arc(D2D1::Point2F(box.left + rx, box.bottom), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        quarter_arc(D2D1::Point2F(box.left, box.top + ry), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        quarter_arc(D2D1::Point2F(box.left + rx, box.top), size, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        m_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }

    void end_figure(D2D1_FIGURE_END end)
    {
        if (m_open) {
            m_sink->EndFigure(end);
            m_open = false;
        }
    }

private:
    void add_bezier(const D2D1_POINT_2F& c1, const D2D1_POINT_2F& c2, const D2D1_POINT_2F& to)
    {
        const D2D1_BEZIER_SEGMENT segment = D2D1::BezierSegment(c1, c2, to);
        m_sink->AddBezier(&segment);
        m_current = to;
    }

    /* A clockwise sweep appears clockwise on screen, which is what a positive sweep
     * angle means in the public API (y grows downward). */
    void quarter_arc(const D2D1_POINT_2F& to, const D2D1_SIZE_F& size, D2D1_SWEEP_DIRECTION sweep)
    {
        const D2D1_ARC_SEGMENT arc =
            D2D1::ArcSegment(to, size, 0.0f, sweep, D2D1_ARC_SIZE_SMALL);
        m_sink->AddArc(&arc);
        m_current = to;
    }

    ID2D1GeometrySink* m_sink;
    D2D1_POINT_2F m_current = D2D1::Point2F(0.0f, 0.0f);
    bool m_open = false;
};

void emit_path(const PathData& path, ID2D1GeometrySink* sink)
{
    SinkWriter writer(sink);
    for (const PathEntry& entry : path.entries) {
        switch (entry.verb) {
        case PathVerbStartNew:
            writer.start(entry.pts[0], entry.pts[1]);
            break;
        case PathVerbLineTo:
            writer.line_to(entry.pts[0], entry.pts[1]);
            break;
        case PathVerbQuadTo:
            writer.quad_to(entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3]);
            break;
        case PathVerbCubicTo:
            writer.cubic_to(entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3], entry.pts[4],
                            entry.pts[5]);
            break;
        case PathVerbClose:
            writer.close();
            break;
        case PathVerbRect:
            writer.rect(Rect{entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3]});
            break;
        case PathVerbRoundRect:
            writer.round_rect(Rect{entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3]},
                              entry.pts[4]);
            break;
        case PathVerbEllipse:
            writer.ellipse(Rect{entry.pts[0], entry.pts[1], entry.pts[2], entry.pts[3]});
            break;
        default:
            break;
        }
    }
    /* Whatever was left open stays open: a fill closes it implicitly (D2D fills open
     * figures), a stroke does not -- the same as the GDI+ engine. */
    writer.end_figure(D2D1_FIGURE_END_OPEN);
}

ComPtr<ID2D1PathGeometry> build_geometry(const PathData& path)
{
    ComPtr<ID2D1PathGeometry> geometry;
    ID2D1Factory* factory = factories().d2d.Get();
    if (!factory || path.empty())
        return geometry;
    if (FAILED(factory->CreatePathGeometry(&geometry)))
        return ComPtr<ID2D1PathGeometry>();
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)))
        return ComPtr<ID2D1PathGeometry>();
    sink->SetFillMode(path.winding == HELIOSVIEW_WINDING_EVENODD ? D2D1_FILL_MODE_ALTERNATE
                                                                 : D2D1_FILL_MODE_WINDING);
    emit_path(path, sink.Get());
    if (FAILED(sink->Close()))
        return ComPtr<ID2D1PathGeometry>();
    return geometry;
}

/* ================= canvas adapter =================
 *
 * The device twin: a WIC bitmap in the one format Direct2D blends in, created on
 * first use so a canvas that is only filled, blitted or encoded never allocates one.
 * It is also what lets the engine report DIRECT_PIXELS: the caller's buffer stays the
 * storage of record, and this side is refreshed from it (upload) and written back to
 * it (download) around every drawing call.
 */
class CanvasAdapterImpl final : public CanvasAdapter {
public:
    CanvasAdapterImpl(int32_t width, int32_t height) : m_width(width), m_height(height) {}

    bool direct_pixels() const override { return true; }

    /* Called after the core wrote to the buffer behind the engine's back (canvas_fill,
     * canvas_set_pixel, canvas_blit, canvas_end_write): the twin is stale from here on
     * and is re-read at the next drawing call. */
    void reload() override { m_synced = false; }

    void* native_bitmap() override { return m_bitmap.Get(); }

    IWICBitmap* ensure_bitmap()
    {
        if (m_bitmap)
            return m_bitmap.Get();
        IWICImagingFactory* wic = factories().wic.Get();
        if (!wic || m_width <= 0 || m_height <= 0)
            return nullptr;
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(wic->CreateBitmap(static_cast<UINT>(m_width), static_cast<UINT>(m_height),
                                     GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            return nullptr;
        m_bitmap = std::move(bitmap);
        m_synced = false;
        return m_bitmap.Get();
    }

    /* buffer -> device, skipped when the two are already in step. */
    bool upload(const CanvasData& canvas)
    {
        if (m_synced && m_bitmap)
            return true;
        if (!ensure_bitmap() || !canvas.pixels || canvas.width != m_width ||
            canvas.height != m_height)
            return false;
        const size_t row_bytes = static_cast<size_t>(m_width) * 4u;
        /* The common case: the canvas is the premultiplied BGRA the device wants and
         * its rows are tight, so the whole buffer moves in one call. */
        if (canvas.format == HELIOSVIEW_FORMAT_BGRA8_PREMUL &&
            canvas.stride == static_cast<int32_t>(row_bytes)) {
            if (!write_rows(canvas.pixels, row_bytes, m_height))
                return false;
        } else if (!convert_to_device(canvas)) {
            return false;
        }
        m_synced = true;
        return true;
    }

    /* device -> buffer. */
    void download(const CanvasData& canvas)
    {
        if (!m_bitmap || !canvas.pixels || canvas.width != m_width || canvas.height != m_height)
            return;
        const size_t row_bytes = static_cast<size_t>(m_width) * 4u;
        if (canvas.format == HELIOSVIEW_FORMAT_BGRA8_PREMUL &&
            canvas.stride == static_cast<int32_t>(row_bytes)) {
            if (!read_rows(canvas.pixels, row_bytes, m_height))
                return;
        } else {
            convert_from_device(canvas);
        }
        m_synced = true;
    }

    int32_t width() const { return m_width; }
    int32_t height() const { return m_height; }

private:
    /* WIC hands its memory out through a lock rather than a write call. The lock is
     * held only for the copy, because Direct2D locks the same bitmap for the whole of
     * a drawing call. */
    bool write_rows(const uint8_t* data, size_t row_bytes, int32_t height)
    {
        const WICRect area{0, 0, m_width, height};
        ComPtr<IWICBitmapLock> lock;
        if (FAILED(m_bitmap->Lock(&area, WICBitmapLockWrite, &lock)))
            return false;
        UINT stride = 0;
        UINT size = 0;
        BYTE* base = nullptr;
        if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &base)))
            return false;
        for (int32_t y = 0; y < height; ++y)
            std::memcpy(base + static_cast<size_t>(y) * stride,
                        data + static_cast<size_t>(y) * row_bytes, row_bytes);
        return true;
    }

    bool read_rows(uint8_t* data, size_t row_bytes, int32_t height)
    {
        return SUCCEEDED(m_bitmap->CopyPixels(nullptr, static_cast<UINT>(row_bytes),
                                              static_cast<UINT>(row_bytes * static_cast<size_t>(height)),
                                              data));
    }

    /* Formats other than premultiplied BGRA go through the core's converter, so the
     * format matrix (channel order, premultiply, gray) lives in one place instead of
     * once per engine. The destination starts cleared because convert_copy blends: a
     * zeroed row is the identity for a source pixel of any alpha. */
    bool convert_to_device(const CanvasData& canvas)
    {
        const size_t row_bytes = static_cast<size_t>(m_width) * 4u;
        m_scratch.assign(row_bytes * static_cast<size_t>(m_height), 0u);
        const CanvasData device{m_width, m_height, static_cast<int32_t>(row_bytes),
                                HELIOSVIEW_FORMAT_BGRA8_PREMUL, m_scratch.data()};
        const Rect all{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)};
        convert_copy(canvas, all, device, 0, 0, 1.0f);
        return write_rows(m_scratch.data(), row_bytes, m_height);
    }

    void convert_from_device(const CanvasData& canvas)
    {
        const size_t row_bytes = static_cast<size_t>(m_width) * 4u;
        m_scratch.resize(row_bytes * static_cast<size_t>(m_height));
        if (!read_rows(m_scratch.data(), row_bytes, m_height))
            return;
        /* Row by row, because the destination rows carry the canvas's own stride and
         * the converter takes a whole layout. Each row is cleared first: the device
         * copy is the whole truth, so the blend must not keep what was there. */
        const int32_t bpp = bytes_per_pixel(canvas.format);
        const CanvasData row_source{m_width, 1, static_cast<int32_t>(row_bytes),
                                    HELIOSVIEW_FORMAT_BGRA8_PREMUL, nullptr};
        for (int32_t y = 0; y < m_height; ++y) {
            uint8_t* row = canvas.pixels + static_cast<size_t>(y) * static_cast<size_t>(canvas.stride);
            std::memset(row, 0, static_cast<size_t>(m_width) * static_cast<size_t>(bpp));
            CanvasData source = row_source;
            source.pixels = m_scratch.data() + static_cast<size_t>(y) * row_bytes;
            const CanvasData target{m_width, 1, canvas.stride, canvas.format, row};
            convert_copy(source, Rect{0.0f, 0.0f, static_cast<float>(m_width), 1.0f}, target, 0, 0,
                         1.0f);
        }
    }

    int32_t m_width = 0;
    int32_t m_height = 0;
    ComPtr<IWICBitmap> m_bitmap;
    std::vector<uint8_t> m_scratch;
    /* Whether the twin still matches the caller's buffer. The core reports every write
     * it makes itself through reload(); a device -> buffer write leaves the two in
     * step, so this stays true across a read-back. */
    bool m_synced = false;
};

/* ================= context =================
 *
 * One BeginDraw/EndDraw pair per public drawing call. Direct2D wants every drawing
 * call inside a pair, and the read-back that keeps the canvas buffer current cannot
 * happen while one is open -- so the operations are atomic rather than a session. The
 * transform and the clip are re-applied at the start of each pair (neither is relied
 * on to survive an EndDraw), and both are described on this side rather than left on
 * the device, which is also what lets clear() drop them for its one call.
 */
class ContextImpl final : public Context {
public:
    ContextImpl(CanvasAdapterImpl& adapter, const CanvasData& data)
        : m_adapter(adapter), m_data(data)
    {
        if (!d2d_ready() || !adapter.ensure_bitmap())
            return;
        /* A WIC render target is software by definition; asking for SOFTWARE keeps the
         * engine off a hardware path that would refuse a WIC bitmap anyway. */
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(factories().d2d->CreateWicBitmapRenderTarget(adapter.ensure_bitmap(), properties,
                                                                &m_target)))
            return;
        if (!adapter.upload(m_data))
            return;
        m_clip_bounds =
            Rect{0.0f, 0.0f, static_cast<float>(m_data.width), static_cast<float>(m_data.height)};
        m_valid = true;
    }

    bool valid() const { return m_valid && m_target; }

    /* ---- state ---- */
    void set_state(const heliosview_painter_state_t& state) override
    {
        m_state = state;
        m_state_dirty = true;
    }

    void set_transform(const float m[6]) override
    {
        std::copy(m, m + 6, m_transform);
        /* The transform is installed by begin(); nothing is left on the device between
         * calls, so there is no stale matrix to take back. */
    }

    /* ---- clipping ---- */

    /* Direct2D clips only ever intersect, so "replace" is modelled on this side: the
     * clip is a list of shapes, set_clip_* replaces it, intersect_* appends to it, and
     * begin() hands the list to the device as layers. Each shape keeps the transform it
     * was installed under, so a clip is resolved into device space once, at the moment
     * the caller asked for it, and a later set_transform carries the drawing to the
     * clip instead of dragging the clip along with it. */
    void set_clip_rect(const Rect& r) override
    {
        m_clips.clear();
        add_clip(rect_path(r));
        m_clip_bounds =
            intersect_rect(canvas_bounds(), device_bounds(normalized_rectf(r), m_transform));
    }

    void intersect_clip_rect(const Rect& r) override
    {
        add_clip(rect_path(r));
        m_clip_bounds = intersect_rect(m_clip_bounds, device_bounds(normalized_rectf(r), m_transform));
    }

    void set_clip_path(const PathData& path) override
    {
        if (path.empty())
            return;
        m_clips.clear();
        add_clip(path);
        /* Curves are bounded by their control points, which is a conservative box --
         * which is all clip_bounds promises. */
        m_clip_bounds = intersect_rect(
            canvas_bounds(), device_bounds(normalized_rectf(path_bounds(path)), m_transform));
    }

    void reset_clip() override
    {
        m_clips.clear();
        m_clip_bounds = canvas_bounds();
    }

    void clip_bounds(Rect& out) const override { out = m_clip_bounds; }

    void save() override
    {
        m_saved_clip_states.push_back(SavedD2dState{m_clips, m_clip_bounds});
    }

    void restore() override
    {
        if (!m_saved_clip_states.empty()) {
            m_clips = m_saved_clip_states.back().clips;
            m_clip_bounds = m_saved_clip_states.back().clip_bounds;
            m_saved_clip_states.pop_back();
        }
    }

    /* ---- shapes ---- */
    void clear(uint32_t argb) override
    {
        /* clear() ignores the transform and the clip by contract, so it gets its own
         * pair with neither applied. Neither is on the device between calls, so only
         * the transform has to be pushed out of the way. */
        end();
        if (!m_target)
            return;
        apply_state();
        m_target->BeginDraw();
        m_target->SetTransform(D2D1::IdentityMatrix());
        const D2D1_COLOR_F color = to_color(argb, 1.0f);
        m_target->Clear(&color);
        if (FAILED(m_target->EndDraw())) {
            m_target.Reset();
            return;
        }
        m_adapter.download(m_data);
    }

    void draw_line(float x1, float y1, float x2, float y2) override
    {
        if (!begin())
            return;
        if (m_stroke)
            m_target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), m_stroke.Get(),
                               stroke_width(), m_stroke_style.Get());
        end();
    }

    void draw_rect(const Rect& r) override
    {
        if (!begin())
            return;
        const D2D1_RECT_F box = normalized_rectf(r);
        if (m_fill)
            m_target->FillRectangle(box, m_fill.Get());
        if (m_stroke)
            m_target->DrawRectangle(box, m_stroke.Get(), stroke_width(), m_stroke_style.Get());
        end();
    }

    void draw_round_rect(const Rect& r, float radius) override
    {
        /* Direct2D has no rounded-rectangle primitive, so it goes through the geometry
         * builder, which is also where the identical construction lives for a
         * PathVerbRoundRect. */
        PathData shape;
        PathEntry entry;
        entry.verb = PathVerbRoundRect;
        entry.pts[0] = r.x;
        entry.pts[1] = r.y;
        entry.pts[2] = r.w;
        entry.pts[3] = r.h;
        entry.pts[4] = radius;
        shape.entries.push_back(entry);
        draw_geometry(shape, true, true);
    }

    void draw_ellipse(const Rect& r) override
    {
        if (!begin())
            return;
        const D2D1_RECT_F box = normalized_rectf(r);
        const float rx = box_width(box) * 0.5f;
        const float ry = box_height(box) * 0.5f;
        if (rx > 0.0f && ry > 0.0f) {
            const D2D1_ELLIPSE ellipse =
                D2D1::Ellipse(D2D1::Point2F(box.left + rx, box.top + ry), rx, ry);
            if (m_fill)
                m_target->FillEllipse(ellipse, m_fill.Get());
            if (m_stroke)
                m_target->DrawEllipse(ellipse, m_stroke.Get(), stroke_width(), m_stroke_style.Get());
        }
        end();
    }

    void draw_arc(const Rect& r, float start_deg, float sweep_deg) override
    {
        const D2D1_RECT_F box = normalized_rectf(r);
        const float rx = box_width(box) * 0.5f;
        const float ry = box_height(box) * 0.5f;
        if (rx <= 0.0f || ry <= 0.0f || sweep_deg == 0.0f)
            return;
        constexpr float k_pi = 3.14159265358979323846f;
        const float cx = box.left + rx;
        const float cy = box.top + ry;
        const float start = start_deg * k_pi / 180.0f;
        const float sweep = sweep_deg * k_pi / 180.0f;
        const D2D1_POINT_2F from =
            D2D1::Point2F(cx + rx * std::cos(start), cy + ry * std::sin(start));
        const D2D1_POINT_2F to =
            D2D1::Point2F(cx + rx * std::cos(start + sweep), cy + ry * std::sin(start + sweep));

        ComPtr<ID2D1PathGeometry> geometry;
        if (!factories().d2d || FAILED(factories().d2d->CreatePathGeometry(&geometry)))
            return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geometry->Open(&sink)))
            return;
        /* An arc is stroke only, and a hollow figure is what keeps a fill from closing
         * it up into a pie slice. */
        sink->BeginFigure(from, D2D1_FIGURE_BEGIN_HOLLOW);
        const D2D1_ARC_SEGMENT arc =
            D2D1::ArcSegment(to, D2D1::SizeF(rx, ry), 0.0f,
                             sweep > 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                          : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                             std::fabs(sweep) > k_pi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL);
        sink->AddArc(&arc);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close()))
            return;

        if (!begin())
            return;
        if (m_stroke)
            m_target->DrawGeometry(geometry.Get(), m_stroke.Get(), stroke_width(),
                                   m_stroke_style.Get());
        end();
    }

    void draw_polyline(const float* points, size_t count, bool closed) override
    {
        if (!points || count < 2)
            return;
        PathData shape;
        PathEntry entry;
        entry.verb = PathVerbStartNew;
        entry.pts[0] = points[0];
        entry.pts[1] = points[1];
        shape.entries.push_back(entry);
        for (size_t i = 1; i < count; ++i) {
            entry.verb = PathVerbLineTo;
            entry.pts[0] = points[i * 2];
            entry.pts[1] = points[i * 2 + 1];
            shape.entries.push_back(entry);
        }
        if (closed) {
            entry.verb = PathVerbClose;
            shape.entries.push_back(entry);
        }
        /* An open polyline is stroke only, a closed one fills as well -- the same split
         * the GDI+ engine makes between DrawLines and FillPolygon. */
        draw_geometry(shape, closed, true);
    }

    void draw_path(const PathData& path, bool fill, bool stroke) override
    {
        if (path.empty())
            return;
        draw_geometry(path, fill, stroke);
    }

    /* ---- text ---- */
    void draw_text(const char* utf8, const Rect& box, uint32_t align) override
    {
        if (!utf8 || !*utf8)
            return;
        apply_state();
        if (!m_text_format)
            return;
        const std::wstring wide = utf8_to_wide(utf8);
        const bool baselined = (align & HELIOSVIEW_ALIGN_BASELINE) != 0;
        const bool has_box = box.w > 0.0f || box.h > 0.0f;
        float x = box.x;
        float y = box.y;
        /* A point origin lays out in a box far larger than any run, so nothing wraps
         * and there is nothing for the alignment to align against. */
        float max_width = has_box ? box.w : 1.0e5f;
        float max_height = has_box ? box.h : 1.0e5f;
        if (baselined) {
            /* The box's top (or the origin) plus the ascent is where the baseline is. */
            y += m_ascent;
            if (has_box)
                max_height -= m_ascent;
        }
        if (max_width <= 0.0f)
            max_width = 1.0e5f;
        if (max_height <= 0.0f)
            max_height = 1.0e5f;

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factories().dwrite->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                                        m_text_format.Get(), max_width, max_height,
                                                        &layout)))
            return;
        if (has_box) {
            layout->SetTextAlignment(text_alignment(align));
            layout->SetParagraphAlignment(paragraph_alignment(align));
        }
        const UINT32 length = static_cast<UINT32>(wide.size());
        if (m_state.font.flags & HELIOSVIEW_FONT_UNDERLINE)
            layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, length});
        if (m_state.font.flags & HELIOSVIEW_FONT_STRIKEOUT)
            layout->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{0, length});

        if (!begin())
            return;
        if (m_text)
            m_target->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), m_text.Get());
        end();
    }

    void measure_text(const char* utf8, heliosview_text_metrics_t* out) override
    {
        if (!out)
            return;
        *out = heliosview_text_metrics_t{};
        apply_state();
        /* The line box comes from the font, not from the string: an empty string has no
         * advance width but the font still has a line box, which is what a caller
         * advancing line by line needs. */
        out->ascent = m_ascent;
        out->descent = m_descent;
        out->line_height = m_line_height;
        if (utf8 && *utf8 && m_text_format && factories().dwrite) {
            const std::wstring wide = utf8_to_wide(utf8);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(factories().dwrite->CreateTextLayout(
                    wide.c_str(), static_cast<UINT32>(wide.size()), m_text_format.Get(), 1.0e5f,
                    1.0e5f, &layout))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    out->width = metrics.width;
                    out->height = metrics.height;
                }
            }
        }
        if (out->height <= 0.0f) {
            out->height = out->ascent + out->descent;
            if (out->height <= 0.0f)
                out->height = font_size() * 1.2f;
        }
        if (out->line_height <= 0.0f)
            out->line_height = out->height;
    }

    /* ---- images ---- */
    void draw_image(CanvasAdapter* src_adapter, const CanvasData& src, const Rect& src_rect,
                    const Rect& dst_rect, float alpha) override
    {
        if (!src.pixels || !m_target)
            return;
        ComPtr<ID2D1Bitmap> bitmap;
        /* A canvas from this engine already has a twin, so the copy is device-to-device
         * and both frames are known to be in the device format. */
        if (auto* own = dynamic_cast<CanvasAdapterImpl*>(src_adapter); own && own->upload(src)) {
            if (FAILED(m_target->CreateBitmapFromWicBitmap(own->ensure_bitmap(), nullptr, &bitmap)))
                return;
        } else if (!wrap_pixels(src, &bitmap)) {
            return;
        }
        const D2D1_RECT_F source =
            D2D1::RectF(src_rect.x, src_rect.y, src_rect.x + src_rect.w, src_rect.y + src_rect.h);
        const D2D1_RECT_F destination = D2D1::RectF(dst_rect.x, dst_rect.y, dst_rect.x + dst_rect.w,
                                                    dst_rect.y + dst_rect.h);
        if (!begin())
            return;
        /* DrawBitmap is the one image call that already takes an opacity, so a
         * translucent blit needs no layer of its own. */
        m_target->DrawBitmap(bitmap.Get(), &destination, std::clamp(alpha, 0.0f, 1.0f),
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
        end();
    }

    void flush() override { end(); }

private:
    struct Clip {
        PathData path;
        /* The transform in effect when the clip was installed. */
        float transform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    };

    /* ---- one drawing operation ---- */

    /* BeginDraw with the painter's transform and clip installed. A failed upload means
     * the canvas is not the size the twin was built for (a resize under a live
     * painter, which callers are not supposed to do): drawing is skipped rather than
     * rendered into a bitmap nobody will ever see. */
    bool begin()
    {
        if (m_drawing)
            return true;
        if (!m_target || !m_adapter.upload(m_data))
            return false;
        apply_state();
        m_target->BeginDraw();
        m_drawing = true;
        /* The clip masks are in device space, so they are pushed with no transform in
         * effect -- a layer mask is interpreted in the render target's own space, which
         * is only unambiguous here. */
        m_target->SetTransform(D2D1::IdentityMatrix());
        m_target->SetAntialiasMode(m_state.antialias ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                                     : D2D1_ANTIALIAS_MODE_ALIASED);
        /* Grayscale, not ClearType: a ClearType run assumes an opaque backdrop and
         * would fringe on a canvas that is usually transparent. */
        m_target->SetTextAntialiasMode(m_state.antialias ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE
                                                        : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
        push_clip();
        m_target->SetTransform(to_matrix(m_transform));
        return true;
    }

    /* EndDraw, then write the result back so the canvas buffer is current for the next
     * call and for any canvas-level read (canvas_get_pixel, canvas_data) in between. */
    void end()
    {
        if (!m_drawing)
            return;
        pop_clip();
        const HRESULT hr = m_target->EndDraw();
        m_drawing = false;
        if (FAILED(hr)) {
            /* A WIC target should not be lost; if it ever is, stop drawing instead of
             * failing on every later call. */
            m_target.Reset();
            return;
        }
        m_adapter.download(m_data);
    }

    /* Records a clip shape with the transform it was installed under. */
    void add_clip(const PathData& shape)
    {
        Clip clip;
        clip.path = shape;
        std::copy(m_transform, m_transform + 6, clip.transform);
        m_clips.push_back(std::move(clip));
    }

    void push_clip()
    {
        for (const Clip& clip : m_clips) {
            ComPtr<ID2D1PathGeometry> geometry = build_geometry(clip.path);
            if (!geometry)
                continue;
            /* The clip is stored in the space it was installed in, so it goes to device
             * space through the transform that was in effect then -- which is also the
             * whole reason a rect clip is a geometry here and not an axis-aligned clip:
             * an axis-aligned clip cannot describe a rotated or skewed rectangle. */
            ComPtr<ID2D1TransformedGeometry> device_geometry;
            if (FAILED(factories().d2d->CreateTransformedGeometry(
                    geometry.Get(), to_matrix(clip.transform), &device_geometry)))
                continue;
            ComPtr<ID2D1Layer> layer;
            if (FAILED(m_target->CreateLayer(&layer)))
                continue;
            /* The transform is identity here, so the mask's space is the device's. */
            m_target->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), device_geometry.Get()), layer.Get());
            /* The layer object has to outlive the push. */
            m_pushed.push_back(std::move(layer));
        }
    }

    /* Layers nest, so they come back off in reverse. */
    void pop_clip()
    {
        for (size_t i = m_pushed.size(); i > 0; --i)
            m_target->PopLayer();
        m_pushed.clear();
    }

    void draw_geometry(const PathData& path, bool fill, bool stroke)
    {
        if (!fill && !stroke)
            return;
        ComPtr<ID2D1PathGeometry> geometry = build_geometry(path);
        if (!geometry)
            return;
        if (!begin())
            return;
        if (fill && m_fill)
            m_target->FillGeometry(geometry.Get(), m_fill.Get());
        if (stroke && m_stroke)
            m_target->DrawGeometry(geometry.Get(), m_stroke.Get(), stroke_width(),
                                   m_stroke_style.Get());
        end();
    }

    /* A source canvas from another engine, or one whose twin has no pixels yet: the
     * pixels are wrapped in a device bitmap. The common layout copies straight through;
     * anything else is converted to the premultiplied BGRA the target blends. */
    bool wrap_pixels(const CanvasData& src, ComPtr<ID2D1Bitmap>* out)
    {
        if (src.width <= 0 || src.height <= 0)
            return false;
        const uint8_t* pixels = src.pixels;
        int32_t stride = src.stride;
        if (src.format != HELIOSVIEW_FORMAT_BGRA8_PREMUL) {
            const size_t row_bytes = static_cast<size_t>(src.width) * 4u;
            m_image_scratch.assign(row_bytes * static_cast<size_t>(src.height), 0u);
            const CanvasData converted{src.width, src.height, static_cast<int32_t>(row_bytes),
                                       HELIOSVIEW_FORMAT_BGRA8_PREMUL, m_image_scratch.data()};
            convert_copy(src,
                         Rect{0.0f, 0.0f, static_cast<float>(src.width), static_cast<float>(src.height)},
                         converted, 0, 0, 1.0f);
            pixels = m_image_scratch.data();
            stride = static_cast<int32_t>(row_bytes);
        }
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        return SUCCEEDED(m_target->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(src.width), static_cast<UINT32>(src.height)), pixels,
            static_cast<UINT32>(stride), properties, out->GetAddressOf()));
    }

    /* ---- state translation ---- */

    /* Pens, brushes and the font are rebuilt lazily: set_state only marks, and the next
     * drawing call translates. */
    void apply_state()
    {
        if (!m_state_dirty)
            return;
        m_state_dirty = false;
        m_stroke.Reset();
        m_fill.Reset();
        m_text.Reset();
        m_stroke_style.Reset();

        create_stroke_style();
        font_from_state();

        if (!m_target)
            return;
        /* A transparent color means "no stroke" / "no fill", which is how the public
         * API expresses them; the painter does not call in at all for those, so this is
         * a second guard rather than the mechanism. */
        if (visible(m_state.stroke_color)) {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(m_target->CreateSolidColorBrush(
                    to_color(m_state.stroke_color, m_state.alpha), &brush)))
                m_stroke = std::move(brush);
        }
        if (visible(m_state.fill_color)) {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(m_target->CreateSolidColorBrush(
                    to_color(m_state.fill_color, m_state.alpha), &brush)))
                m_fill = std::move(brush);
        }
        /* Text draws in the fill color, so its brush is built even when a shape fill
         * was not asked for. */
        ComPtr<ID2D1SolidColorBrush> text_brush;
        if (SUCCEEDED(m_target->CreateSolidColorBrush(to_color(m_state.fill_color, m_state.alpha),
                                                      &text_brush)))
            m_text = std::move(text_brush);
    }

    void create_stroke_style()
    {
        ID2D1Factory* factory = factories().d2d.Get();
        if (!factory)
            return;
        /* Start and dash caps share the line cap setting; the dash cap never shows for
         * a solid line, and a stroke style has to name one. */
        const D2D1_CAP_STYLE cap = d2d_cap(m_state.line_cap);
        const D2D1_STROKE_STYLE_PROPERTIES properties =
            D2D1::StrokeStyleProperties(cap, cap, D2D1_CAP_STYLE_FLAT, d2d_join(m_state.line_join));
        ComPtr<ID2D1StrokeStyle> style;
        if (SUCCEEDED(factory->CreateStrokeStyle(properties, nullptr, 0, &style)))
            m_stroke_style = std::move(style);
    }

    /* The text format and the font's line metrics are rebuilt together: the metrics
     * come from the face the format resolved to, so they cannot go stale apart. */
    void font_from_state()
    {
        m_text_format.Reset();
        m_ascent = 0.0f;
        m_descent = 0.0f;
        m_line_height = 0.0f;
        IDWriteFactory* dwrite = factories().dwrite.Get();
        if (!dwrite)
            return;
        const float size = font_size();
        const std::string family = m_state.font.family ? m_state.font.family : std::string();
        const std::wstring wide_family = utf8_to_wide(family);
        const DWRITE_FONT_WEIGHT weight = (m_state.font.flags & HELIOSVIEW_FONT_BOLD)
                                              ? DWRITE_FONT_WEIGHT_BOLD
                                              : DWRITE_FONT_WEIGHT_NORMAL;
        const DWRITE_FONT_STYLE style = (m_state.font.flags & HELIOSVIEW_FONT_ITALIC)
                                            ? DWRITE_FONT_STYLE_ITALIC
                                            : DWRITE_FONT_STYLE_NORMAL;
        const DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;

        auto make_format = [&](const wchar_t* name) -> ComPtr<IDWriteTextFormat> {
            ComPtr<IDWriteTextFormat> format;
            if (FAILED(dwrite->CreateTextFormat(name && *name ? name : L"Segoe UI", nullptr, weight,
                                                style, stretch, size, L"", &format))) {
                format.Reset();
                return format;
            }
            /* The painter lays a no-box run out without wrapping; NO_WRAP keeps the box
             * path from reflowing a string the caller sized itself. */
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            return format;
        };

        /* An unknown family is not an error: DirectWrite substitutes a face, which is
         * the documented contract for heliosview_font_desc::family. */
        if (!wide_family.empty())
            m_text_format = make_format(wide_family.c_str());
        if (!m_text_format)
            m_text_format = make_format(nullptr);
        if (!m_text_format)
            return;

        DWRITE_FONT_METRICS metrics{};
        if (wide_family.empty() ||
            !face_metrics(dwrite, wide_family, weight, stretch, style, &metrics)) {
            /* Nothing to ask, or a family DirectWrite had to substitute: fall back to
             * the usual proportions rather than reporting an empty line box. */
            m_ascent = size * 0.8f;
            m_descent = size * 0.2f;
            m_line_height = size * 1.2f;
            return;
        }
        const float scale = size / static_cast<float>(metrics.designUnitsPerEm);
        m_ascent = static_cast<float>(metrics.ascent) * scale;
        m_descent = static_cast<float>(metrics.descent) * scale;
        m_line_height = static_cast<float>(metrics.ascent + metrics.descent + metrics.lineGap) * scale;
    }

    static bool face_metrics(IDWriteFactory* dwrite, const std::wstring& family,
                             DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch,
                             DWRITE_FONT_STYLE style, DWRITE_FONT_METRICS* out)
    {
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(dwrite->GetSystemFontCollection(&collection)))
            return false;
        UINT32 index = 0;
        BOOL found = FALSE;
        if (FAILED(collection->FindFamilyName(family.c_str(), &index, &found)) || !found)
            return false;
        ComPtr<IDWriteFontFamily> font_family;
        if (FAILED(collection->GetFontFamily(index, &font_family)))
            return false;
        ComPtr<IDWriteFont> font;
        if (FAILED(font_family->GetFirstMatchingFont(weight, stretch, style, &font)))
            return false;
        ComPtr<IDWriteFontFace> face;
        if (FAILED(font->CreateFontFace(&face)))
            return false;
        face->GetMetrics(out);
        return out->designUnitsPerEm != 0;
    }

    float stroke_width() const { return m_state.stroke_width > 0.0f ? m_state.stroke_width : 1.0f; }

    float font_size() const { return m_state.font.size > 0.0f ? m_state.font.size : 12.0f; }

    static D2D1_CAP_STYLE d2d_cap(heliosview_line_cap_t cap)
    {
        switch (cap) {
        case HELIOSVIEW_CAP_ROUND: return D2D1_CAP_STYLE_ROUND;
        case HELIOSVIEW_CAP_SQUARE: return D2D1_CAP_STYLE_SQUARE;
        default: return D2D1_CAP_STYLE_FLAT; /* butt */
        }
    }

    static D2D1_LINE_JOIN d2d_join(heliosview_line_join_t join)
    {
        switch (join) {
        case HELIOSVIEW_JOIN_ROUND: return D2D1_LINE_JOIN_ROUND;
        case HELIOSVIEW_JOIN_BEVEL: return D2D1_LINE_JOIN_BEVEL;
        default: return D2D1_LINE_JOIN_MITER;
        }
    }

    static DWRITE_TEXT_ALIGNMENT text_alignment(uint32_t align)
    {
        if (align & HELIOSVIEW_ALIGN_HCENTER)
            return DWRITE_TEXT_ALIGNMENT_CENTER;
        if (align & HELIOSVIEW_ALIGN_RIGHT)
            return DWRITE_TEXT_ALIGNMENT_TRAILING;
        return DWRITE_TEXT_ALIGNMENT_LEADING;
    }

    static DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment(uint32_t align)
    {
        if (align & HELIOSVIEW_ALIGN_VCENTER)
            return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        if (align & HELIOSVIEW_ALIGN_BOTTOM)
            return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
        return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    }

    Rect canvas_bounds() const
    {
        return Rect{0.0f, 0.0f, static_cast<float>(m_data.width), static_cast<float>(m_data.height)};
    }

    CanvasAdapterImpl& m_adapter;
    const CanvasData& m_data;
    ComPtr<ID2D1RenderTarget> m_target;
    ComPtr<ID2D1SolidColorBrush> m_stroke;
    ComPtr<ID2D1SolidColorBrush> m_fill;
    ComPtr<ID2D1SolidColorBrush> m_text;
    ComPtr<ID2D1StrokeStyle> m_stroke_style;
    ComPtr<IDWriteTextFormat> m_text_format;
    heliosview_painter_state_t m_state{};
    bool m_state_dirty = true;
    bool m_drawing = false;
    bool m_valid = false;
    float m_transform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    /* The clip as the list of shapes it was built from, oldest first: replayed onto
     * the device at the start of every drawing call. */
    std::vector<Clip> m_clips;
    /* What begin() pushed, oldest first: the layer objects have to outlive their push,
     * and the pushes come off in reverse. */
    std::vector<ComPtr<ID2D1Layer>> m_pushed;
    /* The clip's device-space bounding box, which is all clip_bounds has to report. */
    Rect m_clip_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    float m_ascent = 0.0f;
    float m_descent = 0.0f;
    float m_line_height = 0.0f;
    std::vector<uint8_t> m_image_scratch;

    struct SavedD2dState {
        std::vector<Clip> clips;
        Rect clip_bounds;
    };
    std::vector<SavedD2dState> m_saved_clip_states;
};

/* ================= engine ================= */

class D2dEngine final : public Engine {
public:
    heliosview_canvas_engine_t id() const override { return HELIOSVIEW_ENGINE_D2D; }
    const char* name() const override { return "d2d"; }
    bool probe() override { return d2d_ready(); }
    heliosview_pixel_format_t preferred_format() const override
    {
        return HELIOSVIEW_FORMAT_BGRA8_PREMUL;
    }
    /* The twin is always the premultiplied BGRA Direct2D blends in, so every format is
     * served -- by a conversion on the way in and out, not natively. */
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
        /* No device resource is allocated here: the twin appears on the first drawing
         * call, so a canvas that is only filled, blitted or encoded costs nothing. */
        if (!d2d_ready() || !data.pixels || data.width <= 0 || data.height <= 0)
            return nullptr;
        return std::make_unique<CanvasAdapterImpl>(data.width, data.height);
    }

    std::unique_ptr<Context> create_context(CanvasAdapter& canvas, const CanvasData& data) override
    {
        auto* adapter = dynamic_cast<CanvasAdapterImpl*>(&canvas);
        if (!adapter)
            return nullptr;
        auto context = std::make_unique<ContextImpl>(*adapter, data);
        if (!context->valid())
            return nullptr;
        return context;
    }
};

D2dEngine g_engine;

/* Registered from a static initializer, the pattern the win32 backend uses for its
 * other process-wide hooks.
 *
 * Two names are claimed: the D2D vendor name and its concept name ACCELERATED, so
 * heliosview_engine_probe(ACCELERATED) answers for this engine. AUTO does not consult
 * ACCELERATED -- kAutoChain in heliosview_canvas.cpp is BUILTIN -> NATIVE -- so
 * registering here moves no AUTO canvas off GDI+; adding HELIOSVIEW_ENGINE_ACCELERATED
 * to kAutoChain is what would make D2D an AUTO candidate, and that is a behaviour
 * change rather than an implementation detail (this engine renders through a WIC
 * software target, see the note at the top, so it is not obviously the right default
 * for many small drawing calls).
 *
 * Wired into the build: this file is in src/CMakeLists.txt and d2d1/dwrite/
 * windowscodecs are on the win32 link line (cmake/PlatformLibs.cmake). */
const heliosview_canvas_engine_t g_d2d_names[] = {HELIOSVIEW_ENGINE_D2D, HELIOSVIEW_ENGINE_ACCELERATED};

struct D2dRegistration {
    D2dRegistration() { register_engine(&g_engine, g_d2d_names, 2); }
};

const D2dRegistration g_registration;

} // namespace
} // namespace hv::canvas
