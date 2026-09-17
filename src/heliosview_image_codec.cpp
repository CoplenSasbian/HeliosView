// HeliosView -- image codec: decode/encode for every engine alike.
//
// This is deliberately NOT part of an engine: the pixels live in the canvas and the
// rasterization lives in the engine, but turning an image file into pixels (and
// back) is a separate, dependency-free job. Doing it once here means every engine
// and every platform reads and writes exactly the same formats with the same bytes
// -- a canvas saved on Windows opens identically everywhere, and adding an engine
// never means implementing a codec.
//
// The codec is stb (third_party/stb, public domain / MIT): read PNG, JPEG, BMP, TGA,
// GIF (first frame), PSD, HDR, PNM, PIC; write PNG, JPEG, BMP, TGA. Deliberately
// absent: EXIF orientation, ICC profiles, 16-bit samples, animated GIF frames (see
// the format list in heliosview_canvas.h).
//
// The canvas keeps its own file reading only for the "path" entry point: the codec
// works on bytes (load_memory / encode), and load(path) hands it a buffer.

#include "heliosview_canvas_internal.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457 4701 4702 4703 4996) /* stb is not /W4 clean */
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO 1 /* the codec works on memory; the library does its own file I/O */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#define STBI_ONLY_PNM
#define STBI_ONLY_PIC
#include <stb/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO 1
#include <stb/stb_image_write.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace hv::canvas {

namespace {

/* ================= Format tables =================
 *
 * One table for the names the public API accepts, so heliosview_format_supported,
 * the extension inference and the encoder lookup cannot drift apart. */

struct FormatInfo {
    const char* name;
    bool can_read;
    bool can_write;
    bool has_alpha; /* writing it can keep transparency */
};

constexpr FormatInfo kFormats[] = {
    /* name   read   write  alpha */
    {"png",    true,  true,  true},
    {"jpeg",   true,  true,  false},
    {"jpg",    true,  true,  false}, /* alias of jpeg */
    {"bmp",    true,  true,  false},
    {"tga",    true,  true,  true},
    {"gif",    true,  false, false}, /* decode only, first frame */
    {"psd",    true,  false, false},
    {"hdr",    true,  false, false},
    {"pic",    true,  false, false},
    {"pnm",    true,  false, false},
    {"pgm",    true,  false, false},
    {"ppm",    true,  false, false},
};

/* Lowercase ASCII copy of a format name, so lookups are case-insensitive without
 * locale surprises. */
std::string lowercase(const char* text)
{
    std::string out;
    if (!text)
        return out;
    for (const char* p = text; *p; ++p)
        out.push_back(static_cast<char>((*p >= 'A' && *p <= 'Z') ? (*p - 'A' + 'a') : *p));
    return out;
}

const FormatInfo* find_format(const char* name)
{
    if (!name || !*name)
        return nullptr;
    const std::string lower = lowercase(name);
    for (const FormatInfo& info : kFormats)
        if (lower == info.name)
            return &info;
    return nullptr;
}

/* Format names that are aliases of another encoder resolve to the canonical
 * name stb knows. */
const char* canonical_encoder(const std::string& lower)
{
    if (lower == "jpg")
        return "jpeg";
    return nullptr; /* not an alias: use the name as given */
}

/* ================= Byte buffer sink for stb_image_write ================= */

void append_bytes(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    if (size <= 0 || !data)
        return;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/* ================= Paths ================= */

/* Read a whole file into memory. Returns false when the file cannot be read; the
 * caller can then distinguish "missing" from "unsupported" using the path itself. */
bool read_file(const char* path, std::vector<uint8_t>& out)
{
    std::FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        return false;
    }
    std::rewind(file);
    try {
        out.resize(static_cast<size_t>(size));
    } catch (const std::bad_alloc&) {
        std::fclose(file);
        return false;
    }
    const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

/* Whether the codec can actually decode these bytes: stbi_info parses the header
 * without decoding, so an unsupported or corrupt file is rejected here instead of
 * silently yielding a zero-sized canvas. */
bool probe_memory(const void* data, size_t size, int* out_width, int* out_height)
{
    if (!data || size == 0 || size > static_cast<size_t>(INT32_MAX))
        return false;
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(size), &width,
                              &height, &channels))
        return false;
    if (width <= 0 || height <= 0)
        return false;
    if (out_width)
        *out_width = width;
    if (out_height)
        *out_height = height;
    return true;
}

/* ================= Canvas channel access =================
 *
 * Decoding normalizes to one of the canvas formats; these two functions are the
 * only place that touches the raw layout, next to the format switch the core uses
 * for single pixels. */

bool format_is_gray(heliosview_pixel_format_t format)
{
    return format == HELIOSVIEW_FORMAT_GRAY8;
}

inline uint8_t gray_of(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint8_t>((static_cast<unsigned>(r) * 77u + static_cast<unsigned>(g) * 150u
                                 + static_cast<unsigned>(b) * 29u)
                                >> 8);
}

inline uint8_t premul8(uint8_t v, uint8_t a)
{
    return static_cast<uint8_t>((static_cast<unsigned>(v) * a + 127u) / 255u);
}

/* Write one straight RGBA sample into the canvas at (x, y). */
void store_rgba(const heliosview_canvas_t* canvas, uint8_t* pixels, int32_t stride, int32_t x,
                int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t* p = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride)
               + static_cast<size_t>(x) * static_cast<size_t>(heliosview_canvas_format(canvas) == HELIOSVIEW_FORMAT_GRAY8 ? 1 : 4);
    switch (heliosview_canvas_format(canvas)) {
    case HELIOSVIEW_FORMAT_BGRA8_PREMUL:
        p[0] = premul8(b, a);
        p[1] = premul8(g, a);
        p[2] = premul8(r, a);
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
        p[0] = gray_of(r, g, b);
        break;
    default:
        break;
    }
}

/* Read one pixel of the canvas as straight RGBA (the inverse of store_rgba),
 * for encoding. */
void load_rgba(const uint8_t* p, heliosview_pixel_format_t format, uint8_t* r, uint8_t* g,
               uint8_t* b, uint8_t* a)
{
    switch (format) {
    case HELIOSVIEW_FORMAT_BGRA8_PREMUL: {
        const uint8_t alpha = p[3];
        auto unpremul = [alpha](uint8_t v) {
            if (alpha == 0)
                return static_cast<uint8_t>(0);
            if (alpha == 255)
                return v;
            const unsigned out = (static_cast<unsigned>(v) * 255u + alpha / 2u) / alpha;
            return static_cast<uint8_t>(out > 255u ? 255u : out);
        };
        *b = unpremul(p[0]);
        *g = unpremul(p[1]);
        *r = unpremul(p[2]);
        *a = alpha;
        break;
    }
    case HELIOSVIEW_FORMAT_BGRA8:
        *b = p[0];
        *g = p[1];
        *r = p[2];
        *a = p[3];
        break;
    case HELIOSVIEW_FORMAT_RGBA8:
        *r = p[0];
        *g = p[1];
        *b = p[2];
        *a = p[3];
        break;
    case HELIOSVIEW_FORMAT_GRAY8:
        *r = *g = *b = p[0];
        *a = 255;
        break;
    default:
        *r = *g = *b = 0;
        *a = 0;
        break;
    }
}

} // namespace

/* ================= Public entry points used by the canvas core ================= */

bool codec_supports(const char* format, bool for_encoding)
{
    const FormatInfo* info = find_format(format);
    if (!info)
        return false;
    return for_encoding ? info->can_write : info->can_read;
}

std::string codec_format_from_path(const char* path)
{
    if (!path)
        return {};
    const char* dot = std::strrchr(path, '.');
    if (!dot || !dot[1])
        return {};
    /* A known encoder name only: this both infers the format and validates that the
     * extension is one we can actually write. */
    const FormatInfo* info = find_format(dot + 1);
    if (!info || !info->can_write)
        return {};
    return lowercase(dot + 1);
}

heliosview_canvas_t* codec_load_path(const char* path, heliosview_pixel_format_t format,
                                     heliosview_canvas_engine_t engine)
{
    if (!path || !*path) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "path is NULL or empty");
        return nullptr;
    }
    /* The codec owns the file I/O: it is the only layer that knows whether the bytes
     * it just read are a usable image. */
    std::vector<uint8_t> file;
    if (!read_file(path, file)) {
        hv_fail(HELIOSVIEW_ERROR_GENERIC, "cannot read the image file");
        return nullptr;
    }
    return codec_load_memory(file.data(), file.size(), format, engine);
}

heliosview_canvas_t* codec_load_memory(const void* data, size_t size, heliosview_pixel_format_t format,
                                       heliosview_canvas_engine_t engine)
{
    if (!data || size == 0) {
        hv_fail(HELIOSVIEW_ERROR_INVALID_ARGUMENT, "data is NULL or size is 0");
        return nullptr;
    }

    int width = 0, height = 0;
    if (!probe_memory(data, size, &width, &height)) {
        const char* reason = stbi_failure_reason();
        hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED,
                reason ? reason : "the image data is not in a supported format");
        return nullptr;
    }

    const int channels = format_is_gray(format) ? 1 : 4;
    stbi_uc* decoded = stbi_load_from_memory(static_cast<const stbi_uc*>(data),
                                             static_cast<int>(size), &width, &height, nullptr, channels);
    if (!decoded) {
        const char* reason = stbi_failure_reason();
        hv_fail(HELIOSVIEW_ERROR_GENERIC, reason ? reason : "decoding the image failed");
        return nullptr;
    }

    /* The canvas is created through the public API so the engine resolution, the
     * format defaults and the error reporting all behave exactly as for
     * heliosview_canvas_create -- the codec adds pixels, nothing else. */
    heliosview_canvas_t* canvas = heliosview_canvas_create(width, height, format, engine);
    if (!canvas) {
        stbi_image_free(decoded);
        return nullptr; /* the reason is already recorded by the creation call */
    }

    if (uint8_t* pixels = static_cast<uint8_t*>(heliosview_canvas_data(canvas))) {
        const int32_t stride = heliosview_canvas_stride(canvas);
        const int32_t bpp = format_is_gray(format) ? 1 : 4;
        /* stb hands back tightly packed rows, top-down (row 0 = top), which is the
         * canvas convention -- so this is a row copy through the stride, with no
         * flipping and no channel reordering on the gray path. */
        for (int y = 0; y < height; ++y) {
            const stbi_uc* row = decoded + static_cast<size_t>(y) * static_cast<size_t>(width) * bpp;
            if (format_is_gray(format)) {
                std::memcpy(pixels + static_cast<size_t>(y) * static_cast<size_t>(stride), row,
                            static_cast<size_t>(width));
            } else {
                for (int x = 0; x < width; ++x) {
                    const stbi_uc* px = row + static_cast<size_t>(x) * 4;
                    store_rgba(canvas, pixels, stride, x, y, px[0], px[1], px[2], px[3]);
                }
            }
        }
        heliosview_canvas_end_write(canvas);
    } else {
        /* An engine without DIRECT_PIXELS: write through the pixel accessor, which
         * every engine must provide. */
        for (int y = 0; y < height; ++y) {
            const stbi_uc* row = decoded + static_cast<size_t>(y) * static_cast<size_t>(width) * channels;
            for (int x = 0; x < width; ++x) {
                const stbi_uc* px = row + static_cast<size_t>(x) * channels;
                const uint32_t argb =
                    channels == 1
                        ? (0xFF000000u | (static_cast<uint32_t>(px[0]) << 16)
                           | (static_cast<uint32_t>(px[0]) << 8) | px[0])
                        : ((static_cast<uint32_t>(px[3]) << 24) | (static_cast<uint32_t>(px[0]) << 16)
                           | (static_cast<uint32_t>(px[1]) << 8) | px[2]);
                heliosview_canvas_set_pixel(canvas, x, y, argb);
            }
        }
    }
    stbi_image_free(decoded);
    return canvas;
}

int codec_encode(const CanvasData& canvas, const char* format, int quality, uint32_t background,
                 std::vector<uint8_t>& out)
{
    const std::string requested = lowercase(format);
    const FormatInfo* info = find_format(requested.c_str());
    if (!info || !info->can_write)
        return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "the image format cannot be written by the codec");

    const int width = canvas.width;
    const int height = canvas.height;
    if (width <= 0 || height <= 0 || !canvas.pixels)
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "the canvas has no pixels");

    /* Flatten onto `background` when the target format has no alpha: the caller
     * chooses the color, so a transparent canvas does not silently turn black. */
    const uint8_t bg_a = static_cast<uint8_t>((background >> 24) & 0xFF);
    const uint8_t bg_r = static_cast<uint8_t>((background >> 16) & 0xFF);
    const uint8_t bg_g = static_cast<uint8_t>((background >> 8) & 0xFF);
    const uint8_t bg_b = static_cast<uint8_t>(background & 0xFF);

    const int channels = info->has_alpha ? 4 : 3;
    std::vector<uint8_t> pixels;
    try {
        pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * channels);
    } catch (const std::bad_alloc&) {
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "out of memory preparing the image");
    }

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = canvas.pixels + static_cast<size_t>(y) * static_cast<size_t>(canvas.stride);
        for (int x = 0; x < width; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 0;
            load_rgba(row + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel(canvas.format)),
                      canvas.format, &r, &g, &b, &a);
            uint8_t* out_px = pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width)
                                               + static_cast<size_t>(x))
                                                 * channels;
            if (channels == 4) {
                out_px[0] = r;
                out_px[1] = g;
                out_px[2] = b;
                out_px[3] = a;
            } else {
                /* Straight source-over of the pixel onto the background: with a = 0
                 * the result is the background color exactly, with a = 255 the
                 * pixel. Uses the background's own alpha as the base opacity. */
                const unsigned sa = a;
                const unsigned da = bg_a;
                auto blend = [&](uint8_t src, uint8_t dst) {
                    const unsigned out_a = sa + da * (255u - sa) / 255u;
                    if (out_a == 0)
                        return static_cast<uint8_t>(0);
                    return static_cast<uint8_t>((static_cast<unsigned>(src) * sa
                                                 + static_cast<unsigned>(dst) * da * (255u - sa) / 255u)
                                                / out_a);
                };
                out_px[0] = blend(r, bg_r);
                out_px[1] = blend(g, bg_g);
                out_px[2] = blend(b, bg_b);
            }
        }
    }

    int written = 0;
    if (requested == "png") {
        written = stbi_write_png_to_func(&append_bytes, &out, width, height, channels, pixels.data(),
                                         width * channels);
    } else if (requested == "jpeg" || requested == "jpg") {
        const int q = quality > 0 ? (quality > 100 ? 100 : quality) : 92;
        written = stbi_write_jpg_to_func(&append_bytes, &out, width, height, channels, pixels.data(), q);
    } else if (requested == "bmp") {
        written = stbi_write_bmp_to_func(&append_bytes, &out, width, height, channels, pixels.data());
    } else if (requested == "tga") {
        written = stbi_write_tga_to_func(&append_bytes, &out, width, height, channels, pixels.data());
    } else if (requested == "hdr") {
        /* stb's HDR writer takes floats; not part of the tested baseline -- reject
         * it explicitly instead of writing something unexpected. */
        return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "the hdr encoder is not available");
    } else {
        return hv_fail(HELIOSVIEW_ERROR_UNSUPPORTED, "the image format cannot be written by the codec");
    }

    if (!written || out.empty())
        return hv_fail(HELIOSVIEW_ERROR_GENERIC, "encoding the image failed");
    return 0;
}

} // namespace hv::canvas
