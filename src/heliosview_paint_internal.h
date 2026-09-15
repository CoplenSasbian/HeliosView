#pragma once

/**
 * HeliosView internal paint layer: the engine interface and the shared state of a
 * canvas and a painter. NOT part of the public API (include/HeliosView/heliosview_paint.h
 * is), and never crossing the DLL boundary -- engines are C++ objects inside the
 * library, so plain virtual functions, std::vector and RAII are fine here.
 *
 * ==================== What an engine must provide ====================
 *
 * An engine turns painter calls into pixels written into a caller-owned buffer. It
 * does NOT own the pixels, does NOT know about windows, and does NOT do image
 * encoding: the canvas owns the memory (hv::paint::Canvas), and the codec
 * (heliosview_image_codec.cpp) handles PNG/JPEG/... for every engine alike. That
 * split is what keeps a new engine small and keeps formats identical everywhere.
 *
 * The contract in one line: for every canvas, wrap these pixels and draw into them;
 * the same drawing calls must produce the same geometry on every engine, with only
 * the quality of the rasterization differing (antialiasing, alpha, hinting).
 *
 * ==================== Adding an engine ====================
 *
 * 1. Implement Engine and Context (below) in one .cpp.
 * 2. Register it from a file-static object (hv::paint::register_engine), listing
 *    every name it answers to -- a concept name (NATIVE/ACCELERATED/SOFTWARE) and
 *    the vendor name(s) it is. Registration order is irrelevant: resolution is by
 *    a fixed preference order, not by insertion.
 * 3. Add the .cpp to src/CMakeLists.txt. Nothing else changes -- no public header
 *    edit, no new enum value beyond the reserved block in heliosview_paint.h.
 *
 * An engine that cannot be used in this process (an unavailable GPU, a missing
 * system component) must report it from probe(), not crash: the canvas creation
 * path fails the call with HELIOSVIEW_ERROR_UNSUPPORTED instead of substituting a
 * different engine, so "what you asked for is what you got" always holds. */

#include "heliosview_internal.h"

#include <HeliosView/heliosview_paint.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace hv::paint {

/* ================= Reusable path description =================
 *
 * The engine-neutral shape data every engine converts into its own path type. One
 * verb per entry, with the points that verb consumes:
 *
 *   StartNew  0 points  subpath begins at points[0] -- always first in a subpath
 *   LineTo    1 point
 *   QuadTo    2 points  (control, end)
 *   CubicTo   3 points  (control1, control2, end)
 *   Close     0 points  closes the current subpath
 *
 * The three shape verbs each carry their bounding rectangle in points[0..3] as
 * (x, y, width, height), with radius in points[4] for RoundRect. Whole shapes are
 * separate verbs instead of expanded curves because engines take them natively
 * (GDI+ AddEllipse/AddRectangle, Core Graphics, Cairo), which both matches the
 * rasterization of the direct shape calls and avoids a flattening step. */
enum PathVerb : uint8_t {
    PathVerbStartNew = 0,
    PathVerbLineTo = 1,
    PathVerbQuadTo = 2,
    PathVerbCubicTo = 3,
    PathVerbClose = 4,
    PathVerbRect = 5,
    PathVerbRoundRect = 6,
    PathVerbEllipse = 7,
};

struct PathEntry {
    uint8_t verb = PathVerbStartNew;
    uint8_t pad[3] = {};
    float pts[6] = {}; /* up to 3 (x, y) pairs, or (x, y, w, h, radius) for a shape */
    /* Which points are meaningful for the verb (values in pts are always finite) */
    static int point_count(uint8_t v)
    {
        switch (v) {
        case PathVerbLineTo: return 2;
        case PathVerbQuadTo: return 4;
        case PathVerbCubicTo: return 6;
        case PathVerbRect: return 4;
        case PathVerbRoundRect: return 5;
        case PathVerbEllipse: return 4;
        default: return 0;
        }
    }
};

struct PathData {
    std::vector<PathEntry> entries;
    heliosview_path_winding_t winding = HELIOSVIEW_WINDING_NONZERO;

    bool empty() const { return entries.empty(); }
    void clear() { entries.clear(); }
};

/* The public heliosview_path_t: a builder for PathData. Holds no engine resource.
 *
 * The opaque public types are DEFINED here, not in the headers: the public header
 * only forward-declares `struct heliosview_canvas` / `struct heliosview_painter` /
 * `struct heliosview_path`, so the definition must use exactly those names or the
 * C and C++ types would differ and every exported function would mismatch its
 * declaration. */
struct heliosview_path {
    PathData data;
};

/* ================= Engine geometry =================
 *
 * A rectangle and a color as the engines consume them: floats, straight (not
 * premultiplied) alpha, matching the public API exactly so no conversion happens
 * on this side of a call. */

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct Color {
    float r = 0, g = 0, b = 0, a = 0;
};

inline Color color_from_argb(uint32_t argb)
{
    Color c;
    c.a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    c.r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    c.g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    c.b = static_cast<float>(argb & 0xFF) / 255.0f;
    return c;
}

/* ================= Canvas memory layout =================
 *
 * The canvas's engine-independent half: the pixel buffer and its geometry. Engines
 * only ever see a CanvasData pointing at this storage. */

struct CanvasData {
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0; /* bytes per row; 4-byte aligned */
    heliosview_pixel_format_t format = HELIOSVIEW_FORMAT_BGRA8_PREMUL;
    uint8_t* pixels = nullptr;
};

/* Bytes per pixel for a concrete (non-AUTO) format; 0 for AUTO/invalid. */
int bytes_per_pixel(heliosview_pixel_format_t format);

/* Resolve AUTO to the format this build uses by default (BGRA8 premultiplied).
 * Returns 0 for a format that is not a concrete, supported format. */
heliosview_pixel_format_t resolve_format(heliosview_pixel_format_t format);

/* Copy/converting blit between two canvas layouts (no scaling, straight memcpy of
 * the rows when the formats match). Handles the premultiply/unpremultiply and the
 * channel order, so it is the one place that knows how the formats relate. */
void convert_copy(const CanvasData& src, Rect src_rect, const CanvasData& dst, int32_t dst_x,
                  int32_t dst_y, float alpha);

/* ================= The engine interface ================= */

class Context;

class CanvasAdapter {
public:
    virtual ~CanvasAdapter() = default;
    /* Whether the engine can hand out this buffer as its own (CONTEXT_FEATURE
     * DIRECT_PIXELS). False means canvas_data() returns NULL for this canvas. */
    virtual bool direct_pixels() const = 0;
    /* Called after the caller wrote through canvas_data(): drop any device copy. */
    virtual void reload() {}
    /* The native object a same-engine draw_image can reuse (a GDI+ Bitmap*, ...);
     * nullptr when the engine has none, in which case draw_image wraps the pixels. */
    virtual void* native_bitmap() { return nullptr; }
};

class Context {
public:
    virtual ~Context() = default;

    /* State. Called whenever the painter's state changes, before any drawing that
     * depends on it; engines may cache the translated objects (pen, brush, font). */
    virtual void set_state(const heliosview_painter_state_t& state) = 0;

    /* Transform: the 2x3 affine matrix [a b c d e f] (see heliosview_paint.h).
     * Replaces, never accumulates -- the painter owns the matrix. */
    virtual void set_transform(const float m[6]) = 0;

    /* Clipping. set_* REPLACES the clip, intersect_* narrows the existing one, and
     * reset_clip goes back to the whole canvas. Clip geometry is in the current
     * transform's space. */
    virtual void set_clip_rect(const Rect& r) = 0;
    virtual void set_clip_path(const PathData& path) = 0;
    virtual void intersect_clip_rect(const Rect& r) = 0;
    virtual void reset_clip() = 0;

    /* The current clip's device-space bounding box. It may be larger than the clip --
     * an engine only has to report a box that encloses it ("conservative"). */
    virtual void clip_bounds(Rect& out) const = 0;

    /* Shapes. Fill means fill_color, stroke means stroke_color/stroke_width; the
     * engine is told which to apply, the decision is the painter's. */
    virtual void clear(uint32_t argb) = 0;
    virtual void draw_line(float x1, float y1, float x2, float y2) = 0;
    virtual void draw_rect(const Rect& r) = 0;
    virtual void draw_round_rect(const Rect& r, float radius) = 0;
    virtual void draw_ellipse(const Rect& r) = 0;
    virtual void draw_arc(const Rect& r, float start_deg, float sweep_deg) = 0;
    virtual void draw_polyline(const float* points, size_t count, bool closed) = 0;
    virtual void draw_path(const PathData& path, bool fill, bool stroke) = 0;

    /* Text. `box` is (x, y, width, height) in the current transform's space; the
     * public alignment bits are passed through unchanged. */
    virtual void draw_text(const char* utf8, const Rect& box, uint32_t align) = 0;
    virtual void measure_text(const char* utf8, heliosview_text_metrics_t* out) = 0;

    /* An image: the source canvas's pixels (and its adapter, which may be nullptr
     * for a foreign engine) into the destination rectangle, under the current
     * transform/clip/state. src_rect is already clamped to the source. */
    virtual void draw_image(CanvasAdapter* src_adapter, const CanvasData& src, const Rect& src_rect,
                            const Rect& dst_rect, float alpha) = 0;

    /* Finish: flush pending work so the pixels in the buffer are final. */
    virtual void flush() = 0;
};

class Engine {
public:
    virtual ~Engine() = default;

    /* The primary enum value this engine answers to (its vendor name, e.g.
     * HELIOSVIEW_ENGINE_GDI_PLUS). Also the ordering key for AUTO resolution. */
    virtual heliosview_paint_engine_t id() const = 0;

    /* Short lowercase name for heliosview_engine_name ("gdi+", "cairo", ...) */
    virtual const char* name() const = 0;

    /* Can this engine create its resources in this process? False makes every canvas
     * creation on it fail with HELIOSVIEW_ERROR_UNSUPPORTED; it must not be a
     * "maybe" -- an engine that reports true and then fails create_canvas is a bug
     * in that engine, not a supported state. */
    virtual bool probe() = 0;

    /* Preferred pixel format when the caller passes AUTO. */
    virtual heliosview_pixel_format_t preferred_format() const = 0;

    /* Whether the engine can serve a concrete format at all (resolution is already
     * done by the caller). */
    virtual bool supports_format(heliosview_pixel_format_t format) const = 0;

    /* Capability query, answering heliosview_paint_feature_t values. */
    virtual bool supports_feature(int feature) const = 0;

    /* Wrap `data`'s pixels. Returns nullptr on failure (the engine must not keep a
     * copy of `data`: it lives in the caller's storage and moves with it). */
    virtual std::unique_ptr<CanvasAdapter> create_canvas(const CanvasData& data) = 0;

    /* Begin a drawing session on a canvas adapter created by this engine. */
    virtual std::unique_ptr<Context> create_context(CanvasAdapter& canvas, const CanvasData& data) = 0;
};

/* ================= Engine registry =================
 *
 * A small fixed table owned by the paint core (heliosview_paint.cpp). Engines add
 * themselves from a file-static object; see register_engine below. */

/* Register an engine under several names: `id` plus the concept names it
 * satisfies (HELIOSVIEW_ENGINE_NATIVE / _ACCELERATED / _SOFTWARE) and any
 * additional vendor value. Idempotent per (engine, name) pair. Returns false if the
 * table is full or a name is already taken by another engine. */
bool register_engine(Engine* engine, const heliosview_paint_engine_t* names, size_t name_count);

/* Convenience for the common case of one vendor id plus one concept name. */
bool register_engine(Engine* engine, heliosview_paint_engine_t vendor,
                     heliosview_paint_engine_t concept_name);

/* The registry itself (owned by heliosview_paint.cpp; the query functions read it
 * directly, so it is not hidden in an anonymous namespace). */
constexpr size_t kMaxEngineNames = 32;

struct EngineEntry {
    heliosview_paint_engine_t name = HELIOSVIEW_ENGINE_AUTO;
    Engine* engine = nullptr;
};

extern EngineEntry g_registry[kMaxEngineNames];
extern size_t g_registry_size;

/* The engine a bare HELIOSVIEW_ENGINE_AUTO resolves to first (set by
 * heliosview_set_default_paint_engine); AUTO means "use the resolution chain". */
extern heliosview_paint_engine_t g_default_engine;

/* The engine serving `engine`, after resolving AUTO and the concept names, with
 * probe() already confirmed. nullptr when nothing can serve it (the caller records
 * HELIOSVIEW_ERROR_UNSUPPORTED). */
Engine* resolve_engine(heliosview_paint_engine_t engine);

/* Registration-free variant used by the query functions: resolves without probing,
 * so a compiled-but-unavailable engine can still be named. */
Engine* find_engine(heliosview_paint_engine_t engine);

/* Registers an engine from a static initializer, the pattern the win32 backend uses
 * for other process-wide hooks (see heliosview_window_win32.cpp). */
struct EngineRegistration {
    EngineRegistration(Engine* engine, heliosview_paint_engine_t vendor,
                       heliosview_paint_engine_t concept_name)
    {
        register_engine(engine, vendor, concept_name);
    }
};

/* ================= Canvas and painter =================
 *
 * Both are completed here (the public headers only forward-declare them) because the
 * core and the window integration both need their layout. They keep the public
 * struct names on purpose: `heliosview_canvas_t` IS `::heliosview_canvas`, so the
 * exported C functions and their declarations are the same type.
 *
 * Inside this namespace the short aliases Canvas / Painter / PathBuilder name them,
 * which is what the implementation files use. */

using Canvas = ::heliosview_canvas;
using Painter = ::heliosview_painter;
using PathBuilder = ::heliosview_path;

/* The defaults heliosview_painter_begin installs (documented in the public header). */
heliosview_painter_state_t default_painter_state();

} // namespace hv::paint

/* ================= The opaque public types (definitions) =================
 *
 * These are what the public headers forward-declare, so they must be at global
 * scope with exactly these names. Everything platform- and engine-specific inside
 * them stays private to the implementation files. */

struct heliosview_canvas {
    hv::paint::CanvasData data;
    std::vector<uint8_t> storage; /* data.pixels points into this */
    hv::paint::Engine* engine = nullptr; /* never null on a live canvas */
    heliosview_paint_engine_t engine_id = HELIOSVIEW_ENGINE_AUTO;
    std::unique_ptr<hv::paint::CanvasAdapter> adapter;
    heliosview_painter_t* active_painter = nullptr; /* a canvas takes one painter at a time */

    void refresh_data();
};

struct heliosview_painter {
    heliosview_canvas_t* canvas = nullptr;
    std::unique_ptr<hv::paint::Context> context;
    heliosview_painter_state_t state{};
    /* Font family storage: state.font.family points here, so the caller's string
     * does not have to outlive set_font. */
    std::string font_family;
    float transform[6] = {1, 0, 0, 1, 0, 0};

    struct SavedState {
        heliosview_painter_state_t state;
        float transform[6];
    };
    std::vector<SavedState> stack;
};

struct heliosview_path {
    hv::paint::PathData data;
};

namespace hv::paint {

/* ================= Image codec (heliosview_image_codec.cpp) =================
 *
 * Shared by every engine: decoding/encoding never goes through an engine. */

/* Decode an image into a freshly allocated canvas of the requested format/engine.
 * Returns nullptr on failure, having recorded the reason (unsupported format, bad
 * data, ...). Two entry points, because the two inputs are not interchangeable. */
heliosview_canvas_t* codec_load_path(const char* path, heliosview_pixel_format_t format,
                                     heliosview_paint_engine_t engine);

/* `data` is the encoded image itself (PNG/JPEG/... bytes), not a path. */
heliosview_canvas_t* codec_load_memory(const void* data, size_t size,
                                       heliosview_pixel_format_t format,
                                       heliosview_paint_engine_t engine);

/* Encode a canvas' pixels into a growing byte buffer. `format` is the encoder
 * ("png"/"jpeg"/...), already normalized. Returns 0 on success. */
int codec_encode(const CanvasData& canvas, const char* format, int quality, uint32_t background,
                 std::vector<uint8_t>& out);

/* Whether the codec knows this format name (case-insensitive). `for_encoding`
 * selects the write table. */
bool codec_supports(const char* format, bool for_encoding);

/* Encoder name inferred from a path's extension ("a.PNG" -> "png"), or "" when the
 * extension is not a known encoder. */
std::string codec_format_from_path(const char* path);

} // namespace hv::paint
