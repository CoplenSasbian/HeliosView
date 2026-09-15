#ifndef HELIOSVIEW_HELIOSVIEW_PAINT_H
#define HELIOSVIEW_HELIOSVIEW_PAINT_H

/**
 * HeliosView C API -- drawing: canvas, painter, paths, images.
 *
 * =========================== The model ===========================
 *
 *     heliosview_painter_*        the drawing API: state, transforms, clipping,
 *                                 shapes, paths, text, images. Engine independent:
 *                                 no call here names a rendering engine.
 *       -> heliosview_canvas_t    a CANVAS: a block of pixels with a size, a
 *                                 stride and a pixel format. It is a drawing
 *                                 target, an image (load/save/encode/decode) and
 *                                 a source to draw from -- one type, three uses.
 *         -> a paint engine       the implementation that turns those calls into
 *                                 pixels: GDI+ / GDI / Direct2D / Core Graphics /
 *                                 Cairo / a built-in rasterizer. Chosen per canvas
 *                                 at creation (see "Paint engines"), never hinted
 *                                 at anywhere else.
 *           -> the window         heliosview_window_present() blits a canvas onto
 *                                 a native window -- the LAST step, and the only
 *                                 one that knows what a window is.
 *
 * A canvas is pure memory, so it works with or without a window: draw offscreen,
 * save to a file, load it back, composite canvases, or hand the pixels to a window
 * or a GPU texture. The engine only decides HOW the drawing calls write those
 * bytes.
 *
 * =========================== Coordinates ===========================
 *
 * Origin at the TOP-LEFT of the canvas, x to the RIGHT, y DOWN -- the same
 * convention as the rest of HeliosView and as Win32. No axis flip anywhere.
 *
 * Units are pixels; a 800x600 canvas is 800x600 pixels. A window's canvas is in
 * client pixels, the same space the mouse event coordinates
 * (heliosview_event_t.x/y) use, so hit-testing what you drew needs no conversion.
 * On a high-DPI display multiply logical sizes by the scale factor to get pixels.
 *
 * =========================== Colors ===========================
 *
 * Every color is a 32-bit 0xAARRGGBB value with STRAIGHT (non-premultiplied)
 * alpha: 0xFFFF0000 is opaque red, 0x80FFFFFF half-transparent white. A canvas
 * whose format stores premultiplied pixels (the default does) converts
 * internally, so callers never premultiply anything.
 *
 * In painter state an alpha of 0 means "off": a stroke color of 0 draws no
 * outline, a fill color of 0 no fill.
 *
 * Color management is the platform's business and is deliberately NOT specified
 * here: the same ARGB value is not guaranteed to look pixel-identical on two
 * platforms (macOS color-manages, Windows largely does not).
 *
 * =========================== Image formats ===========================
 *
 * Encoding and decoding is provided by the library itself (vendored stb), NOT by
 * the engine -- so every engine and every platform supports exactly the same
 * formats, and a canvas saved on one platform reads back identically on another:
 *
 *     PNG   read + write   (alpha preserved)      -- the format to prefer
 *     JPEG  read + write   (no alpha; flattened onto a background color)
 *     BMP   read + write   (no alpha; flattened)
 *     TGA   read + write   (alpha preserved)
 *     GIF   read only      (first frame only; animation is not decoded)
 *     PSD, HDR, PNM, PIC   read; HDR, PNM write   (supported by the codec, outside
 *                                                  the tested baseline)
 *     WebP, HEIC, TIFF     not supported
 *
 * Ask before relying on one: heliosview_format_supported(name, for_encoding). A
 * missing format fails the call with HELIOSVIEW_ERROR_UNSUPPORTED.
 *
 * Also not provided: EXIF orientation (a rotated phone JPEG loads in its stored
 * orientation -- rotate it yourself), ICC profiles (dropped), 16-bit samples
 * (reduced to 8), animated GIF frames.
 *
 * =========================== Errors and threading ===========================
 *
 * Same conventions as heliosview.h: 0 = success, negative = error code, and the
 * reason for the most recent failure is readable with heliosview_last_error /
 * heliosview_last_error_string.
 *
 * A canvas is memory, not a window: it may be created, drawn on and destroyed on
 * any thread, one thread at a time. A painter belongs to the thread that began
 * it. The window functions at the end of this header are message-loop-thread only,
 * like every other heliosview_window_* call.
 */

#include <stddef.h>
#include <stdint.h>

#include <HeliosView/heliosview_export.h>

/* heliosview_rect_t (source rectangles below) and the opaque
 * heliosview_window_t declaration live in heliosview.h; it includes this header
 * at its end, so this include is a no-op in the usual umbrella order and pulls
 * the definitions in when this header is included first. */
#include <HeliosView/heliosview.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Paint engines =================
 *
 * The engine is the implementation that turns painter calls into pixels. It is
 * chosen when a canvas is created and reported back by
 * heliosview_canvas_engine(), so an application can build equivalent canvases on
 * several engines and compare them at runtime.
 *
 * CROSS-PLATFORM CODE SHOULD USE ONLY AUTO / NATIVE / ACCELERATED / SOFTWARE.
 * Those are concepts, resolved per platform:
 *
 *                Windows        macOS           Linux
 *     NATIVE     GDI+           Core Graphics   Cairo
 *     ACCELERATED Direct2D      Metal-backed    GL backend
 *     SOFTWARE   the built-in rasterizer (once it exists)
 *
 * The vendor values (>= 16) exist for single-platform tuning and for side-by-side
 * comparison. A vendor engine this platform or build does not provide is still a
 * valid enum value -- it simply is not "compiled" (see heliosview_engine_compiled)
 * and creating a canvas with it fails with HELIOSVIEW_ERROR_UNSUPPORTED. That is
 * deliberate: the header is identical everywhere, so no #ifdef and no build-macro
 * mismatch is possible.
 *
 * Every value below is FIXED for the ABI's lifetime. New engines append in their
 * own reserved block; a value is never reused or renumbered, so an old application
 * binary always resolves an engine to the same implementation. */
typedef enum heliosview_paint_engine {
    /* ---- concept names: use these in portable code ---- */
    HELIOSVIEW_ENGINE_AUTO = 0,         /* best available: ACCELERATED -> NATIVE -> SOFTWARE */
    HELIOSVIEW_ENGINE_NATIVE = 1,       /* this platform's native 2D engine */
    HELIOSVIEW_ENGINE_ACCELERATED = 2,  /* GPU accelerated; fails if unavailable, never silently falls back */
    HELIOSVIEW_ENGINE_SOFTWARE = 3,     /* the library's own rasterizer: identical everywhere, no dependency */

    /* ---- vendor names: single-platform tuning and comparison ---- */
    HELIOSVIEW_ENGINE_GDI = 16,             /* Windows: classic GDI -- fastest, no AA, no alpha */
    HELIOSVIEW_ENGINE_GDI_PLUS = 17,        /* Windows: == NATIVE */
    HELIOSVIEW_ENGINE_D2D = 18,             /* Windows: == ACCELERATED */
    HELIOSVIEW_ENGINE_CORE_GRAPHICS = 32,   /* macOS:   == NATIVE */
    HELIOSVIEW_ENGINE_METAL = 33,           /* macOS:   == ACCELERATED */
    HELIOSVIEW_ENGINE_CAIRO = 48,           /* Linux:   == NATIVE */

    /* New engines go here (64, 80, ...), one block per platform. */
} heliosview_paint_engine_t;

/* ================= Canvas pixel formats =================
 *
 * HELIOSVIEW_FORMAT_AUTO is an input only and resolves to the engine's preferred
 * format; a live canvas always reports a concrete one.
 *
 * The byte order of each format, per pixel:
 *
 *   BGRA8_PREMUL  b0 g1 r2 a3, alpha premultiplied   (== DXGI B8G8R8A8_UNORM, the
 *                                                     native format of GDI+,
 *                                                     Direct2D and Cairo alike)
 *   BGRA8         b0 g1 r2 a3, alpha straight
 *   RGBA8         r0 g1 b2 a3, alpha straight
 *   GRAY8         single 8-bit luminance channel, no alpha (1 byte per pixel)
 *
 * Use heliosview_canvas_stride() for the row pitch: it is always at least
 * width * bytes-per-pixel and 4-byte aligned, but never assume it equals
 * width * bytes-per-pixel. */
typedef enum heliosview_pixel_format {
    HELIOSVIEW_FORMAT_AUTO = 0,          /* resolve to the engine's preferred format (input only) */
    HELIOSVIEW_FORMAT_BGRA8_PREMUL = 1,  /* default */
    HELIOSVIEW_FORMAT_BGRA8 = 2,
    HELIOSVIEW_FORMAT_RGBA8 = 3,
    HELIOSVIEW_FORMAT_GRAY8 = 4,
} heliosview_pixel_format_t;

/* ================= Engine queries =================
 *
 * "compiled" is static (is the engine part of this build?) and "probe" is dynamic
 * (can it create its resources here and now? -- an accelerated engine can be
 * compiled in and still unavailable, e.g. over Remote Desktop or with no GPU).
 * Both are safe to call from any thread at any time. */

/* Whether the engine is part of this build: != 0 = yes. AUTO reports whether any
 * engine is available at all. */
HELIOSVIEW_API int heliosview_engine_compiled(heliosview_paint_engine_t engine);

/* Whether the engine can be used right now (compiled and able to initialize).
 * Creating a canvas asks the same question and fails with
 * HELIOSVIEW_ERROR_UNSUPPORTED when the answer is no. Pass AUTO to ask whether any
 * engine would work. */
HELIOSVIEW_API int heliosview_engine_probe(heliosview_paint_engine_t engine);

/* How many engines this build provides, and the value of the i-th of them
 * (i < count) -- lets a host list what it can choose from without knowing this
 * build's configuration. Both report 0 for an out-of-range index. */
HELIOSVIEW_API int heliosview_engine_count(void);
HELIOSVIEW_API heliosview_paint_engine_t heliosview_engine_at(int index);

/* Short lowercase engine name for logs and diagnostics:
 * "gdi+", "gdi", "d2d", "coregraphics", "metal", "cairo", "software", "auto".
 * Which engine a concept name resolved to is asked of the canvas
 * (heliosview_canvas_engine), not guessed from this string. Never NULL. */
HELIOSVIEW_API const char* heliosview_engine_name(heliosview_paint_engine_t engine);

/* Whether the engine provides a capability (1 = yes, 0 = no; AUTO answers for the
 * engine it would resolve to). Use this instead of testing engine names to decide
 * how to draw -- e.g. ask about ANTIALIAS before relying on smooth edges: a plain
 * GDI canvas reports 0 and the same code still runs, just without smoothing. */
typedef enum heliosview_paint_feature {
    HELIOSVIEW_FEATURE_ANTIALIAS = 0,     /* smooth edge and text rendering */
    HELIOSVIEW_FEATURE_ALPHA_BLEND = 1,   /* per-pixel alpha compositing */
    HELIOSVIEW_FEATURE_TRANSFORM = 2,     /* the full affine transform (rotate/skew, not just translate+scale) */
    HELIOSVIEW_FEATURE_CLIP_PATH = 3,     /* clipping to a path (not only to a rectangle) */
    HELIOSVIEW_FEATURE_PATH_FILL = 4,     /* filling paths, with both winding rules */
    HELIOSVIEW_FEATURE_TEXT = 5,          /* the text calls */
    HELIOSVIEW_FEATURE_TEXT_AA = 6,       /* antialiased text specifically */
    HELIOSVIEW_FEATURE_IMAGE_DRAW = 7,    /* drawing one canvas into another */
    HELIOSVIEW_FEATURE_DIRECT_PIXELS = 8, /* heliosview_canvas_data() hands out the live pixel buffer */
} heliosview_paint_feature_t;

HELIOSVIEW_API int heliosview_engine_supports_feature(heliosview_paint_engine_t engine,
                                                      heliosview_paint_feature_t feature);

/* The library-wide default engine, used wherever AUTO is passed to a canvas
 * creation call. Defaults to HELIOSVIEW_ENGINE_AUTO; set it once, before creating
 * canvases (the same contract as heliosview_set_allocator). Passing AUTO restores
 * the resolution chain (ACCELERATED -> NATIVE -> SOFTWARE). */
HELIOSVIEW_API void heliosview_set_default_paint_engine(heliosview_paint_engine_t engine);
HELIOSVIEW_API heliosview_paint_engine_t heliosview_default_paint_engine(void);

/* ================= Canvas =================
 *
 * Creating a canvas allocates and ZEROES its pixels (fully transparent in every
 * format with alpha, black in GRAY8). The engine is fixed for the canvas's life;
 * to continue drawing on a different engine, clone it. */

typedef struct heliosview_canvas heliosview_canvas_t;

/* Create a canvas of width x height pixels. format and engine may each be AUTO (0)
 * for the default. Returns NULL on failure (non-positive size, unavailable engine,
 * unsupported format, out of memory) with the reason recorded. */
HELIOSVIEW_API heliosview_canvas_t* heliosview_canvas_create(int32_t width, int32_t height,
                                                             heliosview_pixel_format_t format,
                                                             heliosview_paint_engine_t engine);

/* Clone a canvas into a new one, converting to `format` (AUTO = keep the source's)
 * and wrapping it in `engine` (AUTO = keep the source's). The pixels are copied,
 * so the result starts as an exact copy ready to be drawn on further. This is how
 * a canvas moves to another engine or format. Returns NULL on failure. */
HELIOSVIEW_API heliosview_canvas_t* heliosview_canvas_clone(const heliosview_canvas_t* canvas,
                                                            heliosview_pixel_format_t format,
                                                            heliosview_paint_engine_t engine);

/* Destroy a canvas (NULL is a no-op). A painter begun on it must be ended first. */
HELIOSVIEW_API void heliosview_canvas_destroy(heliosview_canvas_t* canvas);

/* Size in pixels; either out pointer may be NULL. 0 = success. */
HELIOSVIEW_API int heliosview_canvas_size(const heliosview_canvas_t* canvas,
                                          int32_t* out_width, int32_t* out_height);

/* Row pitch in bytes (>= width * bytes-per-pixel, 4-byte aligned); 0 on failure. */
HELIOSVIEW_API int32_t heliosview_canvas_stride(const heliosview_canvas_t* canvas);

/* The canvas's concrete pixel format / engine; 0 (AUTO) when canvas is NULL. The
 * engine answer is what AUTO resolved to. */
HELIOSVIEW_API heliosview_pixel_format_t heliosview_canvas_format(const heliosview_canvas_t* canvas);
HELIOSVIEW_API heliosview_paint_engine_t heliosview_canvas_engine(const heliosview_canvas_t* canvas);

/* The live pixel buffer: stride bytes per row, format as reported, writable.
 * Returns NULL when the engine has no directly addressable pixels -- ask
 * heliosview_engine_supports_feature(..., HELIOSVIEW_FEATURE_DIRECT_PIXELS) first,
 * and fall back to set_pixel/get_pixel, which every engine provides.
 * After writing through this pointer, call heliosview_canvas_end_write. */
HELIOSVIEW_API void* heliosview_canvas_data(heliosview_canvas_t* canvas);

/* Announce a direct write through heliosview_canvas_data(): the engine drops any
 * cached copy of these pixels. Call it after writing and before the next paint,
 * blit or save. 0 = success. */
HELIOSVIEW_API int heliosview_canvas_end_write(heliosview_canvas_t* canvas);

/* Resize the canvas (content is NOT preserved). 0 = success. */
HELIOSVIEW_API int heliosview_canvas_resize(heliosview_canvas_t* canvas,
                                            int32_t width, int32_t height);

/* Fill the whole canvas with an ARGB color, ignoring painter state, transform and
 * clipping -- the "clear the canvas" primitive. 0 = success. */
HELIOSVIEW_API int heliosview_canvas_fill(heliosview_canvas_t* canvas, uint32_t argb);

/* Single pixel access, in canvas coordinates. Out-of-range coordinates fail.
 * These work on every engine (the ones without DIRECT_PIXELS implement them
 * internally). 0 = success. */
HELIOSVIEW_API int heliosview_canvas_set_pixel(heliosview_canvas_t* canvas,
                                               int32_t x, int32_t y, uint32_t argb);
HELIOSVIEW_API int heliosview_canvas_get_pixel(const heliosview_canvas_t* canvas,
                                               int32_t x, int32_t y, uint32_t* out_argb);

/* Copy `src` onto `dst` at (x, y): no scaling and no transform, but format
 * conversion and alpha blending (alpha 0..1 scales the source's opacity; 1 = a
 * straight copy). `src_rect` selects a source region (NULL = the whole canvas).
 * Use it to compose canvases, to cache a rendered layer, or to move pixels between
 * formats and engines. 0 = success. */
HELIOSVIEW_API int heliosview_canvas_blit(heliosview_canvas_t* src, heliosview_canvas_t* dst,
                                          int32_t x, int32_t y,
                                          const heliosview_rect_t* src_rect, float alpha);

/* ================= Images: canvas <-> file / memory =================
 *
 * A canvas is an image, so "draw a picture" is "load it, then draw it with
 * heliosview_painter_draw_image". Loading decodes into the canvas's own pixel
 * format; the original encoding is not kept, because a canvas is a pixel buffer,
 * not an image container. That is what makes drawing one a memory copy with no
 * codec involved.
 *
 * The format is taken from the content, not the extension. Everything the codec
 * supports (see the format list at the top) is accepted; anything else fails with
 * HELIOSVIEW_ERROR_UNSUPPORTED. */

/* Decode a file (UTF-8 path). format/engine select the returned canvas's pixel
 * format and engine; AUTO for either uses the default. Returns NULL on failure. */
HELIOSVIEW_API heliosview_canvas_t* heliosview_canvas_load(const char* path,
                                                           heliosview_pixel_format_t format,
                                                           heliosview_paint_engine_t engine);

/* Decode an in-memory image (the bytes of a PNG/JPEG/... file). `data` is copied,
 * the caller keeps ownership. Returns NULL on failure. */
HELIOSVIEW_API heliosview_canvas_t* heliosview_canvas_load_memory(const void* data, size_t size,
                                                                  heliosview_pixel_format_t format,
                                                                  heliosview_paint_engine_t engine);

/* Encode the canvas into a freshly allocated buffer -- the layer that makes an
 * image usable beyond the filesystem (HTTP upload, clipboard, archive, a data URI,
 * byte-exact test assertions).
 *
 *   format      "png" / "jpeg" / "jpg" / "bmp" / "tga" (case-insensitive).
 *   quality     1..100 for lossy formats, ignored by the others; 0 = default (92).
 *   background  ARGB the canvas is flattened onto by formats without alpha (JPEG,
 *               BMP). Pass an opaque color, or 0 to flatten onto black.
 *   out_data    receives a buffer allocated by the library: free it with
 *               heliosview_free. May be NULL to ask only for the size.
 *   out_size    receives the encoded byte count; may be NULL.
 *
 * Returns the byte count (>= 0) on success, negative on failure. With
 * out_data == NULL nothing is encoded into memory but the size is still reported,
 * so the caller can size a buffer itself -- the same two-phase pattern as
 * heliosview_utf8_to_wide. */
HELIOSVIEW_API int heliosview_canvas_encode(heliosview_canvas_t* canvas, const char* format,
                                            int quality, uint32_t background,
                                            void** out_data, size_t* out_size);

/* Encode the canvas straight into a file (the everyday call: no buffer handling).
 *   path    UTF-8 destination.
 *   format  encoder name as above; NULL or "" infers it from the path's extension
 *           (.png, .jpg, .jpeg, .bmp, .tga). An unknown extension fails.
 *   quality / background  as in heliosview_canvas_encode.
 * 0 = success. */
HELIOSVIEW_API int heliosview_canvas_save(heliosview_canvas_t* canvas, const char* path,
                                          const char* format, int quality, uint32_t background);

/* Whether the codec can read (for_encoding == 0) or write (for_encoding != 0) the
 * named format: 1 = yes, 0 = no. Independent of the engine, since the codec is. */
HELIOSVIEW_API int heliosview_format_supported(const char* format, int for_encoding);

/* ================= Painters =================
 *
 * A painter draws onto one canvas. Its state -- stroke, fill, line geometry, font,
 * global alpha, antialiasing, transform, clip -- is what every drawing call reads.
 *
 * heliosview_painter_begin RESETS the state to the defaults below, so a session
 * never inherits whatever the previous one left behind:
 *
 *     stroke_color  0x00000000 (off)   fill_color   0x00000000 (off)
 *     stroke_width  1.0                line_cap/join  butt / miter
 *     font          default sans-serif, 12 px, regular
 *     alpha         1.0                antialias     on
 *     transform     identity           clip          the whole canvas
 *
 * Use heliosview_painter_save / _restore to scope changes. */

typedef struct heliosview_painter heliosview_painter_t;

/* Declared here (defined under "Paths") because the clipping calls take one. */
typedef struct heliosview_path heliosview_path_t;

typedef enum heliosview_line_cap {
    HELIOSVIEW_CAP_BUTT = 0,   /* the line stops at its endpoint (default) */
    HELIOSVIEW_CAP_ROUND = 1,  /* a half circle past the endpoint */
    HELIOSVIEW_CAP_SQUARE = 2, /* a half square past the endpoint */
} heliosview_line_cap_t;

typedef enum heliosview_line_join {
    HELIOSVIEW_JOIN_MITER = 0, /* extend the edges until they meet (default) */
    HELIOSVIEW_JOIN_ROUND = 1, /* round off the corner */
    HELIOSVIEW_JOIN_BEVEL = 2, /* cut the corner off */
} heliosview_line_join_t;

/* Font style bits for heliosview_font_desc::flags. Only regular and bold are
 * distinct weights: the platforms express weight differently (GDI+ has nine steps,
 * Core Graphics a -1..1 trait, Cairo just normal/bold) and a finer API could not be
 * honored uniformly. */
#define HELIOSVIEW_FONT_BOLD      (1u << 0)
#define HELIOSVIEW_FONT_ITALIC    (1u << 1)
#define HELIOSVIEW_FONT_UNDERLINE (1u << 2)
#define HELIOSVIEW_FONT_STRIKEOUT (1u << 3)

typedef struct heliosview_font_desc {
    const char* family; /* UTF-8 family name ("Segoe UI", "Consolas", ...); NULL or "" selects
                         * the platform's default sans-serif. An unknown family falls back to
                         * that default rather than failing. The string is copied. */
    float size;         /* nominal pixel size (the em size), NOT points: a 14 here is the same
                         * 14 as a mouse coordinate. 0 selects 12. */
    uint32_t flags;     /* HELIOSVIEW_FONT_* bits */
} heliosview_font_desc_t;

typedef struct heliosview_painter_state {
    uint32_t stroke_color;            /* ARGB; alpha 0 = no outline */
    uint32_t fill_color;              /* ARGB; alpha 0 = no fill */
    float stroke_width;               /* line width in pixels; <= 0 means 1 */
    heliosview_line_cap_t line_cap;
    heliosview_line_join_t line_join;
    heliosview_font_desc_t font;
    float alpha;                      /* 0..1 global opacity, multiplied into every color */
    int antialias;                    /* != 0 = antialiased edges and text (default) */
} heliosview_painter_state_t;

/* Begin painting on a canvas. Every call must be paired with
 * heliosview_painter_end. Returns NULL on failure (NULL canvas, a painter already
 * active on that canvas, out of memory). */
HELIOSVIEW_API heliosview_painter_t* heliosview_painter_begin(heliosview_canvas_t* canvas);

/* End painting: flush the engine and destroy the painter. Once it returns, the
 * canvas is complete and safe to save, blit or present. 0 = success. */
HELIOSVIEW_API int heliosview_painter_end(heliosview_painter_t* painter);

/* Destroy a painter without a clean end -- for error paths only (e.g. a C++
 * exception unwound past heliosview_painter_end). Releasing the canvas is the
 * point: a canvas with a live painter cannot be painted again. Safe on NULL and
 * after _end. */
HELIOSVIEW_API void heliosview_painter_destroy(heliosview_painter_t* painter);

/* ---- state ---- */

/* Replace the whole state at once (fields left 0 mean the documented default). The
 * transform and clip are not part of the state and are unaffected. 0 = success. */
HELIOSVIEW_API int heliosview_painter_set_state(heliosview_painter_t* painter,
                                                const heliosview_painter_state_t* state);

/* Read the current state back (font.family points at painter-owned storage, valid
 * until the font changes). 0 = success. */
HELIOSVIEW_API int heliosview_painter_get_state(const heliosview_painter_t* painter,
                                                heliosview_painter_state_t* out_state);

/* Convenience setters over set_state */
HELIOSVIEW_API int heliosview_painter_set_stroke(heliosview_painter_t* painter,
                                                 uint32_t argb, float width);
HELIOSVIEW_API int heliosview_painter_set_fill(heliosview_painter_t* painter, uint32_t argb);
HELIOSVIEW_API int heliosview_painter_set_font(heliosview_painter_t* painter,
                                               const heliosview_font_desc_t* desc);
HELIOSVIEW_API int heliosview_painter_set_alpha(heliosview_painter_t* painter, float alpha);
HELIOSVIEW_API int heliosview_painter_set_antialias(heliosview_painter_t* painter, int on);

/* ---- transform ----
 *
 * A 2x3 affine matrix laid out as [a b c d e f] and applied as
 *
 *     x' = a*x + c*y + e
 *     y' = b*x + d*y + f
 *
 * -- the same row-vector, x-right/y-down convention as Win32's XFORM and
 * Direct2D's matrix, so no axis flip is implied. Transforms are cumulative
 * (translate then rotate rotates about the translated origin) and affect the
 * geometry of every later call, including text and images. The clip region is
 * transformed with it, which is what makes transforms usable as viewports. */

HELIOSVIEW_API int heliosview_painter_set_transform(heliosview_painter_t* painter, const float* m);
HELIOSVIEW_API int heliosview_painter_get_transform(const heliosview_painter_t* painter,
                                                    float* out_m); /* 6 floats */
HELIOSVIEW_API int heliosview_painter_translate(heliosview_painter_t* painter, float dx, float dy);
HELIOSVIEW_API int heliosview_painter_scale(heliosview_painter_t* painter, float sx, float sy);
/* Rotate by radians, clockwise on screen (y grows downward) */
HELIOSVIEW_API int heliosview_painter_rotate(heliosview_painter_t* painter, float radians);
HELIOSVIEW_API int heliosview_painter_skew(heliosview_painter_t* painter, float kx, float ky);
HELIOSVIEW_API int heliosview_painter_reset_transform(heliosview_painter_t* painter);

/* ---- clipping ----
 *
 * The clip starts as the whole canvas. set_clip_* REPLACES it; intersect_clip_rect
 * NARROWS it, which is the usual way to nest a temporary restriction. Clip geometry
 * is in the current transform's space. Anything drawn outside is discarded. */

HELIOSVIEW_API int heliosview_painter_set_clip_rect(heliosview_painter_t* painter,
                                                    float x, float y, float width, float height);
HELIOSVIEW_API int heliosview_painter_set_clip_path(heliosview_painter_t* painter,
                                                    heliosview_path_t* path);
HELIOSVIEW_API int heliosview_painter_intersect_clip_rect(heliosview_painter_t* painter,
                                                          float x, float y, float width, float height);
HELIOSVIEW_API int heliosview_painter_reset_clip(heliosview_painter_t* painter);
/* Bounding box of the current clip, in canvas coordinates. An engine may return a box
 * that encloses the clip rather than the exact clip (conservative). */
HELIOSVIEW_API int heliosview_painter_clip_bounds(const heliosview_painter_t* painter,
                                                  heliosview_rect_t* out_rect);

/* ---- shapes ----
 *
 * Each call uses the current state: the outline with stroke_color/stroke_width, the
 * interior with fill_color, the interior first. With both off the call is a no-op.
 * Rectangle arguments are (x, y, width, height); a negative width/height mirrors
 * the shape about x/y. Angles are degrees, clockwise on screen. */

/* Fill the whole canvas with argb, ignoring painter state, transform and clipping --
 * the "clear the canvas" call while painting (its standalone equivalent is
 * heliosview_canvas_fill). 0 = success. */
HELIOSVIEW_API int heliosview_painter_clear(heliosview_painter_t* painter, uint32_t argb);

/* Fill the rectangle with an explicit color, ignoring stroke_color and fill_color
 * (but honoring transform, clip and antialias): set_state + draw_rect in one call.
 * alpha 0 draws nothing. 0 = success. */
HELIOSVIEW_API int heliosview_painter_fill_rect(heliosview_painter_t* painter,
                                                float x, float y, float width, float height,
                                                uint32_t argb);
HELIOSVIEW_API int heliosview_painter_draw_line(heliosview_painter_t* painter,
                                                float x1, float y1, float x2, float y2);
HELIOSVIEW_API int heliosview_painter_draw_rect(heliosview_painter_t* painter,
                                                float x, float y, float width, float height);
/* radius is clamped to half the shorter side; 0 gives a plain rectangle */
HELIOSVIEW_API int heliosview_painter_draw_round_rect(heliosview_painter_t* painter,
                                                      float x, float y, float width, float height,
                                                      float radius);
HELIOSVIEW_API int heliosview_painter_draw_ellipse(heliosview_painter_t* painter,
                                                   float x, float y, float width, float height);
/* Elliptical arc, outline only: 0 degrees is 3 o'clock and grows clockwise;
 * sweep_deg may be negative. */
HELIOSVIEW_API int heliosview_painter_draw_arc(heliosview_painter_t* painter,
                                               float x, float y, float width, float height,
                                               float start_deg, float sweep_deg);
/* `points` holds count pairs (x0, y0, x1, y1, ...). closed != 0 connects the last
 * point back to the first. At least 2 points (3 for a fillable polygon). */
HELIOSVIEW_API int heliosview_painter_draw_polyline(heliosview_painter_t* painter,
                                                    const float* points, size_t count, int closed);
HELIOSVIEW_API int heliosview_painter_draw_polygon(heliosview_painter_t* painter,
                                                   const float* points, size_t count);

/* ---- paths ----
 *
 * A reusable shape description (segments plus whole sub-shapes) that can be
 * stroked, filled and used as a clip any number of times, under any transform and
 * on any engine. A path holds no engine resources: the engine converts it when it
 * is used.
 *
 * The winding rule decides what is "inside" a path with several contours or a
 * self-intersection:
 *   NONZERO (default) inside when a ray crosses the outline more times one way than
 *                     the other -- holes need opposite winding.
 *   EVENODD           inside when a ray crosses it an odd number of times -- nested
 *                     contours alternate filled and hole. */

typedef enum heliosview_path_winding {
    HELIOSVIEW_WINDING_NONZERO = 0,
    HELIOSVIEW_WINDING_EVENODD = 1,
} heliosview_path_winding_t;

HELIOSVIEW_API heliosview_path_t* heliosview_path_create(void);
HELIOSVIEW_API void heliosview_path_destroy(heliosview_path_t* path); /* NULL is a no-op */
HELIOSVIEW_API void heliosview_path_reset(heliosview_path_t* path);   /* keeps the winding rule */
HELIOSVIEW_API void heliosview_path_move_to(heliosview_path_t* path, float x, float y);
HELIOSVIEW_API void heliosview_path_line_to(heliosview_path_t* path, float x, float y);
HELIOSVIEW_API void heliosview_path_quad_to(heliosview_path_t* path,
                                            float cx, float cy, float x, float y);
HELIOSVIEW_API void heliosview_path_cubic_to(heliosview_path_t* path,
                                             float c1x, float c1y, float c2x, float c2y,
                                             float x, float y);
HELIOSVIEW_API void heliosview_path_close(heliosview_path_t* path);
HELIOSVIEW_API void heliosview_path_add_rect(heliosview_path_t* path,
                                             float x, float y, float width, float height);
HELIOSVIEW_API void heliosview_path_add_round_rect(heliosview_path_t* path,
                                                   float x, float y, float width, float height,
                                                   float radius);
HELIOSVIEW_API void heliosview_path_add_ellipse(heliosview_path_t* path,
                                                float x, float y, float width, float height);
HELIOSVIEW_API void heliosview_path_set_winding(heliosview_path_t* path,
                                                heliosview_path_winding_t winding);
HELIOSVIEW_API heliosview_path_winding_t heliosview_path_winding(const heliosview_path_t* path);

/* Draw a path with the current state: the interior with fill_color (using the
 * path's winding rule), then the outline with stroke_color/stroke_width. 0 = success. */
HELIOSVIEW_API int heliosview_painter_draw_path(heliosview_painter_t* painter,
                                                heliosview_path_t* path);
/* Fill / stroke with an explicit color, ignoring the corresponding state field */
HELIOSVIEW_API int heliosview_painter_fill_path(heliosview_painter_t* painter,
                                                heliosview_path_t* path, uint32_t argb);
HELIOSVIEW_API int heliosview_painter_stroke_path(heliosview_painter_t* painter,
                                                  heliosview_path_t* path,
                                                  uint32_t argb, float width);

/* ---- text ----
 *
 * Text is UTF-8 in, laid out by the platform's text stack, painted with the state's
 * fill_color (stroke_color does not outline glyphs). Family, pixel size and
 * bold/italic/underline/strikeout come from the state's font.
 *
 * draw_text puts (x, y) at the TOP-LEFT of the text box and draws one line.
 * draw_text_ex draws one line inside the given box, positioned by the
 * HELIOSVIEW_ALIGN_* bits and clipped to it; the box may be larger than the text
 * (to align it) or smaller (to clip it).
 *
 * Neither wraps: for multi-line text draw each line, advancing y by
 * heliosview_painter_line_height(). Line breaking stays the application's decision
 * instead of a guess at word-wrap rules.
 *
 * Measured values are this engine's own: the same string, family and size measure
 * slightly differently on another platform's engine. Measure and draw with the SAME
 * engine, and never treat a measurement as a cross-platform constant. */

/* Horizontal alignment, mutually exclusive (default LEFT) */
#define HELIOSVIEW_ALIGN_LEFT    0u
#define HELIOSVIEW_ALIGN_HCENTER (1u << 0)
#define HELIOSVIEW_ALIGN_RIGHT   (1u << 1)
/* Vertical alignment, mutually exclusive (default TOP) */
#define HELIOSVIEW_ALIGN_TOP     0u
#define HELIOSVIEW_ALIGN_VCENTER (1u << 2)
#define HELIOSVIEW_ALIGN_BOTTOM  (1u << 3)
#define HELIOSVIEW_ALIGN_BASELINE (1u << 4) /* y / the box's aligned line uses the baseline */

HELIOSVIEW_API int heliosview_painter_draw_text(heliosview_painter_t* painter,
                                                const char* utf8, float x, float y);
HELIOSVIEW_API int heliosview_painter_draw_text_ex(heliosview_painter_t* painter,
                                                   const char* utf8,
                                                   float x, float y, float width, float height,
                                                   uint32_t align);

typedef struct heliosview_text_metrics {
    float width;       /* advance width of the line */
    float height;      /* height of the line box (ascent + descent) */
    float ascent;      /* from the top of the line box down to the baseline */
    float descent;     /* from the baseline down to the bottom of the line box */
    float line_height; /* recommended distance between consecutive lines */
} heliosview_text_metrics_t;

HELIOSVIEW_API int heliosview_painter_measure_text(heliosview_painter_t* painter,
                                                   const char* utf8,
                                                   heliosview_text_metrics_t* out_metrics);
HELIOSVIEW_API int heliosview_painter_line_height(const heliosview_painter_t* painter,
                                                  float* out_height);

/* ---- images ---- */

/* Draw `image` (another canvas) into the rectangle (dx, dy, dw, dh), scaled to
 * fit, under the current transform, clip, alpha and antialias setting. `src_rect`
 * selects a sub-region of the source (NULL = all of it), which is how a sprite
 * sheet is drawn. A negative dw/dh mirrors it.
 *
 * The source may be any canvas of any format on any engine -- the engine converts,
 * and a straight copy is used when formats and scale allow it. It must not be the
 * canvas being painted. 0 = success. */
HELIOSVIEW_API int heliosview_painter_draw_image(heliosview_painter_t* painter,
                                                 heliosview_canvas_t* image,
                                                 float dx, float dy, float dw, float dh,
                                                 const heliosview_rect_t* src_rect,
                                                 float alpha);

/* ---- state stack ----
 *
 * save/restore scope state changes (stroke, fill, font, alpha, transform, clip) so a
 * nested drawing routine cannot leak its settings into its caller. Calls nest;
 * restore returns to the most recent unmatched save. */

HELIOSVIEW_API int heliosview_painter_save(heliosview_painter_t* painter);
HELIOSVIEW_API int heliosview_painter_restore(heliosview_painter_t* painter);

/* ================= Window integration (the last step) =================
 *
 * A window owns a canvas ("backing canvas") sized to its client area. Painting a
 * window is: the library hands the paint handler a painter already targeting that
 * canvas, the handler draws, the library blits the canvas onto the window. That is
 * why double buffering is on by default and why there is no flicker: nothing is
 * ever drawn straight to the screen.
 *
 * The library drives this from WM_PAINT, delivered to the application as a
 * HELIOSVIEW_EVENT_WINDOW_PAINT event whose `painter` field is the painter to draw
 * with. Ask for a repaint with heliosview_window_invalidate / _invalidate_rect.
 *
 * Message-loop thread only.
 *
 * NOTE: these functions are part of the design but NOT implemented yet -- they
 * currently report HELIOSVIEW_ERROR_UNSUPPORTED. Window integration is the next
 * batch; everything above this line works headless today. */

HELIOSVIEW_API heliosview_canvas_t* heliosview_window_canvas(const heliosview_window_t* window);
HELIOSVIEW_API int heliosview_window_set_paint_engine(heliosview_window_t* window,
                                                      heliosview_paint_engine_t engine);
HELIOSVIEW_API heliosview_paint_engine_t heliosview_window_paint_engine(const heliosview_window_t* window);
HELIOSVIEW_API int heliosview_window_set_double_buffered(heliosview_window_t* window, int on);
HELIOSVIEW_API int heliosview_window_is_double_buffered(const heliosview_window_t* window);
HELIOSVIEW_API int heliosview_window_set_background_color(heliosview_window_t* window, uint32_t argb);
HELIOSVIEW_API int heliosview_window_invalidate(heliosview_window_t* window);
HELIOSVIEW_API int heliosview_window_invalidate_rect(heliosview_window_t* window,
                                                     const heliosview_rect_t* rect);
HELIOSVIEW_API int heliosview_window_update(heliosview_window_t* window);
HELIOSVIEW_API int heliosview_window_present(heliosview_window_t* window);
HELIOSVIEW_API int heliosview_window_client_rect(const heliosview_window_t* window,
                                                 heliosview_rect_t* out_rect);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_PAINT_H */
