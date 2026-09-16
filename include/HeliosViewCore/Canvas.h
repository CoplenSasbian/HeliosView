#pragma once

/**
 * HeliosView.Core -- drawing: canvases, painters, paths and images.
 *
 * A typed, RAII wrapper over <HeliosView/heliosview_canvas.h>; the C header stays
 * the contract, this one only changes the spelling:
 *
 *   heliosview_canvas_t*           -> helios::Canvas    (owns it, frees on destruction)
 *   heliosview_painter_begin/_end  -> helios::Painter   (begins in the constructor,
 *                                                        ends in the destructor)
 *   heliosview_path_t*             -> helios::Path      (owns it)
 *   heliosview_canvas_engine_t      -> helios::PaintEngine
 *   heliosview_pixel_format_t      -> helios::PixelFormat
 *   heliosview_painter_state_t     -> helios::PainterState
 *   heliosview_font_desc_t         -> helios::FontDesc  (owns the family string)
 *   heliosview_text_metrics_t      -> helios::TextMetrics
 *   heliosview_rect_t              -> helios::Rect      (from <HeliosViewCore/System.h>)
 *
 * Nothing here needs a window: a canvas is memory.
 *
 * Usage:
 *   helios::Canvas canvas(640, 420);
 *   {
 *       helios::Painter p(canvas);                  // the session is the scope
 *       p.clear(0xFF1E2430u);
 *       p.fillRect(20, 20, 200, 120, 0xFF2D7FF9u);
 *       p.setFont({"Segoe UI", 24.0f, helios::FontFlag::Bold});
 *       p.setFill(0xFFF2F2F7u);
 *       p.drawText("HeliosView canvas", 60, 28);
 *   }                                               // painter ends here: flushed
 *   canvas.save("examples/out/shape.png");          // the format comes from the extension
 *
 * Error handling -- one rule, everywhere:
 *   - a call that produces something (a canvas, a clone, a decoded image, a
 *     painter session) returns an object; ask it valid().
 *   - a call that performs an operation returns bool: false means "it did not
 *     happen" and the reason is the library's thread-local last error, readable
 *     with helios::lastErrorDescription("op") or thrown with helios::throwLastError
 *     (see <HeliosViewCore/Error.h>).
 *   - a query that can have no answer returns std::optional<T> (empty = no answer).
 *   Use valid()/bool, not exceptions, wherever a failure is expected: drawing into
 *   a wrong-sized canvas is a normal outcome, not an error.
 *
 * Threading: a canvas is memory, so it may be used from any thread -- one thread at
 * a time. A painter stays on the thread that created it, and a canvas must outlive
 * every painter begun on it. The window-canvas functions at the end of
 * heliosview_canvas.h are deliberately not wrapped: they are not implemented yet
 * (they report HELIOSVIEW_ERROR_UNSUPPORTED on every call).
 */

#include <HeliosView/heliosview_canvas.h>
#include <HeliosViewCore/Error.h>
#include <HeliosViewCore/System.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helios {

class Canvas;
class Painter;
class Path;

/* ---------- enums ---------- */

// The implementation that turns painter calls into pixels (mirrors
// heliosview_canvas_engine_t). Portable code uses only Auto / Builtin / Native:
// Builtin is the consistent high-performance cross-platform engine (Blend2D).
// Native is this platform's OS 2D engine (GDI+ on Windows, Core Graphics on macOS,
// Cairo on Linux).
enum class PaintEngine : int32_t {
	Auto = HELIOSVIEW_ENGINE_AUTO,               // best available: builtin -> native
	Builtin = HELIOSVIEW_ENGINE_BUILTIN,         // cross-platform high-performance engine (Blend2D)
	Native = HELIOSVIEW_ENGINE_NATIVE,           // this platform's native 2D engine (GDI+ on Windows)
	Software = HELIOSVIEW_ENGINE_SOFTWARE,       // alias for Builtin
	Accelerated = HELIOSVIEW_ENGINE_ACCELERATED, // optional GPU engine (Direct2D on Windows)
	Gdi = HELIOSVIEW_ENGINE_GDI,                 // Windows: classic GDI (fastest, no AA, no alpha)
	GdiPlus = HELIOSVIEW_ENGINE_GDI_PLUS,        // Windows: == Native
	D2D = HELIOSVIEW_ENGINE_D2D,                 // Windows: == Accelerated
	Blend2D = HELIOSVIEW_ENGINE_BLEND2D,         // Cross-platform: == Builtin
	CoreGraphics = HELIOSVIEW_ENGINE_CORE_GRAPHICS,
	Metal = HELIOSVIEW_ENGINE_METAL,
	Cairo = HELIOSVIEW_ENGINE_CAIRO,
};

// The pixel layout of a canvas (mirrors heliosview_pixel_format_t). Auto is an
// input only: a live canvas always reports a concrete format.
enum class PixelFormat : int32_t {
	Auto = HELIOSVIEW_FORMAT_AUTO,
	Bgra8Premul = HELIOSVIEW_FORMAT_BGRA8_PREMUL, // b,g,r,a; alpha premultiplied (the default)
	Bgra8 = HELIOSVIEW_FORMAT_BGRA8,              // b,g,r,a; alpha straight
	Rgba8 = HELIOSVIEW_FORMAT_RGBA8,              // r,g,b,a; alpha straight
	Gray8 = HELIOSVIEW_FORMAT_GRAY8,              // one luminance byte per pixel
};

// A capability an engine may or may not have (mirrors heliosview_canvas_feature_t);
// ask with engineSupportsFeature() instead of assuming.
enum class PaintFeature : int32_t {
	Antialias = HELIOSVIEW_FEATURE_ANTIALIAS,
	AlphaBlend = HELIOSVIEW_FEATURE_ALPHA_BLEND,
	Transform = HELIOSVIEW_FEATURE_TRANSFORM,
	ClipPath = HELIOSVIEW_FEATURE_CLIP_PATH,
	PathFill = HELIOSVIEW_FEATURE_PATH_FILL,
	Text = HELIOSVIEW_FEATURE_TEXT,
	TextAntialias = HELIOSVIEW_FEATURE_TEXT_AA,
	ImageDraw = HELIOSVIEW_FEATURE_IMAGE_DRAW,
	DirectPixels = HELIOSVIEW_FEATURE_DIRECT_PIXELS,
};

// How a stroked line ends (mirrors heliosview_line_cap_t).
enum class LineCap : int32_t {
	Butt = HELIOSVIEW_CAP_BUTT,     // the line stops at its endpoint (default)
	Round = HELIOSVIEW_CAP_ROUND,   // a half circle past the endpoint
	Square = HELIOSVIEW_CAP_SQUARE, // a half square past the endpoint
};

// How the corner between two stroked segments is filled (mirrors
// heliosview_line_join_t).
enum class LineJoin : int32_t {
	Miter = HELIOSVIEW_JOIN_MITER, // extend the edges until they meet (default)
	Round = HELIOSVIEW_JOIN_ROUND,
	Bevel = HELIOSVIEW_JOIN_BEVEL,
};

// What counts as "inside" a path that has several contours or crosses itself
// (mirrors heliosview_path_winding_t).
enum class PathWinding : int32_t {
	NonZero = HELIOSVIEW_WINDING_NONZERO, // holes need the opposite winding direction (default)
	EvenOdd = HELIOSVIEW_WINDING_EVENODD, // nested contours alternate filled / hole
};

// Font style bits for FontDesc::flags (mirrors the HELIOSVIEW_FONT_* macros; OR
// them together).
enum class FontFlag : uint32_t {
	None = 0,
	Bold = HELIOSVIEW_FONT_BOLD,
	Italic = HELIOSVIEW_FONT_ITALIC,
	Underline = HELIOSVIEW_FONT_UNDERLINE,
	Strikeout = HELIOSVIEW_FONT_STRIKEOUT,
};

inline constexpr FontFlag operator|(FontFlag a, FontFlag b)
{
	return static_cast<FontFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr uint32_t toUint(FontFlag flags)
{
	return static_cast<uint32_t>(flags);
}

// Alignment of one line of text inside a box, for Painter::drawTextEx (mirrors the
// HELIOSVIEW_ALIGN_* macros; OR one horizontal with one vertical bit).
enum class TextAlign : uint32_t {
	Left = HELIOSVIEW_ALIGN_LEFT,         // default
	HCenter = HELIOSVIEW_ALIGN_HCENTER,
	Right = HELIOSVIEW_ALIGN_RIGHT,
	Top = HELIOSVIEW_ALIGN_TOP,           // default
	VCenter = HELIOSVIEW_ALIGN_VCENTER,
	Bottom = HELIOSVIEW_ALIGN_BOTTOM,
	Baseline = HELIOSVIEW_ALIGN_BASELINE, // y is the baseline, not the top of the line box
};

inline constexpr TextAlign operator|(TextAlign a, TextAlign b)
{
	return static_cast<TextAlign>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr uint32_t toUint(TextAlign align)
{
	return static_cast<uint32_t>(align);
}

/* ---------- value types ---------- */

// A font: family, nominal pixel size (not points) and style bits (mirrors
// heliosview_font_desc_t; the family string is owned here, the C layer copies it).
// An unknown family falls back to the platform default instead of failing.
struct FontDesc {
	std::string family; // "" selects the platform's default sans-serif
	float size = 12.0f; // em size in pixels; <= 0 selects 12
	FontFlag flags = FontFlag::None;
};

// Everything that decides how a drawing call draws (mirrors
// heliosview_painter_state_t). The transform and the clip are separate: they are
// part of the save()/restore() stack, but not of this struct.
struct PainterState {
	uint32_t strokeColor = 0x00000000u; // ARGB; alpha 0 = no outline
	uint32_t fillColor = 0x00000000u;   // ARGB; alpha 0 = no fill
	float strokeWidth = 1.0f;           // line width in pixels; <= 0 means 1
	LineCap lineCap = LineCap::Butt;
	LineJoin lineJoin = LineJoin::Miter;
	FontDesc font;
	float alpha = 1.0f;    // 0..1 global opacity, multiplied into every color
	bool antialias = true; // smooth edges and text
};

// The measured size of one line of text (mirrors heliosview_text_metrics_t). These
// are the engine's own metrics: never treat them as cross-platform constants.
struct TextMetrics {
	float width = 0.0f;      // advance width of the line
	float height = 0.0f;     // height of the line box (ascent + descent)
	float ascent = 0.0f;     // top of the line box down to the baseline
	float descent = 0.0f;    // baseline down to the bottom of the line box
	float lineHeight = 0.0f; // recommended distance between consecutive lines
};

// A 2x3 affine matrix [a b c d e f] (mirrors the float[6] the C transform calls
// take), applied as x' = a*x + c*y + e, y' = b*x + d*y + f -- the row-vector,
// x-right / y-down layout Win32's XFORM and CSS' matrix() also use.
using Matrix = std::array<float, 6>;

inline constexpr Matrix identityMatrix()
{
	return {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
}

/* ---------- conversions to/from the C forms ---------- */

inline heliosview_canvas_engine_t toC(PaintEngine engine)
{
	return static_cast<heliosview_canvas_engine_t>(engine);
}

inline PaintEngine fromC(heliosview_canvas_engine_t engine)
{
	return static_cast<PaintEngine>(engine);
}

inline heliosview_pixel_format_t toC(PixelFormat format)
{
	return static_cast<heliosview_pixel_format_t>(format);
}

inline PixelFormat fromC(heliosview_pixel_format_t format)
{
	return static_cast<PixelFormat>(format);
}

inline heliosview_canvas_feature_t toC(PaintFeature feature)
{
	return static_cast<heliosview_canvas_feature_t>(feature);
}

inline heliosview_line_cap_t toC(LineCap cap)
{
	return static_cast<heliosview_line_cap_t>(cap);
}

inline heliosview_line_join_t toC(LineJoin join)
{
	return static_cast<heliosview_line_join_t>(join);
}

inline heliosview_path_winding_t toC(PathWinding winding)
{
	return static_cast<heliosview_path_winding_t>(winding);
}

inline heliosview_rect_t toC(const Rect& rect)
{
	return {rect.x, rect.y, rect.width, rect.height};
}

// The returned struct borrows FontDesc::family: use it in the call it was made for
// (the C layer copies the string during that call, so the FontDesc may be temporary).
inline heliosview_font_desc_t toC(const FontDesc& desc)
{
	heliosview_font_desc_t c{};
	c.family = desc.family.empty() ? nullptr : desc.family.c_str();
	c.size = desc.size;
	c.flags = toUint(desc.flags);
	return c;
}

inline FontDesc fromC(const heliosview_font_desc_t& desc)
{
	FontDesc out;
	if (desc.family != nullptr)
		out.family = desc.family;
	out.size = desc.size;
	out.flags = static_cast<FontFlag>(desc.flags);
	return out;
}

// Same borrowing rule for the font member: do not keep the result of this call
// around while the family string may already be gone (it is used immediately).
inline heliosview_painter_state_t toC(const PainterState& state)
{
	heliosview_painter_state_t c{};
	c.stroke_color = state.strokeColor;
	c.fill_color = state.fillColor;
	c.stroke_width = state.strokeWidth;
	c.line_cap = toC(state.lineCap);
	c.line_join = toC(state.lineJoin);
	c.font = toC(state.font);
	c.alpha = state.alpha;
	c.antialias = state.antialias ? 1 : 0;
	return c;
}

// Copies what it needs, including the family string: unlike the C struct this comes
// from, the result stays valid on its own.
inline PainterState fromC(const heliosview_painter_state_t& state)
{
	PainterState out;
	out.strokeColor = state.stroke_color;
	out.fillColor = state.fill_color;
	out.strokeWidth = state.stroke_width;
	out.lineCap = static_cast<LineCap>(state.line_cap);
	out.lineJoin = static_cast<LineJoin>(state.line_join);
	out.font = fromC(state.font);
	out.alpha = state.alpha;
	out.antialias = state.antialias != 0;
	return out;
}

inline TextMetrics fromC(const heliosview_text_metrics_t& metrics)
{
	TextMetrics out;
	out.width = metrics.width;
	out.height = metrics.height;
	out.ascent = metrics.ascent;
	out.descent = metrics.descent;
	out.lineHeight = metrics.line_height;
	return out;
}

/* ---------- engine and codec queries ---------- */

// Whether the engine is part of this build (Auto: whether any engine is). Independent
// of whether it works here -- that is engineProbe.
inline bool engineCompiled(PaintEngine engine)
{
	return heliosview_engine_compiled(toC(engine)) != 0;
}

// Whether the engine can be used right now: compiled and able to initialize. This is
// the call to make before forcing an engine (on a machine without a GPU,
// PaintEngine::Accelerated probes false and PaintEngine::Auto still works).
inline bool engineProbe(PaintEngine engine)
{
	return heliosview_engine_probe(toC(engine)) != 0;
}

// How many engines this build provides, and the value of the i-th of them
// (0 <= index < engineCount()).
inline int engineCount()
{
	return heliosview_engine_count();
}

inline PaintEngine engineAt(int index)
{
	return fromC(heliosview_engine_at(index));
}

// The engines this build provides, in index order.
inline std::vector<PaintEngine> engines()
{
	const int count = engineCount();
	std::vector<PaintEngine> out;
	if (count > 0)
		out.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i)
		out.push_back(engineAt(i));
	return out;
}

// Short lowercase engine name for logs: "gdi+", "gdi", "d2d", "coregraphics",
// "metal", "cairo", "software", "auto". Never empty.
inline std::string engineName(PaintEngine engine)
{
	return heliosview_engine_name(toC(engine));
}

inline bool engineSupportsFeature(PaintEngine engine, PaintFeature feature)
{
	return heliosview_engine_supports_feature(toC(engine), toC(feature)) != 0;
}

// The library-wide default engine, used wherever Auto is passed to a canvas creation
// call. Set it once, before creating canvases; Auto restores the default.
inline void setDefaultPaintEngine(PaintEngine engine)
{
	heliosview_set_default_canvas_engine(toC(engine));
}

inline PaintEngine defaultPaintEngine()
{
	return fromC(heliosview_default_canvas_engine());
}

// Whether the codec can read (forEncoding == false) or write (true) the named format
// ("png", "jpeg"/"jpg", "bmp", "tga", ...). Independent of the engine, because the
// codec is: image I/O works even where no drawing engine does.
inline bool formatSupported(std::string_view format, bool forEncoding = false)
{
	const std::string name(format);
	return heliosview_format_supported(name.c_str(), forEncoding ? 1 : 0) != 0;
}

/* ---------- Canvas ---------- */

// A block of pixels with a size, a stride and a pixel format: a drawing target, an
// image (load / save / encode) and a source to draw from. Owns the C canvas and
// destroys it; move-only. An invalid Canvas (the default-constructed one, or a
// failed factory call) is a plain empty object, not a broken one: everything on it
// fails and nothing crashes.
//
// Drawing on it needs a Painter; while one is active the canvas cannot be resized,
// filled, saved or blitted (the C layer rejects that with
// HELIOSVIEW_ERROR_INVALID_STATE), because the engine's copy of the pixels is not
// flushed until the painter ends.
class Canvas {
public:
	// An empty canvas: nothing to draw on, everything fails. Useful as a member that
	// is assigned later, and as the result of a failed load().
	Canvas() = default;

	// A width x height canvas, its pixels fully transparent. Invalid (ask valid())
	// when the size is not positive, the format is unsupported or the engine is
	// unavailable.
	Canvas(int32_t width, int32_t height, PixelFormat format = PixelFormat::Auto,
		   PaintEngine engine = PaintEngine::Auto)
		: m_canvas(heliosview_canvas_create(width, height, toC(format), toC(engine)))
	{
	}

	~Canvas()
	{
		heliosview_canvas_destroy(m_canvas);
	}

	Canvas(const Canvas&) = delete;
	Canvas& operator=(const Canvas&) = delete;

	Canvas(Canvas&& other) noexcept : m_canvas(other.m_canvas)
	{
		other.m_canvas = nullptr;
	}

	Canvas& operator=(Canvas&& other) noexcept
	{
		if (this != &other) {
			heliosview_canvas_destroy(m_canvas);
			m_canvas = other.m_canvas;
			other.m_canvas = nullptr;
		}
		return *this;
	}

	// True when the canvas exists (see the constructor and the factories below)
	bool valid() const
	{
		return m_canvas != nullptr;
	}

	// The underlying C handle (NULL when !valid()), for the C API calls this wrapper
	// does not cover.
	heliosview_canvas_t* handle() const
	{
		return m_canvas;
	}

	/* ---- creating canvases ---- */

	// Decode an image file (UTF-8 path; the codec picks the format from the content,
	// not from the extension). Invalid on failure, with the reason recorded.
	static Canvas load(std::string_view path, PixelFormat format = PixelFormat::Auto,
					   PaintEngine engine = PaintEngine::Auto)
	{
		const std::string file(path);
		return Canvas(heliosview_canvas_load(file.c_str(), toC(format), toC(engine)));
	}

	// Decode an in-memory image (the bytes of a PNG / JPEG / ... file). `data` is only
	// read, and only during the call.
	static Canvas decode(const void* data, size_t size, PixelFormat format = PixelFormat::Auto,
						 PaintEngine engine = PaintEngine::Auto)
	{
		return Canvas(heliosview_canvas_load_memory(data, size, toC(format), toC(engine)));
	}

	// A copy of this canvas, in `format` (Auto = the source's) and on `engine` (Auto =
	// the source's): how a canvas moves between engines or formats. Invalid on failure.
	Canvas clone(PixelFormat format = PixelFormat::Auto, PaintEngine engine = PaintEngine::Auto) const
	{
		return Canvas(heliosview_canvas_clone(m_canvas, toC(format), toC(engine)));
	}

	/* ---- geometry, format, engine ---- */

	// Size in pixels (0 when !valid())
	int32_t width() const
	{
		int32_t width = 0;
		int32_t height = 0;
		heliosview_canvas_size(m_canvas, &width, &height);
		return width;
	}

	int32_t height() const
	{
		int32_t width = 0;
		int32_t height = 0;
		heliosview_canvas_size(m_canvas, &width, &height);
		return height;
	}

	// Row pitch in bytes (>= width * bytes per pixel); 0 when !valid()
	int32_t stride() const
	{
		return heliosview_canvas_stride(m_canvas);
	}

	// The concrete format the canvas was created with (never Auto when valid())
	PixelFormat format() const
	{
		return fromC(heliosview_canvas_format(m_canvas));
	}

	// The concrete engine the canvas landed on (never Auto when valid())
	PaintEngine engine() const
	{
		return fromC(heliosview_canvas_engine(m_canvas));
	}

	/* ---- pixels ---- */

	// Resize the canvas. The content is NOT preserved. Fails while a painter is active.
	bool resize(int32_t width, int32_t height)
	{
		return heliosview_canvas_resize(m_canvas, width, height) == 0;
	}

	// Fill the whole canvas with an ARGB color, ignoring painter state, transform and
	// clipping -- the "clear the canvas" primitive. The pixels start fully transparent.
	bool fill(uint32_t argb)
	{
		return heliosview_canvas_fill(m_canvas, argb) == 0;
	}

	// Write one pixel, in canvas coordinates and ARGB (on a premultiplied canvas the
	// value is converted; a color with alpha 0 clears the pixel). Out-of-range
	// coordinates fail.
	bool setPixel(int32_t x, int32_t y, uint32_t argb)
	{
		return heliosview_canvas_set_pixel(m_canvas, x, y, argb) == 0;
	}

	// Read one pixel as straight ARGB, or std::nullopt when the coordinates are out of
	// range (there is no valid value to return).
	std::optional<uint32_t> getPixel(int32_t x, int32_t y) const
	{
		uint32_t argb = 0;
		if (heliosview_canvas_get_pixel(m_canvas, x, y, &argb) != 0)
			return std::nullopt;
		return argb;
	}

	// The live pixel buffer (stride bytes per row, in this canvas's format), or NULL
	// when the engine has no directly addressable pixels: check
	// engineSupportsFeature(engine(), PaintFeature::DirectPixels) first. After writing
	// through it, call endWrite() so the engine drops its cached copy.
	void* data()
	{
		return heliosview_canvas_data(m_canvas);
	}

	// Announce a direct write through data(). Fails when there was none.
	bool endWrite()
	{
		return heliosview_canvas_end_write(m_canvas) == 0;
	}

	// Copy this canvas onto `dst` at (x, y): no scaling and no transform, but format
	// conversion and alpha blending (0..1 scales the source's opacity). `srcRect`
	// selects a region of this canvas (NULL = all of it); a region may not run past
	// the edges. Copying a canvas onto itself fails.
	bool blitTo(Canvas& dst, int32_t x, int32_t y, const Rect* srcRect = nullptr, float alpha = 1.0f)
	{
		heliosview_rect_t rect{};
		if (srcRect != nullptr)
			rect = toC(*srcRect);
		return heliosview_canvas_blit(m_canvas, dst.handle(), x, y, srcRect != nullptr ? &rect : nullptr, alpha) == 0;
	}

	/* ---- saving / encoding ---- */

	// Encode into a freshly allocated buffer. `format` is "png" / "jpeg" / "jpg" /
	// "bmp" / "tga" (case-insensitive); `quality` is 1..100 for the lossy ones (0 =
	// default 92); `background` is the ARGB color a format without alpha is flattened
	// onto (0 = black). An empty vector means the encode failed.
	std::vector<uint8_t> encode(std::string_view format = "png", int quality = 0, uint32_t background = 0)
	{
		const std::string name(format);
		void* bytes = nullptr;
		size_t size = 0;
		const int encoded = heliosview_canvas_encode(m_canvas, name.c_str(), quality, background, &bytes, &size);
		if (encoded <= 0 || bytes == nullptr) {
			heliosview_free(bytes);
			return {};
		}
		const auto* begin = static_cast<const uint8_t*>(bytes);
		std::vector<uint8_t> out(begin, begin + size);
		heliosview_free(bytes);
		return out;
	}

	// The byte count encode() would produce with the same arguments, without encoding
	// into memory (the C API's two-phase pattern); 0 when it would fail.
	size_t encodedSize(std::string_view format = "png", int quality = 0, uint32_t background = 0)
	{
		const std::string name(format);
		size_t size = 0;
		const int encoded = heliosview_canvas_encode(m_canvas, name.c_str(), quality, background, nullptr, &size);
		return encoded > 0 ? size : 0;
	}

	// Encode straight into a file (UTF-8 path). `format` as in encode(); empty infers
	// it from the extension (.png, .jpg/.jpeg, .bmp, .tga) and fails on anything else.
	bool save(std::string_view path, std::string_view format = {}, int quality = 0, uint32_t background = 0)
	{
		const std::string file(path);
		const std::string name(format);
		return heliosview_canvas_save(m_canvas, file.c_str(), name.empty() ? nullptr : name.c_str(), quality,
									  background) == 0;
	}

private:
	// Adopt a C canvas handed over by the C API (NULL = invalid)
	explicit Canvas(heliosview_canvas_t* canvas) : m_canvas(canvas)
	{
	}

	heliosview_canvas_t* m_canvas = nullptr;
};

/* ---------- Path ---------- */

// A shape description: a sequence of sub-shapes that can be filled, stroked and used
// as a clip any number of times, on any canvas, under any transform. Owns the C path;
// move-only. Only invalid when out of memory -- an empty path is a valid empty shape.
//
// The verbs are fire-and-forget (the C API reports no error: a path is just data).
// Validate the geometry by filling it and looking at the pixels.
class Path {
public:
	Path() : m_path(heliosview_path_create())
	{
	}

	~Path()
	{
		heliosview_path_destroy(m_path);
	}

	Path(const Path&) = delete;
	Path& operator=(const Path&) = delete;

	Path(Path&& other) noexcept : m_path(other.m_path)
	{
		other.m_path = nullptr;
	}

	Path& operator=(Path&& other) noexcept
	{
		if (this != &other) {
			heliosview_path_destroy(m_path);
			m_path = other.m_path;
			other.m_path = nullptr;
		}
		return *this;
	}

	bool valid() const
	{
		return m_path != nullptr;
	}

	heliosview_path_t* handle() const
	{
		return m_path;
	}

	// Forget every sub-shape, keeping the winding rule
	void reset()
	{
		heliosview_path_reset(m_path);
	}

	/* ---- the verbs ---- */

	// Start a new sub-shape at (x, y)
	void moveTo(float x, float y)
	{
		heliosview_path_move_to(m_path, x, y);
	}

	// Straight segment from the current point
	void lineTo(float x, float y)
	{
		heliosview_path_line_to(m_path, x, y);
	}

	// Quadratic segment from the current point: one control point (cx, cy), then (x, y)
	void quadTo(float cx, float cy, float x, float y)
	{
		heliosview_path_quad_to(m_path, cx, cy, x, y);
	}

	// Cubic segment from the current point: two control points, then (x, y)
	void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
	{
		heliosview_path_cubic_to(m_path, c1x, c1y, c2x, c2y, x, y);
	}

	// Connect the current point back to the sub-shape's start: fill and stroke treat
	// the contour as closed.
	void close()
	{
		heliosview_path_close(m_path);
	}

	void addRect(float x, float y, float width, float height)
	{
		heliosview_path_add_rect(m_path, x, y, width, height);
	}

	// `radius` is clamped to half the shorter side; 0 gives a plain rectangle
	void addRoundRect(float x, float y, float width, float height, float radius)
	{
		heliosview_path_add_round_rect(m_path, x, y, width, height, radius);
	}

	void addEllipse(float x, float y, float width, float height)
	{
		heliosview_path_add_ellipse(m_path, x, y, width, height);
	}

	/* ---- the winding rule ---- */

	void setWinding(PathWinding winding)
	{
		heliosview_path_set_winding(m_path, toC(winding));
	}

	PathWinding winding() const
	{
		return static_cast<PathWinding>(heliosview_path_winding(m_path));
	}

private:
	heliosview_path_t* m_path = nullptr;
};

/* ---------- Painter ---------- */

// Draws onto one Canvas. The session begins in the constructor and ends in the
// destructor (which flushes the engine and releases the canvas), so the natural
// shape is a scope:
//
//     { Painter p(canvas); p.clear(...); p.fillRect(...); }
//
// The state always starts at the documented defaults (stroke and fill off, width 1,
// butt / miter, default 12px font, alpha 1, antialias on, identity transform, clip =
// the whole canvas), so a painter is predictable and two scopes cannot interfere.
//
// Move-only; the Canvas must outlive it. Invalid when the canvas is invalid or a
// painter is already active on it. After end(), the methods fail instead of drawing
// on released state.
class Painter {
public:
	explicit Painter(Canvas& canvas)
		: m_painter(canvas.valid() ? heliosview_painter_begin(canvas.handle()) : nullptr)
	{
	}

	~Painter()
	{
		// Flush and release. end() is idempotent, so calling it explicitly first is fine.
		if (m_painter != nullptr)
			heliosview_painter_end(m_painter);
	}

	Painter(const Painter&) = delete;
	Painter& operator=(const Painter&) = delete;

	Painter(Painter&& other) noexcept : m_painter(other.m_painter)
	{
		other.m_painter = nullptr;
	}

	Painter& operator=(Painter&& other) noexcept
	{
		if (this != &other) {
			end();
			m_painter = other.m_painter;
			other.m_painter = nullptr;
		}
		return *this;
	}

	// True when the session started (a valid canvas, no painter already active on it)
	bool valid() const
	{
		return m_painter != nullptr;
	}

	heliosview_painter_t* handle() const
	{
		return m_painter;
	}

	// End the session now: the canvas is complete and safe to save, read, blit or
	// resize. Returns false when it had already ended (the destructor's no-op case).
	bool end()
	{
		if (m_painter == nullptr)
			return false;
		heliosview_painter_t* painter = m_painter;
		m_painter = nullptr;
		return heliosview_painter_end(painter) == 0;
	}

	/* ---- state and style ---- */

	// Outline color (ARGB) and width in pixels, used by the stroke calls
	bool setStroke(uint32_t argb, float width)
	{
		return heliosview_painter_set_stroke(m_painter, argb, width) == 0;
	}

	// Fill color (ARGB): the color the shape verbs and fillPath() fill with
	bool setFill(uint32_t argb)
	{
		return heliosview_painter_set_fill(m_painter, argb) == 0;
	}

	// The font used by drawText / drawTextEx / measureText. The string is copied --
	// a temporary FontDesc is fine.
	bool setFont(const FontDesc& desc)
	{
		const heliosview_font_desc_t c = toC(desc);
		return heliosview_painter_set_font(m_painter, &c) == 0;
	}

	// Global opacity 0..1, multiplied into every color drawn from now on
	bool setAlpha(float alpha)
	{
		return heliosview_painter_set_alpha(m_painter, alpha) == 0;
	}

	// Smooth edges and text (on by default)
	bool setAntialias(bool on)
	{
		return heliosview_painter_set_antialias(m_painter, on ? 1 : 0) == 0;
	}

	// Read the whole state back, including the current font
	bool state(PainterState& out) const
	{
		heliosview_painter_state_t c{};
		if (heliosview_painter_get_state(m_painter, &c) != 0)
			return false;
		out = fromC(c);
		return true;
	}

	// Change several fields at once (get the current state, edit it, set it back).
	// Only the fields in PainterState are touched: the transform and the clip are not.
	bool setState(const PainterState& state)
	{
		const heliosview_painter_state_t c = toC(state);
		return heliosview_painter_set_state(m_painter, &c) == 0;
	}

	/* ---- transform (applies to everything drawn from here on) ---- */

	bool setTransform(const Matrix& matrix)
	{
		return heliosview_painter_set_transform(m_painter, matrix.data()) == 0;
	}

	bool setTransform(const float* matrix6)
	{
		return heliosview_painter_set_transform(m_painter, matrix6) == 0;
	}

	bool transform(Matrix& out) const
	{
		return heliosview_painter_get_transform(m_painter, out.data()) == 0;
	}

	// Shift the coordinate system by (dx, dy)
	bool translate(float dx, float dy)
	{
		return heliosview_painter_translate(m_painter, dx, dy) == 0;
	}

	// Scale about the origin: 4, 4 turns a 10px square into a 40px one
	bool scale(float sx, float sy)
	{
		return heliosview_painter_scale(m_painter, sx, sy) == 0;
	}

	// Rotate about the origin by `radians`, clockwise on screen (y grows downwards)
	bool rotate(float radians)
	{
		return heliosview_painter_rotate(m_painter, radians) == 0;
	}

	// Shear: x' = x + kx*y, y' = y + ky*x
	bool skew(float kx, float ky)
	{
		return heliosview_painter_skew(m_painter, kx, ky) == 0;
	}

	// Back to the identity transform
	bool resetTransform()
	{
		return heliosview_painter_reset_transform(m_painter) == 0;
	}

	/* ---- clipping ---- */

	// Clip to a rectangle, replacing any previous clip. The rectangle is in the
	// transform's own space, so under a translate(40,40) a clip at (0,0,40,40) lands at
	// (40,40) on the canvas. Infinite rectangles are the way to say "no clip in x".
	bool setClipRect(float x, float y, float width, float height)
	{
		return heliosview_painter_set_clip_rect(m_painter, x, y, width, height) == 0;
	}

	// Clip to a path, replacing any previous clip. `path` is copied at the call; the
	// Path object itself can be changed or destroyed afterwards.
	bool setClipPath(Path& path)
	{
		return heliosview_painter_set_clip_path(m_painter, path.handle()) == 0;
	}

	// Narrow the current clip to its intersection with a rectangle (the usual way to
	// build a composite clip after setClipPath)
	bool intersectClipRect(float x, float y, float width, float height)
	{
		return heliosview_painter_intersect_clip_rect(m_painter, x, y, width, height) == 0;
	}

	// Remove the clip: draw anywhere on the canvas again
	bool resetClip()
	{
		return heliosview_painter_reset_clip(m_painter) == 0;
	}

	// Bounding box of the current clip, in canvas coordinates. An engine may return a
	// box that encloses the clip instead of the exact clip.
	bool clipBounds(Rect& out) const
	{
		heliosview_rect_t rect{};
		if (heliosview_painter_clip_bounds(m_painter, &rect) != 0)
			return false;
		out = toRect(rect);
		return true;
	}

	/* ---- drawing ---- */

	// Fill the whole canvas, ignoring the transform and the clip (but not the state:
	// unlike Canvas::fill, this is a painter call and blends)
	bool clear(uint32_t argb)
	{
		return heliosview_painter_clear(m_painter, argb) == 0;
	}

	// Rectangle filled with `argb` in one call: the shape verbs fill with their
	// argument, not with the fill_color state.
	bool fillRect(float x, float y, float width, float height, uint32_t argb)
	{
		return heliosview_painter_fill_rect(m_painter, x, y, width, height, argb) == 0;
	}

	// Straight line with the stroke color and width
	bool drawLine(float x1, float y1, float x2, float y2)
	{
		return heliosview_painter_draw_line(m_painter, x1, y1, x2, y2) == 0;
	}

	// Rectangle: fill_color inside, stroke_color as the outline
	bool drawRect(float x, float y, float width, float height)
	{
		return heliosview_painter_draw_rect(m_painter, x, y, width, height) == 0;
	}

	bool drawRoundRect(float x, float y, float width, float height, float radius)
	{
		return heliosview_painter_draw_round_rect(m_painter, x, y, width, height, radius) == 0;
	}

	// Ellipse inscribed in the rectangle
	bool drawEllipse(float x, float y, float width, float height)
	{
		return heliosview_painter_draw_ellipse(m_painter, x, y, width, height) == 0;
	}

	// Elliptical arc, outline only: 0 degrees is 3 o'clock and grows clockwise;
	// `sweepDeg` may be negative
	bool drawArc(float x, float y, float width, float height, float startDeg, float sweepDeg)
	{
		return heliosview_painter_draw_arc(m_painter, x, y, width, height, startDeg, sweepDeg) == 0;
	}

	// Stroked polyline through `count` (x, y) pairs; `closed` connects the last point
	// back to the first
	bool drawPolyline(const float* points, size_t count, bool closed = false)
	{
		return heliosview_painter_draw_polyline(m_painter, points, count, closed ? 1 : 0) == 0;
	}

	// Filled polygon through `count` (x, y) pairs (>= 3 points)
	bool drawPolygon(const float* points, size_t count)
	{
		return heliosview_painter_draw_polygon(m_painter, points, count) == 0;
	}

	/* ---- paths ---- */

	// Fill then stroke `path`, using fill_color and stroke_color
	bool drawPath(Path& path)
	{
		return heliosview_painter_draw_path(m_painter, path.handle()) == 0;
	}

	// Fill `path` with `argb` (the fill_color state is ignored)
	bool fillPath(Path& path, uint32_t argb)
	{
		return heliosview_painter_fill_path(m_painter, path.handle(), argb) == 0;
	}

	// Stroke `path` with `argb` and `width`
	bool strokePath(Path& path, uint32_t argb, float width)
	{
		return heliosview_painter_stroke_path(m_painter, path.handle(), argb, width) == 0;
	}

	/* ---- text ---- */

	// Draw one line of UTF-8 text with the top-left of its line box at (x, y), using
	// fill_color. No line breaking and no wrapping: split the string yourself, using
	// lineHeight() as the step.
	bool drawText(std::string_view utf8, float x, float y)
	{
		const std::string text(utf8);
		return heliosview_painter_draw_text(m_painter, text.c_str(), x, y) == 0;
	}

	// Draw one line of UTF-8 text aligned inside a box, clipped to it: the box may be
	// larger or smaller than the text.
	bool drawTextEx(std::string_view utf8, float x, float y, float width, float height, TextAlign align)
	{
		const std::string text(utf8);
		return heliosview_painter_draw_text_ex(m_painter, text.c_str(), x, y, width, height, toUint(align)) == 0;
	}

	// Measure one line of UTF-8 text with the current font (this is what tells you how
	// wide a string will be)
	bool measureText(std::string_view utf8, TextMetrics& out)
	{
		const std::string text(utf8);
		heliosview_text_metrics_t metrics{};
		if (heliosview_painter_measure_text(m_painter, text.c_str(), &metrics) != 0)
			return false;
		out = fromC(metrics);
		return true;
	}

	// The recommended distance between consecutive lines with the current font, or
	// std::nullopt when the engine cannot say
	std::optional<float> lineHeight() const
	{
		float height = 0.0f;
		if (heliosview_painter_line_height(m_painter, &height) != 0)
			return std::nullopt;
		return height;
	}

	/* ---- images ---- */

	// Draw another canvas -- an image or a sprite sheet -- into (dx, dy, dw, dh),
	// scaled to fit, under the current transform, clip, alpha and antialias. `srcRect`
	// selects a region of the source (NULL = all of it); a negative dw / dh mirrors it.
	// The source may be any canvas, any format, any engine -- but not the canvas being
	// drawn into.
	bool drawImage(Canvas& image, float dx, float dy, float dw, float dh, const Rect* srcRect = nullptr,
				   float alpha = 1.0f)
	{
		heliosview_rect_t rect{};
		if (srcRect != nullptr)
			rect = toC(*srcRect);
		return heliosview_painter_draw_image(m_painter, image.handle(), dx, dy, dw, dh,
											srcRect != nullptr ? &rect : nullptr, alpha) == 0;
	}

	/* ---- the state stack ---- */

	// Push the whole drawing state: stroke, fill, font, alpha, transform and clip.
	// These are the calls that make nested drawing routines safe.
	bool save()
	{
		return heliosview_painter_save(m_painter) == 0;
	}

	// Pop back to the save()d state. Fails when nothing was saved.
	bool restore()
	{
		return heliosview_painter_restore(m_painter) == 0;
	}

	// RAII save()/restore() for a scope:
	//
	//     { Painter::StateGuard guard(p); p.translate(100, 100); p.drawRect(...); }
	//     // the transform (and every other state field) is back
	//
	// Unlike a bare save()/restore() pair this survives an early return or an
	// exception. saved() is false when the save itself failed (there is then nothing
	// to restore).
	class StateGuard {
	public:
		explicit StateGuard(Painter& painter) : m_painter(&painter), m_saved(painter.save())
		{
		}

		~StateGuard()
		{
			if (m_saved)
				m_painter->restore();
		}

		StateGuard(const StateGuard&) = delete;
		StateGuard& operator=(const StateGuard&) = delete;

		bool saved() const
		{
			return m_saved;
		}

	private:
		Painter* m_painter;
		bool m_saved;
	};

private:
	heliosview_painter_t* m_painter = nullptr;
};

} // namespace helios
