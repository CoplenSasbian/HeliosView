// HeliosView -- platform-independent canvas core: the engine registry, the canvas
// (pixel storage + layout), the path builder, and the painter session that drives
// an engine's Context. Engine implementations live in src/<platform>/ and follow
// the contract in heliosview_canvas_internal.h.
//
// Nothing here knows about a platform, a window, or an image format: the pixels
// live here, the drawing goes through hv::canvas::Context, and the image codec is a
// separate translation unit shared by every engine.
//
// The public heliosview_* functions are defined at global scope with C linkage
// (they are declared inside extern "C" in the public header); everything else
// lives in hv::canvas.

#include "heliosview_canvas_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace hv::canvas {

/* ================= Format helpers ================= */

int bytes_per_pixel(heliosview_pixel_format_t format)
{
    switch (format) {
    case HELIOSVIEW_FORMAT_BGRA8_PREMUL:
    case HELIOSVIEW_FORMAT_BGRA8:
    case HELIOSVIEW_FORMAT_RGBA8:
        return 4;
    case HELIOSVIEW_FORMAT_GRAY8:
        return 1;
    default:
        return 0; /* AUTO or unknown */
    }
}

heliosview_pixel_format_t resolve_format(heliosview_pixel_format_t format)
{
    if (format == HELIOSVIEW_FORMAT_AUTO)
        return HELIOSVIEW_FORMAT_BGRA8_PREMUL; /* the format every engine serves natively */
    return bytes_per_pixel(format) ? format : HELIOSVIEW_FORMAT_AUTO;
}

/* Keeps the CanvasData in sync with the storage vector after a (re)allocation. A free
 * function rather than a member, because heliosview_canvas's members are defined at
 * global scope and this helper is only used in this file. */
void refresh_data(heliosview_canvas& canvas)
{
    canvas.data.pixels = canvas.storage.empty() ? nullptr : canvas.storage.data();
}

heliosview_painter_state_t default_canvas_state()
{
    heliosview_painter_state_t state{};
    state.stroke_color = 0; /* off */
    state.fill_color = 0;   /* off */
    state.stroke_width = 1.0f;
    state.line_cap = HELIOSVIEW_CAP_BUTT;
    state.line_join = HELIOSVIEW_JOIN_MITER;
    state.font.family = nullptr;
    state.font.size = 12.0f;
    state.font.flags = 0;
    state.alpha = 1.0f;
    state.antialias = 1;
    return state;
}

/* ================= Engine registry ================= */

EngineEntry g_registry[kMaxEngineNames];
size_t g_registry_size = 0;
heliosview_canvas_engine_t g_default_engine = HELIOSVIEW_ENGINE_AUTO;

namespace detail1 {
/* Preference order for HELIOSVIEW_ENGINE_AUTO: BUILTIN first (consistent cross-platform),
 * then NATIVE (OS native renderer). */
constexpr heliosview_canvas_engine_t kAutoChain[] = {
    HELIOSVIEW_ENGINE_BUILTIN,
    HELIOSVIEW_ENGINE_NATIVE,
};

} // namespace detail1

using namespace detail1; /* visible to the rest of hv::canvas */
bool register_engine(Engine* engine, const heliosview_canvas_engine_t* names, size_t name_count)
{
    if (!engine || !names || name_count == 0)
        return false;
    for (size_t i = 0; i < name_count; ++i) {
        if (g_registry_size >= kMaxEngineNames)
            return false;
        /* A name already bound to another engine is a registration bug: refuse it
         * rather than silently shadowing (which would make resolution depend on
         * registration order). */
        for (size_t j = 0; j < g_registry_size; ++j)
            if (g_registry[j].name == names[i] && g_registry[j].engine != engine)
                return false;
        g_registry[g_registry_size++] = EngineEntry{names[i], engine};
    }
    return true;
}

bool register_engine(Engine* engine, heliosview_canvas_engine_t vendor, heliosview_canvas_engine_t concept_name)
{
    const heliosview_canvas_engine_t names[2] = {vendor, concept_name};
    return register_engine(engine, names, 2);
}

Engine* find_engine(heliosview_canvas_engine_t engine)
{
    for (size_t i = 0; i < g_registry_size; ++i)
        if (g_registry[i].name == engine)
            return g_registry[i].engine;
    return nullptr;
}

Engine* resolve_engine(heliosview_canvas_engine_t engine)
{
    if (engine == HELIOSVIEW_ENGINE_AUTO)
        engine = g_default_engine; /* heliosview_set_default_canvas_engine overrides the chain */

    if (engine == HELIOSVIEW_ENGINE_AUTO) {
        for (const heliosview_canvas_engine_t candidate : kAutoChain)
            if (Engine* e = find_engine(candidate); e && e->probe())
                return e;
        return nullptr;
    }

    if (Engine* e = find_engine(engine); e && e->probe())
        return e;
    return nullptr;
}

} // namespace hv::canvas

using namespace hv::canvas;


/* ================= Color conversion =================
 *
 * One place knows how the canvas formats relate, so the pixel accessors and the
 * canvas-to-canvas copy agree with each other and with the engines. Values at this
 * boundary are STRAIGHT ARGB; premultiplication happens only where storage demands
 * it (here and inside each engine). */

namespace detail2 {
/* stride: bytes per row, 4-byte aligned (engines want an aligned pitch). */
int32_t stride_for(int32_t width, int bpp)
{
    return ((width * bpp) + 3) & ~3;
}

inline uint8_t unpremul(uint8_t v, uint8_t a)
{
    if (a == 0)
        return 0;
    if (a == 255)
        return v;
    const unsigned r = (static_cast<unsigned>(v) * 255u + a / 2u) / a;
    return static_cast<uint8_t>(r > 255u ? 255u : r);
}

inline uint8_t premul(uint8_t v, uint8_t a)
{
    return static_cast<uint8_t>((static_cast<unsigned>(v) * a + 127u) / 255u);
}

/* Rec. 601 luma, the usual 8-bit gray conversion */
inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint8_t>((static_cast<unsigned>(r) * 77u + static_cast<unsigned>(g) * 150u
                                 + static_cast<unsigned>(b) * 29u)
                                >> 8);
}

uint32_t read_pixel(const CanvasData& c, int32_t x, int32_t y)
{
    const uint8_t* p = c.pixels + static_cast<size_t>(y) * static_cast<size_t>(c.stride)
                     + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel(c.format));
    switch (c.format) {
    case HELIOSVIEW_FORMAT_BGRA8_PREMUL: {
        const uint8_t a = p[3];
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(unpremul(p[2], a)) << 16)
             | (static_cast<uint32_t>(unpremul(p[1], a)) << 8) | static_cast<uint32_t>(unpremul(p[0], a));
    }
    case HELIOSVIEW_FORMAT_BGRA8:
        return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
    case HELIOSVIEW_FORMAT_RGBA8:
        return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[0]) << 16)
             | (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[2]);
    case HELIOSVIEW_FORMAT_GRAY8: {
        const uint8_t g = p[0];
        return 0xFF000000u | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(g) << 8) | g;
    }
    default:
        return 0;
    }
}

void write_pixel(const CanvasData& c, int32_t x, int32_t y, uint32_t argb, float alpha = 1.0f)
{
    uint8_t* p = c.pixels + static_cast<size_t>(y) * static_cast<size_t>(c.stride)
               + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel(c.format));
    const uint8_t a0 = static_cast<uint8_t>((argb >> 24) & 0xFF);
    const uint8_t a = static_cast<uint8_t>(
        std::clamp(static_cast<float>(a0) * alpha + 0.5f, 0.0f, 255.0f));
    const uint8_t r = static_cast<uint8_t>((argb >> 16) & 0xFF);
    const uint8_t g = static_cast<uint8_t>((argb >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>(argb & 0xFF);
    switch (c.format) {
    case HELIOSVIEW_FORMAT_BGRA8_PREMUL:
        p[0] = premul(b, a);
        p[1] = premul(g, a);
        p[2] = premul(r, a);
        p[3] = a;
        break;
    case HELIOSVIEW_FORMAT_BGRA8:
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
        break;
    case HELIOSVIEW_FORMAT_RGBA8:
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = a;
        break;
    case HELIOSVIEW_FORMAT_GRAY8:
        p[0] = luma(r, g, b);
        break;
    default:
        break;
    }
}

/* Source-over blend of a straight ARGB value onto the destination pixel. */
void blend_pixel(const CanvasData& dst, int32_t x, int32_t y, uint32_t src_argb, float alpha)
{
    const float sa = static_cast<float>((src_argb >> 24) & 0xFF) / 255.0f * alpha;
    if (sa <= 0.0f)
        return;
    if (sa >= 1.0f) {
        write_pixel(dst, x, y, src_argb, alpha); /* opaque source: a plain overwrite */
        return;
    }
    const uint32_t dp = read_pixel(dst, x, y);
    const float da = static_cast<float>((dp >> 24) & 0xFF) / 255.0f;
    const float out_a = sa + da * (1.0f - sa);
    auto mix = [&](int shift) {
        const float sc = static_cast<float>((src_argb >> shift) & 0xFF) / 255.0f;
        const float dc = static_cast<float>((dp >> shift) & 0xFF) / 255.0f;
        const float oc = out_a > 0.0f ? (sc * sa + dc * da * (1.0f - sa)) / out_a : 0.0f;
        return static_cast<unsigned>(std::clamp(oc * 255.0f + 0.5f, 0.0f, 255.0f));
    };
    const uint32_t out = (static_cast<uint32_t>(out_a * 255.0f + 0.5f) << 24) | (mix(16) << 16)
                       | (mix(8) << 8) | mix(0);
    write_pixel(dst, x, y, out);
}

} // namespace detail2

using namespace detail2; /* visible to the rest of hv::canvas */
void hv::canvas::convert_copy(const CanvasData& src, Rect src_rect, const CanvasData& dst, int32_t dst_x,
                             int32_t dst_y, float alpha)
{
    const int32_t w = static_cast<int32_t>(src_rect.w);
    const int32_t h = static_cast<int32_t>(src_rect.h);
    if (w <= 0 || h <= 0 || !src.pixels || !dst.pixels)
        return;
    const int32_t sx0 = static_cast<int32_t>(src_rect.x);
    const int32_t sy0 = static_cast<int32_t>(src_rect.y);

    for (int32_t row = 0; row < h; ++row) {
        const int32_t dy = dst_y + row;
        if (dy < 0 || dy >= dst.height)
            continue;
        const int32_t sy = sy0 + row;
        if (sy < 0 || sy >= src.height)
            continue;
        for (int32_t col = 0; col < w; ++col) {
            const int32_t dx = dst_x + col;
            if (dx < 0 || dx >= dst.width)
                continue;
            const int32_t sx = sx0 + col;
            if (sx < 0 || sx >= src.width)
                continue;
            blend_pixel(dst, dx, dy, read_pixel(src, sx, sy), alpha);
        }
    }
}

namespace detail3 {
/* ================= Shared helpers for the C entry points =================
 *
 * These live in the namespace (not an anonymous one): the exported C functions are
 * defined at global scope, and a name in an anonymous namespace cannot be reached by
 * qualified lookup from there. They are not exported (internal linkage would be the
 * only thing an anonymous namespace added, and the header does not declare them). */

Engine* engine_for_canvas(heliosview_canvas_engine_t requested)
{
    if (Engine* e = resolve_engine(requested))
        return e;
    if (requested == HELIOSVIEW_ENGINE_AUTO)
        hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "no canvas engine is available on this platform");
    else
        hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "the requested canvas engine is not available");
    return nullptr;
}

heliosview_canvas_t* canvas_alloc(int32_t width, int32_t height, heliosview_pixel_format_t format,
                                  heliosview_canvas_engine_t engine_id)
{
    if (width <= 0 || height <= 0) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas size must be positive");
        return nullptr;
    }
    Engine* engine = engine_for_canvas(engine_id);
    if (!engine)
        return nullptr;

    const heliosview_pixel_format_t concrete = resolve_format(format);
    if (concrete == HELIOSVIEW_FORMAT_AUTO || !engine->supports_format(concrete)) {
        hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "the engine does not support the requested pixel format");
        return nullptr;
    }

    Canvas* canvas = nullptr;
    try {
        canvas = hv::hv_alloc<Canvas>();
        canvas->data.width = width;
        canvas->data.height = height;
        canvas->data.format = concrete;
        canvas->data.stride = stride_for(width, bytes_per_pixel(concrete));
        canvas->storage.assign(static_cast<size_t>(canvas->data.stride) * static_cast<size_t>(height), 0);
        canvas->data.pixels = canvas->storage.data();
        canvas->engine = engine;
        canvas->engine_id = engine->id();
        canvas->adapter = engine->create_canvas(canvas->data);
        if (!canvas->adapter) {
            hv::hv_dealloc(canvas);
            hv_fail(HELIOSVIEW_ERROR_GENERIC, "the canvas engine could not wrap the canvas pixels");
            return nullptr;
        }
    } catch (const std::bad_alloc&) {
        if (canvas)
            hv::hv_dealloc(canvas);
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory allocating the canvas");
        return nullptr;
    }
    return canvas;
}

Painter* painter_checked(heliosview_painter_t* painter, const char* what)
{
    if (!painter)
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, what);
    return painter;
}

bool has_alpha(uint32_t argb)
{
    return ((argb >> 24) & 0xFF) != 0;
}

/* Apply a modified state for the duration of one drawing call, then put the
 * painter's state back. Used by the "explicit color" calls (fill_rect,
 * fill_path, ...) which draw with a color that is not in the state. */
struct ScopedState {
    Painter* p;
    heliosview_painter_state_t original;
    explicit ScopedState(Painter* painter, const heliosview_painter_state_t& temporary)
        : p(painter), original(painter->state)
    {
        p->context->set_state(temporary);
    }
    ~ScopedState() { p->context->set_state(original); }
    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;
};

/* Build the state a shape call should draw with: whichever of fill/stroke the
 * painter has enabled. Returns false when neither is on (nothing to draw). */
bool shape_state(const Painter* p, heliosview_painter_state_t* out)
{
    const bool fill = has_alpha(p->state.fill_color);
    const bool stroke = has_alpha(p->state.stroke_color);
    if (!fill && !stroke)
        return false;
    *out = p->state;
    if (!fill)
        out->fill_color = 0;
    if (!stroke)
        out->stroke_color = 0;
    return true;
}

} // namespace detail3

using namespace detail3; /* visible to the rest of hv::canvas */

/* The helper block above is the last one inside hv::canvas; close it here. */


/* ================= Public C API: engine queries ================= */

extern "C" {

int heliosview_engine_count(void)
{
    /* Count distinct engines, not names: an engine registered under a concept name
     * and a vendor name is one engine. */
    int count = 0;
    hv::canvas::Engine* seen[hv::canvas::kMaxEngineNames];
    for (size_t i = 0; i < hv::canvas::g_registry_size; ++i) {
        bool known = false;
        for (int j = 0; j < count; ++j)
            if (seen[j] == hv::canvas::g_registry[i].engine)
                known = true;
        if (!known)
            seen[count++] = hv::canvas::g_registry[i].engine;
    }
    return count;
}

heliosview_canvas_engine_t heliosview_engine_at(int index)
{
    if (index < 0)
        return HELIOSVIEW_ENGINE_AUTO;
    hv::canvas::Engine* seen[hv::canvas::kMaxEngineNames];
    int count = 0;
    for (size_t i = 0; i < hv::canvas::g_registry_size; ++i) {
        bool known = false;
        for (int j = 0; j < count; ++j)
            if (seen[j] == hv::canvas::g_registry[i].engine)
                known = true;
        if (!known) {
            if (count == index)
                return hv::canvas::g_registry[i].engine->id();
            seen[count++] = hv::canvas::g_registry[i].engine;
        }
    }
    return HELIOSVIEW_ENGINE_AUTO;
}

int heliosview_engine_compiled(heliosview_canvas_engine_t engine)
{
    if (engine == HELIOSVIEW_ENGINE_AUTO)
        return heliosview_engine_count() > 0 ? 1 : 0;
    return hv::canvas::find_engine(engine) ? 1 : 0;
}

int heliosview_engine_probe(heliosview_canvas_engine_t engine)
{
    return hv::canvas::resolve_engine(engine) ? 1 : 0;
}

const char* heliosview_engine_name(heliosview_canvas_engine_t engine)
{
    if (engine == HELIOSVIEW_ENGINE_AUTO)
        return "auto";
    if (hv::canvas::Engine* e = hv::canvas::find_engine(engine))
        return e->name();
    switch (engine) {
    case HELIOSVIEW_ENGINE_BUILTIN: return "builtin";
    case HELIOSVIEW_ENGINE_NATIVE: return "native";
    case HELIOSVIEW_ENGINE_ACCELERATED: return "accelerated";
    case HELIOSVIEW_ENGINE_GDI: return "gdi";
    case HELIOSVIEW_ENGINE_GDI_PLUS: return "gdi+";
    case HELIOSVIEW_ENGINE_D2D: return "d2d";
    case HELIOSVIEW_ENGINE_BLEND2D: return "blend2d";
    case HELIOSVIEW_ENGINE_CORE_GRAPHICS: return "coregraphics";
    case HELIOSVIEW_ENGINE_METAL: return "metal";
    case HELIOSVIEW_ENGINE_CAIRO: return "cairo";
    default: return "unknown";
    }
}

int heliosview_engine_supports_feature(heliosview_canvas_engine_t engine, heliosview_canvas_feature_t feature)
{
    hv::canvas::Engine* e = hv::canvas::resolve_engine(engine);
    if (!e)
        return 0;
    return e->supports_feature(static_cast<int>(feature)) ? 1 : 0;
}

void heliosview_set_default_canvas_engine(heliosview_canvas_engine_t engine)
{
    hv::canvas::g_default_engine = engine;
}

heliosview_canvas_engine_t heliosview_default_canvas_engine(void)
{
    return hv::canvas::g_default_engine;
}

/* ================= Public C API: canvas ================= */

heliosview_canvas_t* heliosview_canvas_create(int32_t width, int32_t height,
                                              heliosview_pixel_format_t format,
                                              heliosview_canvas_engine_t engine)
{
    return canvas_alloc(width, height, format, engine);
}

heliosview_canvas_t* heliosview_canvas_clone(const heliosview_canvas_t* canvas,
                                             heliosview_pixel_format_t format,
                                             heliosview_canvas_engine_t engine)
{
    if (!canvas) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
        return nullptr;
    }
    const heliosview_pixel_format_t want_format =
        format == HELIOSVIEW_FORMAT_AUTO ? canvas->data.format : format;
    const heliosview_canvas_engine_t want_engine =
        engine == HELIOSVIEW_ENGINE_AUTO ? canvas->engine_id : engine;

    heliosview_canvas_t* copy = heliosview_canvas_create(canvas->data.width, canvas->data.height,
                                                         want_format, want_engine);
    if (!copy)
        return nullptr;
    const hv::canvas::Rect all{0, 0, static_cast<float>(canvas->data.width),
                              static_cast<float>(canvas->data.height)};
    hv::canvas::convert_copy(canvas->data, all, copy->data, 0, 0, 1.0f);
    if (copy->adapter)
        copy->adapter->reload(); /* the pixels were written behind the engine's back */
    return copy;
}

void heliosview_canvas_destroy(heliosview_canvas_t* canvas)
{
    if (!canvas)
        return;
    heliosview_painter_destroy(canvas->active_painter); /* a live painter holds a Context */
    canvas->adapter.reset();
    hv::hv_dealloc(canvas);
}

int heliosview_canvas_size(const heliosview_canvas_t* canvas, int32_t* out_width, int32_t* out_height)
{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (out_width)
        *out_width = canvas->data.width;
    if (out_height)
        *out_height = canvas->data.height;
    return 0;
}

int32_t heliosview_canvas_stride(const heliosview_canvas_t* canvas)
{
    return canvas ? canvas->data.stride : 0;
}

heliosview_pixel_format_t heliosview_canvas_format(const heliosview_canvas_t* canvas)
{
    return canvas ? canvas->data.format : HELIOSVIEW_FORMAT_AUTO;
}

heliosview_canvas_engine_t heliosview_canvas_engine(const heliosview_canvas_t* canvas)
{
    return canvas ? canvas->engine_id : HELIOSVIEW_ENGINE_AUTO;
}

void* heliosview_canvas_data(heliosview_canvas_t* canvas)
{
    if (!canvas) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
        return nullptr;
    }
    if (canvas->adapter && !canvas->adapter->direct_pixels()) {
        hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED,
                "this engine does not expose its pixels directly; use set_pixel/get_pixel");
        return nullptr;
    }
    return canvas->data.pixels;
}

heliosview_pixel_view_t heliosview_canvas_pixel_view(const heliosview_canvas_t* canvas)
{
    if (!canvas)
        return heliosview_pixel_view_t{nullptr, 0, 0, 0, HELIOSVIEW_FORMAT_AUTO};
    return heliosview_pixel_view_t{
        canvas->data.pixels,
        canvas->data.width,
        canvas->data.height,
        canvas->data.stride,
        canvas->data.format
    };
}

int heliosview_canvas_end_write(heliosview_canvas_t* canvas)

{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (canvas->adapter)
        canvas->adapter->reload();
    return 0;
}

int heliosview_canvas_resize(heliosview_canvas_t* canvas, int32_t width, int32_t height)
{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (width <= 0 || height <= 0)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas size must be positive");
    if (canvas->active_painter)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "a painter is active on the canvas");

    canvas->data.width = width;
    canvas->data.height = height;
    canvas->data.stride = stride_for(width, hv::canvas::bytes_per_pixel(canvas->data.format));
    try {
        canvas->storage.assign(static_cast<size_t>(canvas->data.stride) * static_cast<size_t>(height), 0);
    } catch (const std::bad_alloc&) {
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory resizing the canvas");
    }
    canvas->data.pixels = canvas->storage.data();
    /* The pixels moved: the engine must be handed the new buffer. */
    canvas->adapter = canvas->engine->create_canvas(canvas->data);
    if (!canvas->adapter)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "the canvas engine could not wrap the resized canvas");
    return 0;
}

int heliosview_canvas_fill(heliosview_canvas_t* canvas, uint32_t argb)
{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (canvas->active_painter)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "a painter is active on the canvas; use painter_clear");

    const int bpp = hv::canvas::bytes_per_pixel(canvas->data.format);
    for (int32_t y = 0; y < canvas->data.height; ++y) {
        uint8_t* row = canvas->data.pixels + static_cast<size_t>(y) * static_cast<size_t>(canvas->data.stride);
        if (bpp == 4) {
            /* One conversion per row: splat the resulting word. */
            uint32_t word = 0;
            hv::canvas::CanvasData probe{1, 1, 4, canvas->data.format, reinterpret_cast<uint8_t*>(&word)};
            write_pixel(probe, 0, 0, argb);
            for (int32_t x = 0; x < canvas->data.width; ++x)
                std::memcpy(row + static_cast<size_t>(x) * 4, &word, 4);
        } else {
            const uint8_t g = luma(static_cast<uint8_t>((argb >> 16) & 0xFF),
                                   static_cast<uint8_t>((argb >> 8) & 0xFF),
                                   static_cast<uint8_t>(argb & 0xFF));
            std::memset(row, g, static_cast<size_t>(canvas->data.width));
        }
    }
    if (canvas->adapter)
        canvas->adapter->reload();
    return 0;
}

int heliosview_canvas_set_pixel(heliosview_canvas_t* canvas, int32_t x, int32_t y, uint32_t argb)
{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (x < 0 || y < 0 || x >= canvas->data.width || y >= canvas->data.height)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "pixel is outside the canvas");
    write_pixel(canvas->data, x, y, argb);
    if (canvas->adapter)
        canvas->adapter->reload();
    return 0;
}

int heliosview_canvas_get_pixel(const heliosview_canvas_t* canvas, int32_t x, int32_t y, uint32_t* out_argb)
{
    if (!canvas || !out_argb)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas or out_argb is NULL");
    if (x < 0 || y < 0 || x >= canvas->data.width || y >= canvas->data.height)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "pixel is outside the canvas");
    *out_argb = read_pixel(canvas->data, x, y);
    return 0;
}

int heliosview_canvas_blit(heliosview_canvas_t* src, heliosview_canvas_t* dst, int32_t x, int32_t y,
                           const heliosview_rect_t* src_rect, float alpha)
{
    if (!src || !dst)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "src or dst canvas is NULL");
    hv::canvas::Rect region{0, 0, static_cast<float>(src->data.width), static_cast<float>(src->data.height)};
    if (src_rect)
        region = hv::canvas::Rect{static_cast<float>(src_rect->x), static_cast<float>(src_rect->y),
                                 static_cast<float>(src_rect->width), static_cast<float>(src_rect->height)};
    hv::canvas::convert_copy(src->data, region, dst->data, x, y, std::clamp(alpha, 0.0f, 1.0f));
    if (dst->adapter)
        dst->adapter->reload();
    return 0;
}

/* ================= Public C API: images ================= */

heliosview_canvas_t* heliosview_canvas_load(const char* path, heliosview_pixel_format_t format,
                                            heliosview_canvas_engine_t engine)
{
    if (!path || !*path) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL or empty");
        return nullptr;
    }
    return hv::canvas::codec_load_path(path, format, engine);
}

heliosview_canvas_t* heliosview_canvas_load_memory(const void* data, size_t size,
                                                   heliosview_pixel_format_t format,
                                                   heliosview_canvas_engine_t engine)
{
    if (!data || size == 0) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "data is NULL or size is 0");
        return nullptr;
    }
    return hv::canvas::codec_load_memory(data, size, format, engine);
}

int heliosview_canvas_encode(heliosview_canvas_t* canvas, const char* format, int quality,
                             uint32_t background, void** out_data, size_t* out_size)
{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (!format || !*format)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "format is NULL or empty");
    if (canvas->active_painter)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "a painter is active on the canvas");
    if (out_data)
        *out_data = nullptr;
    if (out_size)
        *out_size = 0;

    std::vector<uint8_t> bytes;
    const int rc = hv::canvas::codec_encode(canvas->data, format, quality, background, bytes);
    if (rc != 0)
        return rc;
    if (out_size)
        *out_size = bytes.size();
    if (out_data) {
        void* buffer = hv::g_allocator.alloc
                           ? hv::g_allocator.alloc(bytes.size(), hv::g_allocator.context)
                           : std::malloc(bytes.size());
        if (!buffer)
            return hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory for the encoded image");
        if (!bytes.empty())
            std::memcpy(buffer, bytes.data(), bytes.size());
        *out_data = buffer;
    }
    return static_cast<int>(bytes.size());
}

int heliosview_canvas_save(heliosview_canvas_t* canvas, const char* path, const char* format,
                           int quality, uint32_t background)
{
    if (!canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
    if (!path || !*path)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL or empty");
    if (canvas->active_painter)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "a painter is active on the canvas");

    const std::string encoder = format && *format ? std::string(format) : hv::canvas::codec_format_from_path(path);
    if (encoder.empty())
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT,
                       "cannot infer the image format from the path extension");
    if (!hv::canvas::codec_supports(encoder.c_str(), /*for_encoding=*/true))
        return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "the image format cannot be written by the codec");

    std::vector<uint8_t> bytes;
    const int rc = hv::canvas::codec_encode(canvas->data, encoder.c_str(), quality, background, bytes);
    if (rc != 0)
        return rc;

    std::FILE* file = std::fopen(path, "wb");
    if (!file)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "cannot open the output file for writing");
    const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool ok = written == bytes.size();
    std::fclose(file);
    if (!ok)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "writing the image file failed");
    return 0;
}

int heliosview_format_supported(const char* format, int for_encoding)
{
    if (!format || !*format)
        return 0;
    return hv::canvas::codec_supports(format, for_encoding != 0) ? 1 : 0;
}

/* ================= Public C API: painters ================= */

heliosview_painter_t* heliosview_painter_begin(heliosview_canvas_t* canvas)
{
    if (!canvas) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "canvas is NULL");
        return nullptr;
    }
    if (canvas->active_painter) {
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "the canvas already has a painter; end it first");
        return nullptr;
    }
    heliosview_painter_t* painter = nullptr;
    try {
        painter = hv::hv_alloc<hv::canvas::Painter>();
        painter->canvas = canvas;
        painter->context = canvas->engine->create_context(*canvas->adapter, canvas->data);
        if (!painter->context) {
            hv::hv_dealloc(painter);
            hv_fail(HELIOSVIEW_ERROR_GENERIC, "the canvas engine could not start a drawing session");
            return nullptr;
        }
        painter->state = hv::canvas::default_canvas_state();
        painter->context->set_state(painter->state);
        painter->context->set_transform(painter->transform);
        canvas->active_painter = painter;
    } catch (const std::bad_alloc&) {
        if (painter)
            hv::hv_dealloc(painter);
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory starting the painter");
        return nullptr;
    }
    return painter;
}

int heliosview_painter_end(heliosview_painter_t* painter)
{
    if (!painter)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "painter is NULL");
    if (painter->context)
        painter->context->flush();
    heliosview_painter_destroy(painter);
    return 0;
}

void heliosview_painter_destroy(heliosview_painter_t* painter)
{
    if (!painter)
        return;
    if (painter->canvas && painter->canvas->active_painter == painter)
        painter->canvas->active_painter = nullptr;
    painter->context.reset();
    hv::hv_dealloc(painter);
}

int heliosview_painter_set_state(heliosview_painter_t* painter, const heliosview_painter_state_t* state)
{
    if (!painter || !state)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "painter or state is NULL");
    heliosview_painter_state_t next = *state;
    /* Normalize the documented defaults for the fields that have one. */
    if (next.font.size <= 0.0f)
        next.font.size = 12.0f;
    if (next.stroke_width <= 0.0f)
        next.stroke_width = 1.0f;
    next.alpha = std::clamp(next.alpha, 0.0f, 1.0f);
    if (next.font.family && *next.font.family)
        painter->font_family = next.font.family;
    else
        painter->font_family.clear();
    next.font.family = painter->font_family.empty() ? nullptr : painter->font_family.c_str();
    painter->state = next;
    painter->context->set_state(painter->state);
    return 0;
}

int heliosview_painter_get_state(const heliosview_painter_t* painter, heliosview_painter_state_t* out_state)
{
    if (!painter || !out_state)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "painter or out_state is NULL");
    *out_state = painter->state;
    return 0;
}

int heliosview_painter_set_stroke(heliosview_painter_t* painter, uint32_t argb, float width)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->state.stroke_color = argb;
    painter->state.stroke_width = width > 0.0f ? width : 1.0f;
    painter->context->set_state(painter->state);
    return 0;
}

int heliosview_painter_set_fill(heliosview_painter_t* painter, uint32_t argb)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->state.fill_color = argb;
    painter->context->set_state(painter->state);
    return 0;
}

int heliosview_painter_set_font(heliosview_painter_t* painter, const heliosview_font_desc_t* desc)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!desc)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "font description is NULL");
    heliosview_painter_state_t next = painter->state;
    next.font = *desc;
    if (next.font.size <= 0.0f)
        next.font.size = 12.0f;
    if (next.font.family && *next.font.family)
        painter->font_family = next.font.family;
    else
        painter->font_family.clear();
    next.font.family = painter->font_family.empty() ? nullptr : painter->font_family.c_str();
    painter->state = next;
    painter->context->set_state(painter->state);
    return 0;
}

int heliosview_painter_set_alpha(heliosview_painter_t* painter, float alpha)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->state.alpha = std::clamp(alpha, 0.0f, 1.0f);
    painter->context->set_state(painter->state);
    return 0;
}

int heliosview_painter_set_antialias(heliosview_painter_t* painter, int on)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->state.antialias = on ? 1 : 0;
    painter->context->set_state(painter->state);
    return 0;
}

/* ================= Public C API: transform ================= */

namespace detail4 {
/* current = current * extra, both 2x3 affine matrices in [a b c d e f] form
 * (row vector: x' = a*x + c*y + e, y' = b*x + d*y + f). */
void matrix_multiply(float* current, const float* extra)
{
    const float a = current[0], b = current[1], c = current[2], d = current[3], e = current[4],
                f = current[5];
    const float na = extra[0], nb = extra[1], nc = extra[2], nd = extra[3], ne = extra[4],
                nf = extra[5];
    current[0] = a * na + c * nb;
    current[1] = b * na + d * nb;
    current[2] = a * nc + c * nd;
    current[3] = b * nc + d * nd;
    current[4] = a * ne + c * nf + e;
    current[5] = b * ne + d * nf + f;
}

int painter_apply_matrix(heliosview_painter_t* painter, const float extra[6])
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    matrix_multiply(painter->transform, extra);
    painter->context->set_transform(painter->transform);
    return 0;
}

} // namespace detail4

using namespace detail4; /* visible to the rest of hv::canvas */


int heliosview_painter_set_transform(heliosview_painter_t* painter, const float* m)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!m)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "matrix is NULL");
    std::memcpy(painter->transform, m, sizeof(painter->transform));
    painter->context->set_transform(painter->transform);
    return 0;
}

int heliosview_painter_get_transform(const heliosview_painter_t* painter, float* out_m)
{
    if (!painter || !out_m)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "painter or out_m is NULL");
    std::memcpy(out_m, painter->transform, sizeof(painter->transform));
    return 0;
}

int heliosview_painter_translate(heliosview_painter_t* painter, float dx, float dy)
{
    const float m[6] = {1, 0, 0, 1, dx, dy};
    return painter_apply_matrix(painter, m);
}

int heliosview_painter_scale(heliosview_painter_t* painter, float sx, float sy)
{
    const float m[6] = {sx, 0, 0, sy, 0, 0};
    return painter_apply_matrix(painter, m);
}

int heliosview_painter_rotate(heliosview_painter_t* painter, float radians)
{
    const float s = std::sin(radians);
    const float c = std::cos(radians);
    const float m[6] = {c, s, -s, c, 0, 0};
    return painter_apply_matrix(painter, m);
}

int heliosview_painter_skew(heliosview_painter_t* painter, float kx, float ky)
{
    const float m[6] = {1, ky, kx, 1, 0, 0};
    return painter_apply_matrix(painter, m);
}

int heliosview_painter_reset_transform(heliosview_painter_t* painter)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    const float identity[6] = {1, 0, 0, 1, 0, 0};
    std::memcpy(painter->transform, identity, sizeof(identity));
    painter->context->set_transform(painter->transform);
    return 0;
}

/* ================= Public C API: clipping ================= */

int heliosview_painter_set_clip_rect(heliosview_painter_t* painter, float x, float y, float width,
                                     float height)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->context->set_clip_rect(hv::canvas::Rect{x, y, width, height});
    return 0;
}

int heliosview_painter_set_clip_path(heliosview_painter_t* painter, heliosview_path_t* path)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!path)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL");
    painter->context->set_clip_path(path->data);
    return 0;
}

int heliosview_painter_intersect_clip_rect(heliosview_painter_t* painter, float x, float y,
                                           float width, float height)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->context->intersect_clip_rect(hv::canvas::Rect{x, y, width, height});
    return 0;
}

int heliosview_painter_reset_clip(heliosview_painter_t* painter)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->context->reset_clip();
    return 0;
}

int heliosview_painter_clip_bounds(const heliosview_painter_t* painter, heliosview_rect_t* out_rect)
{
    if (!painter || !out_rect)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "painter or out_rect is NULL");
    Rect bounds{};
    painter->context->clip_bounds(bounds);
    /* The clip must be enclosed, so outward: min values round down, max values up. */
    out_rect->x = static_cast<int32_t>(std::floor(bounds.x));
    out_rect->y = static_cast<int32_t>(std::floor(bounds.y));
    out_rect->width = static_cast<int32_t>(std::ceil(bounds.x + bounds.w)) - out_rect->x;
    out_rect->height = static_cast<int32_t>(std::ceil(bounds.y + bounds.h)) - out_rect->y;
    return 0;
}

/* ================= Public C API: shapes ================= */

int heliosview_painter_clear(heliosview_painter_t* painter, uint32_t argb)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    painter->context->clear(argb);
    return 0;
}

int heliosview_painter_fill_rect(heliosview_painter_t* painter, float x, float y, float width,
                                 float height, uint32_t argb)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!has_alpha(argb))
        return 0;
    heliosview_painter_state_t s = painter->state;
    s.fill_color = argb;
    s.stroke_color = 0;
    {
        ScopedState guard(painter, s);
        painter->context->set_transform(painter->transform);
        painter->context->draw_rect(hv::canvas::Rect{x, y, width, height});
    }
    return 0;
}

int heliosview_painter_draw_line(heliosview_painter_t* painter, float x1, float y1, float x2, float y2)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!has_alpha(painter->state.stroke_color))
        return 0;
    heliosview_painter_state_t s = painter->state;
    s.fill_color = 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_line(x1, y1, x2, y2);
    return 0;
}

int heliosview_painter_draw_rect(heliosview_painter_t* painter, float x, float y, float width, float height)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    heliosview_painter_state_t s{};
    if (!shape_state(painter, &s))
        return 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_rect(hv::canvas::Rect{x, y, width, height});
    return 0;
}

int heliosview_painter_draw_round_rect(heliosview_painter_t* painter, float x, float y, float width,
                                       float height, float radius)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    heliosview_painter_state_t s{};
    if (!shape_state(painter, &s))
        return 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_round_rect(hv::canvas::Rect{x, y, width, height}, radius);
    return 0;
}

int heliosview_painter_draw_ellipse(heliosview_painter_t* painter, float x, float y, float width,
                                    float height)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    heliosview_painter_state_t s{};
    if (!shape_state(painter, &s))
        return 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_ellipse(hv::canvas::Rect{x, y, width, height});
    return 0;
}

int heliosview_painter_draw_arc(heliosview_painter_t* painter, float x, float y, float width,
                                float height, float start_deg, float sweep_deg)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!has_alpha(painter->state.stroke_color))
        return 0;
    heliosview_painter_state_t s = painter->state;
    s.fill_color = 0; /* an arc is an outline only */
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_arc(hv::canvas::Rect{x, y, width, height}, start_deg, sweep_deg);
    return 0;
}

int heliosview_painter_draw_polyline(heliosview_painter_t* painter, const float* points, size_t count,
                                     int closed)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!points || count < 2)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "a polyline needs at least 2 points");
    heliosview_painter_state_t s{};
    if (!shape_state(painter, &s))
        return 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_polyline(points, count, closed != 0);
    return 0;
}

int heliosview_painter_draw_polygon(heliosview_painter_t* painter, const float* points, size_t count)
{
    if (!points || count < 3)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "a polygon needs at least 3 points");
    return heliosview_painter_draw_polyline(painter, points, count, 1);
}

/* ================= Public C API: paths ================= */

heliosview_path_t* heliosview_path_create(void)
{
    try {
        return hv::hv_alloc<hv::canvas::PathBuilder>();
    } catch (const std::bad_alloc&) {
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory creating the path");
        return nullptr;
    }
}

void heliosview_path_destroy(heliosview_path_t* path)
{
    if (path)
        hv::hv_dealloc(path);
}

void heliosview_path_reset(heliosview_path_t* path)
{
    if (path)
        path->data.clear();
}

namespace detail5 {
void path_add(heliosview_path_t* path, uint8_t verb, const float* pts, int count)
{
    if (!path)
        return;
    hv::canvas::PathEntry entry;
    entry.verb = verb;
    if (pts && count > 0)
        std::memcpy(entry.pts, pts, static_cast<size_t>(count) * sizeof(float));
    path->data.entries.push_back(entry);
}

void path_add_point(heliosview_path_t* path, uint8_t verb, float x, float y)
{
    const float pts[2] = {x, y};
    path_add(path, verb, pts, 2);
}

} // namespace detail5

using namespace detail5; /* visible to the rest of hv::canvas */


void heliosview_path_move_to(heliosview_path_t* path, float x, float y)
{
    path_add_point(path, hv::canvas::PathVerbStartNew, x, y);
}

void heliosview_path_line_to(heliosview_path_t* path, float x, float y)
{
    path_add_point(path, hv::canvas::PathVerbLineTo, x, y);
}

void heliosview_path_quad_to(heliosview_path_t* path, float cx, float cy, float x, float y)
{
    const float pts[4] = {cx, cy, x, y};
    path_add(path, hv::canvas::PathVerbQuadTo, pts, 4);
}

void heliosview_path_cubic_to(heliosview_path_t* path, float c1x, float c1y, float c2x, float c2y,
                              float x, float y)
{
    const float pts[6] = {c1x, c1y, c2x, c2y, x, y};
    path_add(path, hv::canvas::PathVerbCubicTo, pts, 6);
}

void heliosview_path_close(heliosview_path_t* path)
{
    path_add(path, hv::canvas::PathVerbClose, nullptr, 0);
}

void heliosview_path_add_rect(heliosview_path_t* path, float x, float y, float width, float height)
{
    const float pts[4] = {x, y, width, height};
    path_add(path, hv::canvas::PathVerbRect, pts, 4);
}

void heliosview_path_add_round_rect(heliosview_path_t* path, float x, float y, float width,
                                    float height, float radius)
{
    const float pts[5] = {x, y, width, height, radius};
    path_add(path, hv::canvas::PathVerbRoundRect, pts, 5);
}

void heliosview_path_add_ellipse(heliosview_path_t* path, float x, float y, float width, float height)
{
    const float pts[4] = {x, y, width, height};
    path_add(path, hv::canvas::PathVerbEllipse, pts, 4);
}

void heliosview_path_set_winding(heliosview_path_t* path, heliosview_path_winding_t winding)
{
    if (path)
        path->data.winding = winding;
}

heliosview_path_winding_t heliosview_path_winding(const heliosview_path_t* path)
{
    return path ? path->data.winding : HELIOSVIEW_WINDING_NONZERO;
}

int heliosview_painter_draw_path(heliosview_painter_t* painter, heliosview_path_t* path)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!path)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL");
    painter->context->set_transform(painter->transform);
    painter->context->draw_path(path->data, has_alpha(painter->state.fill_color),
                                has_alpha(painter->state.stroke_color));
    return 0;
}

int heliosview_painter_fill_path(heliosview_painter_t* painter, heliosview_path_t* path, uint32_t argb)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!path)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL");
    heliosview_painter_state_t s = painter->state;
    s.fill_color = argb;
    s.stroke_color = 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_path(path->data, true, false);
    return 0;
}

int heliosview_painter_stroke_path(heliosview_painter_t* painter, heliosview_path_t* path,
                                   uint32_t argb, float width)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!path)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL");
    heliosview_painter_state_t s = painter->state;
    s.stroke_color = argb;
    s.stroke_width = width > 0.0f ? width : 1.0f;
    s.fill_color = 0;
    ScopedState guard(painter, s);
    painter->context->set_transform(painter->transform);
    painter->context->draw_path(path->data, false, true);
    return 0;
}

/* ================= Public C API: text ================= */

int heliosview_painter_draw_text(heliosview_painter_t* painter, const char* utf8, float x, float y)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!utf8)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "text is NULL");
    if (!has_alpha(painter->state.fill_color))
        return 0;
    painter->context->set_transform(painter->transform);
    painter->context->draw_text(utf8, hv::canvas::Rect{x, y, 0.0f, 0.0f},
                                HELIOSVIEW_ALIGN_LEFT | HELIOSVIEW_ALIGN_TOP);
    return 0;
}

int heliosview_painter_draw_text_ex(heliosview_painter_t* painter, const char* utf8, float x, float y,
                                    float width, float height, uint32_t align)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!utf8)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "text is NULL");
    if (!has_alpha(painter->state.fill_color))
        return 0;
    painter->context->set_transform(painter->transform);
    painter->context->draw_text(utf8, hv::canvas::Rect{x, y, width, height}, align);
    return 0;
}

int heliosview_painter_measure_text(heliosview_painter_t* painter, const char* utf8,
                                    heliosview_text_metrics_t* out_metrics)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!out_metrics)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "out_metrics is NULL");
    *out_metrics = heliosview_text_metrics_t{};
    if (!utf8)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "text is NULL");
    painter->context->measure_text(utf8, out_metrics);
    return 0;
}

int heliosview_painter_line_height(const heliosview_painter_t* painter, float* out_height)
{
    if (!painter || !out_height)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "painter or out_height is NULL");
    *out_height = 0.0f;
    /* An empty string measures to the font's own line box, which is exactly the
     * line pitch a caller advances by. */
    heliosview_text_metrics_t metrics{};
    painter->context->measure_text("", &metrics);
    *out_height = metrics.line_height;
    return 0;
}

/* ================= Public C API: images ================= */

int heliosview_painter_draw_image(heliosview_painter_t* painter, heliosview_canvas_t* image,
                                  float dx, float dy, float dw, float dh,
                                  const heliosview_rect_t* src_rect, float alpha)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (!image)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "image is NULL");
    if (image == painter->canvas)
        return hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "a canvas cannot draw itself");

    hv::canvas::Rect src{0, 0, static_cast<float>(image->data.width),
                        static_cast<float>(image->data.height)};
    if (src_rect)
        src = hv::canvas::Rect{static_cast<float>(src_rect->x), static_cast<float>(src_rect->y),
                              static_cast<float>(src_rect->width), static_cast<float>(src_rect->height)};
    /* Clamp to the source so an engine never reads outside the buffer. */
    const float x0 = std::max(0.0f, src.x);
    const float y0 = std::max(0.0f, src.y);
    const float x1 = std::min(static_cast<float>(image->data.width), src.x + src.w);
    const float y1 = std::min(static_cast<float>(image->data.height), src.y + src.h);
    if (x1 <= x0 || y1 <= y0)
        return 0; /* nothing to draw */

    painter->context->set_transform(painter->transform);
    painter->context->draw_image(image->adapter.get(), image->data,
                                 hv::canvas::Rect{x0, y0, x1 - x0, y1 - y0},
                                 hv::canvas::Rect{dx, dy, dw, dh}, std::clamp(alpha, 0.0f, 1.0f));
    return 0;
}

/* ================= Public C API: state stack ================= */

int heliosview_painter_save(heliosview_painter_t* painter)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    hv::canvas::Painter::SavedState saved;
    saved.state = painter->state;
    saved.font_family = painter->font_family;
    std::memcpy(saved.transform, painter->transform, sizeof(saved.transform));
    try {
        painter->stack.push_back(saved);
    } catch (const std::bad_alloc&) {
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory saving the painter state");
    }
    if (painter->context)
        painter->context->save();
    return 0;
}

int heliosview_painter_restore(heliosview_painter_t* painter)
{
    if (!painter_checked(painter, "painter is NULL"))
        return HELIOSVIEW_ERROR_INVALID_ARGUMENT;
    if (painter->stack.empty())
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "painter_restore without a matching painter_save");
    const hv::canvas::Painter::SavedState saved = painter->stack.back();
    painter->stack.pop_back();
    painter->state = saved.state;
    painter->font_family = saved.font_family;
    painter->state.font.family = painter->font_family.empty() ? nullptr : painter->font_family.c_str();
    std::memcpy(painter->transform, saved.transform, sizeof(painter->transform));
    if (painter->context)
        painter->context->restore();
    painter->context->set_state(painter->state);
    painter->context->set_transform(painter->transform);
    return 0;
}

/* ================= Public C API: window integration (next batch) ================= */

namespace detail6 {
int window_canvas_not_implemented(const char* what)
{
    return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, what);
}

} // namespace detail6

using namespace detail6; /* visible to the rest of hv::canvas */
heliosview_canvas_t* heliosview_window_canvas(const heliosview_window_t*)
{
    window_canvas_not_implemented("window drawing is not implemented yet");
    return nullptr;
}

int heliosview_window_set_canvas_engine(heliosview_window_t*, heliosview_canvas_engine_t)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

heliosview_canvas_engine_t heliosview_window_canvas_engine(const heliosview_window_t*)
{
    return HELIOSVIEW_ENGINE_AUTO;
}

int heliosview_window_set_double_buffered(heliosview_window_t*, int)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

int heliosview_window_is_double_buffered(const heliosview_window_t*)
{
    return 0;
}

int heliosview_window_set_background_color(heliosview_window_t*, uint32_t)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

int heliosview_window_invalidate(heliosview_window_t*)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

int heliosview_window_invalidate_rect(heliosview_window_t*, const heliosview_rect_t*)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

int heliosview_window_update(heliosview_window_t*)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

int heliosview_window_present(heliosview_window_t*)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

int heliosview_window_client_rect(const heliosview_window_t*, heliosview_rect_t*)
{
    return window_canvas_not_implemented("window drawing is not implemented yet");
}

} // extern "C"
