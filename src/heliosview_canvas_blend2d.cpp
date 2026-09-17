// HeliosView -- cross-platform canvas engine: Blend2D.
//
// This is the implementation of hv::canvas::Engine using Blend2D (https://blend2d.com):
// a modern, high-performance 2D vector graphics engine with JIT compilation.
// It provides identical, pixel-accurate rendering across all platforms (Windows, macOS,
// Linux) without depending on OS-specific 2D rendering APIs (GDI+, Direct2D, Cairo,
// Core Graphics).
//
// Threading: Blend2D contexts are thread-safe per session (one thread per painter).

#include "heliosview_canvas_internal.h"
#include <blend2d/blend2d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace hv::canvas {
namespace {

/* ================= Geometry helpers ================= */

inline Rect normalized_rect(const Rect& r)
{
    Rect out = r;
    if (out.w < 0.0f) {
        out.x += out.w;
        out.w = -out.w;
    }
    if (out.h < 0.0f) {
        out.y += out.h;
        out.h = -out.h;
    }
    return out;
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

inline Rect device_bounds(const Rect& r, const float m[6])
{
    const float xs[4] = {r.x, r.x + r.w, r.x + r.w, r.x};
    const float ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = xs[i] * m[0] + ys[i] * m[2] + m[4];
        const float y = ys[i] * m[3] + xs[i] * m[1] + m[5];
        if (i == 0) {
            left = right = x;
            top = bottom = y;
        } else {
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    return Rect{left, top, right - left, bottom - top};
}

inline BLStrokeCap blend2d_cap(heliosview_line_cap_t cap)
{
    switch (cap) {
    case HELIOSVIEW_CAP_ROUND: return BL_STROKE_CAP_ROUND;
    case HELIOSVIEW_CAP_SQUARE: return BL_STROKE_CAP_SQUARE;
    default: return BL_STROKE_CAP_BUTT;
    }
}

inline BLStrokeJoin blend2d_join(heliosview_line_join_t join)
{
    switch (join) {
    case HELIOSVIEW_JOIN_ROUND: return BL_STROKE_JOIN_ROUND;
    case HELIOSVIEW_JOIN_BEVEL: return BL_STROKE_JOIN_BEVEL;
    default: return BL_STROKE_JOIN_MITER_CLIP;
    }
}

static void build_bl_path(const PathData& path, BLPath& out)
{
    out.reset();
    for (const auto& e : path.entries) {
        switch (e.verb) {
        case PathVerbStartNew:
            out.move_to(e.pts[0], e.pts[1]);
            break;
        case PathVerbLineTo:
            out.line_to(e.pts[0], e.pts[1]);
            break;
        case PathVerbQuadTo:
            out.quad_to(e.pts[0], e.pts[1], e.pts[2], e.pts[3]);
            break;
        case PathVerbCubicTo:
            out.cubic_to(e.pts[0], e.pts[1], e.pts[2], e.pts[3], e.pts[4], e.pts[5]);
            break;
        case PathVerbClose:
            out.close();
            break;
        case PathVerbRect: {
            const Rect r = normalized_rect(Rect{e.pts[0], e.pts[1], e.pts[2], e.pts[3]});
            out.add_rect(BLRect(r.x, r.y, r.w, r.h));
            break;
        }
        case PathVerbRoundRect: {
            const Rect r = normalized_rect(Rect{e.pts[0], e.pts[1], e.pts[2], e.pts[3]});
            const float limit = (r.w < r.h ? r.w : r.h) * 0.5f;
            const float radius = std::clamp(e.pts[4], 0.0f, limit);
            out.add_round_rect(BLRoundRect(r.x, r.y, r.w, r.h, radius, radius));
            break;
        }
        case PathVerbEllipse: {
            const Rect r = normalized_rect(Rect{e.pts[0], e.pts[1], e.pts[2], e.pts[3]});
            const double cx = r.x + r.w * 0.5;
            const double cy = r.y + r.h * 0.5;
            const double rx = r.w * 0.5;
            const double ry = r.h * 0.5;
            out.add_ellipse(BLEllipse(cx, cy, rx, ry));
            break;
        }
        default:
            break;
        }
    }
}

/* ================= System font loader ================= */

struct CachedFace {
    BLFontFace face;
    std::vector<uint8_t> data;
};

#if defined(_WIN32)
static std::wstring utf8_to_wide(const std::string& str)
{
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wide[0], size);
    return wide;
}

static std::string find_windows_font_file(const std::string& family, uint32_t flags)
{
    auto iequals = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    };

    const bool bold = (flags & HELIOSVIEW_FONT_BOLD) != 0;
    const bool italic = (flags & HELIOSVIEW_FONT_ITALIC) != 0;

    if (iequals(family, "Segoe UI")) {
        if (bold && italic) return "C:\\Windows\\Fonts\\segoeuiz.ttf";
        if (bold) return "C:\\Windows\\Fonts\\segoeuib.ttf";
        if (italic) return "C:\\Windows\\Fonts\\segoeuii.ttf";
        return "C:\\Windows\\Fonts\\segoeui.ttf";
    }
    if (iequals(family, "Microsoft YaHei") || iequals(family, "Microsoft YaHei UI") ||
        family == "\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91" /* 微软雅黑 */) {
        return bold ? "C:\\Windows\\Fonts\\msyhbd.ttc" : "C:\\Windows\\Fonts\\msyh.ttc";
    }
    if (iequals(family, "SimSun") || iequals(family, "NSimSun") ||
        family == "\xe5\xae\x8b\xe4\xbd\x93" /* 宋体 */ || family == "\xe6\x96\xb0\xe5\xae\x8b\xe4\xbd\x93" /* 新宋体 */) {
        return "C:\\Windows\\Fonts\\simsun.ttc";
    }
    if (iequals(family, "SimHei") || family == "\xe9\xbb\x91\xe4\xbd\x93" /* 黑体 */) {
        return "C:\\Windows\\Fonts\\simhei.ttf";
    }
    if (iequals(family, "KaiTi") || family == "\xe6\xa5\xb7\xe4\xbd\x93" /* 楷体 */) {
        return "C:\\Windows\\Fonts\\simkai.ttf";
    }
    if (iequals(family, "FangSong") || family == "\xe4\xbb\xbf\xe5\xae\x8b" /* 仿宋 */) {
        return "C:\\Windows\\Fonts\\simfang.ttf";
    }
    if (iequals(family, "Arial")) {
        if (bold && italic) return "C:\\Windows\\Fonts\\arialbi.ttf";
        if (bold) return "C:\\Windows\\Fonts\\arialbd.ttf";
        if (italic) return "C:\\Windows\\Fonts\\ariali.ttf";
        return "C:\\Windows\\Fonts\\arial.ttf";
    }
    if (iequals(family, "Calibri")) {
        if (bold) return "C:\\Windows\\Fonts\\calibrib.ttf";
        if (italic) return "C:\\Windows\\Fonts\\calibrii.ttf";
        return "C:\\Windows\\Fonts\\calibri.ttf";
    }
    if (iequals(family, "Consolas")) {
        if (bold) return "C:\\Windows\\Fonts\\consolab.ttf";
        return "C:\\Windows\\Fonts\\consola.ttf";
    }
    if (iequals(family, "Tahoma")) {
        if (bold) return "C:\\Windows\\Fonts\\tahomabd.ttf";
        return "C:\\Windows\\Fonts\\tahoma.ttf";
    }

    static std::mutex s_reg_mutex;
    static std::unordered_map<std::string, std::string> s_reg_fonts;
    static bool s_reg_scanned = false;

    std::lock_guard<std::mutex> lock(s_reg_mutex);
    if (!s_reg_scanned) {
        s_reg_scanned = true;
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                          0, KEY_READ, &hkey) == ERROR_SUCCESS) {
            WCHAR val_name[512];
            BYTE val_data[1024];
            DWORD idx = 0;
            DWORD name_len = 512;
            DWORD data_len = 1024;
            DWORD type = 0;
            while (RegEnumValueW(hkey, idx++, val_name, &name_len, nullptr, &type, val_data, &data_len) == ERROR_SUCCESS) {
                if (type == REG_SZ && data_len > 0) {
                    int n_u8 = WideCharToMultiByte(CP_UTF8, 0, val_name, name_len, nullptr, 0, nullptr, nullptr);
                    std::string key_u8(n_u8, 0);
                    WideCharToMultiByte(CP_UTF8, 0, val_name, name_len, &key_u8[0], n_u8, nullptr, nullptr);
                    for (auto& c : key_u8) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

                    const auto* wdata = reinterpret_cast<const WCHAR*>(val_data);
                    int d_u8 = WideCharToMultiByte(CP_UTF8, 0, wdata, -1, nullptr, 0, nullptr, nullptr);
                    std::string file_u8(d_u8 > 1 ? d_u8 - 1 : 0, 0);
                    if (d_u8 > 1) {
                        WideCharToMultiByte(CP_UTF8, 0, wdata, -1, &file_u8[0], d_u8, nullptr, nullptr);
                    }
                    if (!file_u8.empty()) {
                        if (file_u8.find(":\\") == std::string::npos && file_u8.find(":/") == std::string::npos) {
                            file_u8 = "C:\\Windows\\Fonts\\" + file_u8;
                        }
                        s_reg_fonts[key_u8] = file_u8;
                    }
                }
                name_len = 512;
                data_len = 1024;
            }
            RegCloseKey(hkey);
        }
    }

    std::string lower_target = family;
    for (auto& c : lower_target) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (const auto& [reg_name, path] : s_reg_fonts) {
        if (reg_name.find(lower_target) != std::string::npos) {
            return path;
        }
    }
    return "";
}
#endif

static BLFont get_system_font(const char* family, float size, uint32_t flags)
{
    static std::mutex s_font_cache_mutex;
    static std::unordered_map<std::string, std::shared_ptr<CachedFace>> s_font_cache;
    const std::string name = (family && *family) ? family : "Segoe UI";
    const std::string key = name + "#" + std::to_string(flags);

    {
        std::lock_guard<std::mutex> lock(s_font_cache_mutex);
        auto it = s_font_cache.find(key);
        if (it != s_font_cache.end()) {
            BLFont font;
            font.create_from_face(it->second->face, size > 0.0f ? size : 12.0f);
            return font;
        }
    }

#if defined(_WIN32)
    const std::string file_path = find_windows_font_file(name, flags);
    if (!file_path.empty()) {
        BLFontFace face;
        if (face.create_from_file(file_path.c_str()) == BL_SUCCESS) {
            auto cached = std::make_shared<CachedFace>();
            cached->face = face;
            {
                std::lock_guard<std::mutex> lock(s_font_cache_mutex);
                s_font_cache[key] = cached;
            }
            BLFont font;
            font.create_from_face(face, size > 0.0f ? size : 12.0f);
            return font;
        }
    }

    // Try GDI font data query
    const std::wstring wide_name = utf8_to_wide(name);
    HDC hdc = CreateCompatibleDC(nullptr);
    HFONT hfont = CreateFontW(
        -static_cast<int>(size > 0.0f ? size : 12.0f), 0, 0, 0,
        (flags & HELIOSVIEW_FONT_BOLD) ? FW_BOLD : FW_NORMAL,
        (flags & HELIOSVIEW_FONT_ITALIC) ? TRUE : FALSE,
        (flags & HELIOSVIEW_FONT_UNDERLINE) ? TRUE : FALSE,
        0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        wide_name.c_str()
    );

    BLFont font;
    if (hfont) {
        HGDIOBJ old_font = SelectObject(hdc, hfont);
        DWORD bytes_count = GetFontData(hdc, 0, 0, nullptr, 0);
        if (bytes_count != GDI_ERROR && bytes_count > 0) {
            auto cached = std::make_shared<CachedFace>();
            cached->data.resize(bytes_count);
            GetFontData(hdc, 0, 0, cached->data.data(), bytes_count);
            BLFontData font_data;
            if (font_data.create_from_data(cached->data.data(), cached->data.size()) == BL_SUCCESS) {
                if (cached->face.create_from_data(font_data, 0) == BL_SUCCESS) {
                    {
                        std::lock_guard<std::mutex> lock(s_font_cache_mutex);
                        s_font_cache[key] = cached;
                    }
                    font.create_from_face(cached->face, size > 0.0f ? size : 12.0f);
                }
            }
        }
        SelectObject(hdc, old_font);
        DeleteObject(hfont);
    }
    DeleteDC(hdc);
    if (font.is_valid())
        return font;
#endif

    // Fallback: try common font files
    static const char* kFallbackPaths[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/SFCompact.ttf",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };

    for (const char* path : kFallbackPaths) {
        BLFontFace face;
        if (face.create_from_file(path) == BL_SUCCESS) {
            auto cached = std::make_shared<CachedFace>();
            cached->face = face;
            s_font_cache[key] = cached;
            BLFont fallback_font;
            fallback_font.create_from_face(face, size > 0.0f ? size : 12.0f);
            return fallback_font;
        }
    }

    return BLFont();
}

static BLFont get_cjk_fallback_font(float size, uint32_t flags)
{
    static std::mutex s_cjk_mutex;
    static std::shared_ptr<CachedFace> s_cjk_face;
    static std::shared_ptr<CachedFace> s_cjk_face_bold;

    const bool bold = (flags & HELIOSVIEW_FONT_BOLD) != 0;
    std::shared_ptr<CachedFace>& target_face = bold ? s_cjk_face_bold : s_cjk_face;

    {
        std::lock_guard<std::mutex> lock(s_cjk_mutex);
        if (target_face && target_face->face.is_valid()) {
            BLFont font;
            font.create_from_face(target_face->face, size > 0.0f ? size : 12.0f);
            return font;
        }
    }

    static const char* kCjkFontPaths[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\msjh.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/Library/Fonts/Songti.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
#endif
    };

    std::lock_guard<std::mutex> lock(s_cjk_mutex);
    if (!target_face) {
        target_face = std::make_shared<CachedFace>();
        size_t start_idx = (bold ? 0 : 1);
        for (size_t i = start_idx; i < sizeof(kCjkFontPaths) / sizeof(kCjkFontPaths[0]); ++i) {
            if (target_face->face.create_from_file(kCjkFontPaths[i]) == BL_SUCCESS) {
                break;
            }
        }
        if (!target_face->face.is_valid()) {
            target_face->face.create_from_file(kCjkFontPaths[0]);
        }
    }

    if (target_face && target_face->face.is_valid()) {
        BLFont font;
        font.create_from_face(target_face->face, size > 0.0f ? size : 12.0f);
        return font;
    }
    return BLFont();
}

struct TextRun {
    BLFont font;
    std::unique_ptr<BLGlyphBuffer> gb;
    float advance_x = 0.0f;
};

struct TextLayoutResult {
    std::vector<TextRun> runs;
    float total_width = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_height = 0.0f;
};

static TextLayoutResult layout_text(const char* utf8, const BLFont& primary_font, float size, uint32_t flags)
{
    TextLayoutResult result{};
    if (!utf8 || !*utf8 || !primary_font.is_valid())
        return result;

    BLFontMetrics primary_fm = primary_font.metrics();
    result.ascent = primary_fm.ascent;
    result.descent = primary_fm.descent;
    result.line_height = primary_fm.ascent + primary_fm.descent + primary_fm.line_gap;

    auto initial_gb = std::make_unique<BLGlyphBuffer>();
    initial_gb->set_text(utf8, std::strlen(utf8), BL_TEXT_ENCODING_UTF8);
    const size_t char_count = initial_gb->size();
    if (char_count == 0)
        return result;

    std::vector<uint32_t> ucs4(char_count);
    std::memcpy(ucs4.data(), initial_gb->content(), char_count * sizeof(uint32_t));

    BLGlyphMappingState state;
    primary_font.map_text_to_glyphs(*initial_gb, state);

    BLFont fallback_font;
    if (state.undefined_count > 0) {
        fallback_font = get_cjk_fallback_font(size, flags);
        if (fallback_font.is_valid()) {
            BLFontMetrics fb_fm = fallback_font.metrics();
            if (fb_fm.ascent > result.ascent) result.ascent = fb_fm.ascent;
            if (fb_fm.descent > result.descent) result.descent = fb_fm.descent;
            const float fb_lh = fb_fm.ascent + fb_fm.descent + fb_fm.line_gap;
            if (fb_lh > result.line_height) result.line_height = fb_lh;
        }
    }

    if (state.undefined_count == 0 || !fallback_font.is_valid()) {
        primary_font.position_glyphs(*initial_gb);
        BLTextMetrics tm;
        primary_font.get_text_metrics(*initial_gb, tm);
        TextRun run;
        run.font = primary_font;
        run.advance_x = static_cast<float>(tm.advance.x);
        run.gb = std::move(initial_gb);
        result.total_width = run.advance_x;
        result.runs.push_back(std::move(run));
        return result;
    }

    size_t i = 0;
    while (i < char_count) {
        bool use_fallback = (initial_gb->content()[i] == 0);
        size_t start = i;
        while (i < char_count && (initial_gb->content()[i] == 0) == use_fallback) {
            ++i;
        }
        size_t count = i - start;

        TextRun run;
        run.font = use_fallback ? fallback_font : primary_font;
        run.gb = std::make_unique<BLGlyphBuffer>();
        run.gb->set_text(ucs4.data() + start, count, BL_TEXT_ENCODING_UTF32);
        run.font.shape(*run.gb);
        BLTextMetrics tm;
        run.font.get_text_metrics(*run.gb, tm);
        run.advance_x = static_cast<float>(tm.advance.x);
        result.total_width += run.advance_x;
        result.runs.push_back(std::move(run));
    }

    return result;
}


/* ================= Canvas adapter ================= */

class CanvasAdapterImpl final : public CanvasAdapter {
public:
    CanvasAdapterImpl(const CanvasData& data)
        : m_width(data.width), m_height(data.height), m_stride(data.stride),
          m_format(data.format), m_pixels(data.pixels)
    {
        wrap_or_convert();
    }

    bool direct_pixels() const override { return m_format == HELIOSVIEW_FORMAT_BGRA8_PREMUL; }
    void* native_bitmap() override { return &m_image; }
    BLImage& image() { return m_image; }

    void reload() override
    {
        wrap_or_convert();
    }

    void sync_to_buffer()
    {
        // Read back pixels if image buffer was detached/converted from canvas buffer
        BLImageData img_data;
        if (m_image.get_data(&img_data) != BL_SUCCESS)
            return;
        if (img_data.pixel_data != m_pixels) {
            const CanvasData src{m_width, m_height, static_cast<int32_t>(img_data.stride),
                                 HELIOSVIEW_FORMAT_BGRA8_PREMUL, static_cast<uint8_t*>(img_data.pixel_data)};
            const CanvasData dst{m_width, m_height, m_stride, m_format, m_pixels};
            convert_copy(src, Rect{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)},
                         dst, 0, 0, 1.0f);
        }
    }


private:
    void wrap_or_convert()
    {
        if (m_format == HELIOSVIEW_FORMAT_BGRA8_PREMUL) {
            m_image.create_from_data(m_width, m_height, BL_FORMAT_PRGB32, m_pixels, m_stride);
        } else {
            m_image.create(m_width, m_height, BL_FORMAT_PRGB32);
            BLImageData img_data;
            if (m_image.get_data(&img_data) == BL_SUCCESS) {
                const CanvasData src{m_width, m_height, m_stride, m_format, m_pixels};
                const CanvasData dst{m_width, m_height, static_cast<int32_t>(img_data.stride),
                                     HELIOSVIEW_FORMAT_BGRA8_PREMUL, static_cast<uint8_t*>(img_data.pixel_data)};
                std::memset(img_data.pixel_data, 0, img_data.stride * static_cast<size_t>(m_height));
                convert_copy(src, Rect{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)},
                             dst, 0, 0, 1.0f);
            }
        }
    }

    int32_t m_width;
    int32_t m_height;
    int32_t m_stride;
    heliosview_pixel_format_t m_format;
    uint8_t* m_pixels;
    BLImage m_image;
};

/* ================= Context ================= */

class ContextImpl final : public Context {
public:
    ContextImpl(CanvasAdapterImpl& adapter, const CanvasData& data)
        : m_adapter(adapter), m_data(data)
    {
        m_ctx.begin(m_adapter.image());
        m_clip_bounds = canvas_bounds();
    }

    ~ContextImpl() override
    {
        m_ctx.end();
        m_adapter.sync_to_buffer();
    }

    /* ---- state ---- */
    void set_state(const heliosview_painter_state_t& state) override
    {
        m_state = state;
        m_state_dirty = true;
    }

    void set_transform(const float m[6]) override
    {
        std::memcpy(m_transform, m, sizeof(m_transform));
        m_matrix = BLMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5]);
        m_ctx.set_transform(m_matrix);
    }

    /* ---- clipping ---- */
    void set_clip_rect(const Rect& r) override
    {
        const Rect rect = normalized_rect(r);
        m_has_clip_path = false;
        m_ctx.restore_clipping();
        m_ctx.clip_to_rect(BLRect(rect.x, rect.y, rect.w, rect.h));
        m_clip_bounds = intersect_rect(canvas_bounds(), device_bounds(rect, m_transform));
    }

    void intersect_clip_rect(const Rect& r) override
    {
        const Rect rect = normalized_rect(r);
        m_ctx.clip_to_rect(BLRect(rect.x, rect.y, rect.w, rect.h));
        m_clip_bounds = intersect_rect(m_clip_bounds, device_bounds(rect, m_transform));

        if (m_has_clip_path) {
            BLImage rect_mask;
            rect_mask.create(m_data.width, m_data.height, BL_FORMAT_A8);
            BLContext rm_ctx(rect_mask);
            rm_ctx.clear_all();
            rm_ctx.set_transform(m_matrix);
            rm_ctx.fill_rect(BLRect(rect.x, rect.y, rect.w, rect.h), BLRgba32(0xFF, 0xFF, 0xFF, 0xFF));
            rm_ctx.end();

            BLImageData mask_data;
            BLImageData rect_data;
            if (m_clip_mask.get_data(&mask_data) == BL_SUCCESS && rect_mask.get_data(&rect_data) == BL_SUCCESS) {
                for (int32_t y = 0; y < m_data.height; ++y) {
                    uint8_t* m_row = static_cast<uint8_t*>(mask_data.pixel_data) + static_cast<size_t>(y) * mask_data.stride;
                    const uint8_t* r_row = static_cast<const uint8_t*>(rect_data.pixel_data) + static_cast<size_t>(y) * rect_data.stride;
                    for (int32_t x = 0; x < m_data.width; ++x) {
                        m_row[x] = static_cast<uint8_t>((static_cast<uint32_t>(m_row[x]) * r_row[x] + 127) / 255);
                    }
                }
            }
        }
    }

    void set_clip_path(const PathData& path) override
    {
        m_has_clip_path = false;
        if (path.empty())
            return;

        BLPath bl_path;
        build_bl_path(path, bl_path);
        BLBox box;
        bl_path.get_bounding_box(&box);

        m_clip_mask.create(m_data.width, m_data.height, BL_FORMAT_A8);
        BLContext mask_ctx(m_clip_mask);
        mask_ctx.clear_all();
        mask_ctx.set_transform(m_matrix);
        mask_ctx.set_fill_rule(path.winding == HELIOSVIEW_WINDING_EVENODD ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO);
        mask_ctx.fill_path(bl_path, BLRgba32(0xFF, 0xFF, 0xFF, 0xFF));
        mask_ctx.end();

        const Rect local_box{
            static_cast<float>(box.x0), static_cast<float>(box.y0),
            static_cast<float>(box.x1 - box.x0), static_cast<float>(box.y1 - box.y0)
        };
        m_clip_bounds = intersect_rect(canvas_bounds(), device_bounds(local_box, m_transform));
        m_has_clip_path = true;
    }

    void reset_clip() override
    {
        m_has_clip_path = false;
        m_ctx.restore_clipping();
        m_clip_bounds = canvas_bounds();
    }

    void clip_bounds(Rect& out) const override { out = m_clip_bounds; }

    /* ---- state stack ---- */
    struct SavedState {
        bool has_clip_path;
        BLImage clip_mask;
        Rect clip_bounds;
    };

    void save() override
    {
        m_ctx.save();
        m_saved_states.push_back(SavedState{m_has_clip_path, m_clip_mask, m_clip_bounds});
    }

    void restore() override
    {
        m_ctx.restore();
        if (!m_saved_states.empty()) {
            const auto& s = m_saved_states.back();
            m_has_clip_path = s.has_clip_path;
            m_clip_mask = s.clip_mask;
            m_clip_bounds = s.clip_bounds;
            m_saved_states.pop_back();
        }
    }

    /* ---- shapes ---- */
    void clear(uint32_t argb) override
    {
        // clear() ignores transform, clip, and painter state
        m_ctx.flush(BL_CONTEXT_FLUSH_SYNC);
        BLImageData img_data;
        if (m_adapter.image().get_data(&img_data) == BL_SUCCESS) {
            const uint32_t a = (argb >> 24) & 0xFF;
            const uint32_t r = ((argb >> 16) & 0xFF) * a / 255;
            const uint32_t g = ((argb >> 8) & 0xFF) * a / 255;
            const uint32_t b = (argb & 0xFF) * a / 255;
            const uint32_t word = (a << 24) | (r << 16) | (g << 8) | b;
            for (int32_t y = 0; y < m_data.height; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(img_data.pixel_data) + static_cast<size_t>(y) * img_data.stride);
                for (int32_t x = 0; x < m_data.width; ++x) {
                    row[x] = word;
                }
            }
        }
        m_adapter.sync_to_buffer();
    }

    void draw_line(float x1, float y1, float x2, float y2) override
    {
        apply_draw([&](BLContext& ctx) {
            if (has_stroke())
                ctx.stroke_line(x1, y1, x2, y2);
        });
    }

    void draw_rect(const Rect& r) override
    {
        const Rect rect = normalized_rect(r);
        const BLRect bl_rect(rect.x, rect.y, rect.w, rect.h);
        apply_draw([&](BLContext& ctx) {
            if (has_fill())
                ctx.fill_rect(bl_rect);
            if (has_stroke())
                ctx.stroke_rect(bl_rect);
        });
    }

    void draw_round_rect(const Rect& r, float radius) override
    {
        const Rect rect = normalized_rect(r);
        const float limit = (rect.w < rect.h ? rect.w : rect.h) * 0.5f;
        const float rad = std::clamp(radius, 0.0f, limit);
        const BLRoundRect bl_round_rect(rect.x, rect.y, rect.w, rect.h, rad, rad);
        apply_draw([&](BLContext& ctx) {
            if (has_fill())
                ctx.fill_round_rect(bl_round_rect);
            if (has_stroke())
                ctx.stroke_round_rect(bl_round_rect);
        });
    }

    void draw_ellipse(const Rect& r) override
    {
        const Rect rect = normalized_rect(r);
        const double cx = rect.x + rect.w * 0.5;
        const double cy = rect.y + rect.h * 0.5;
        const double rx = rect.w * 0.5;
        const double ry = rect.h * 0.5;
        const BLEllipse bl_ellipse(cx, cy, rx, ry);
        apply_draw([&](BLContext& ctx) {
            if (has_fill())
                ctx.fill_ellipse(bl_ellipse);
            if (has_stroke())
                ctx.stroke_ellipse(bl_ellipse);
        });
    }

    void draw_arc(const Rect& r, float start_deg, float sweep_deg) override
    {
        const Rect rect = normalized_rect(r);
        const double cx = rect.x + rect.w * 0.5;
        const double cy = rect.y + rect.h * 0.5;
        const double rx = rect.w * 0.5;
        const double ry = rect.h * 0.5;
        const double start_rad = start_deg * (M_PI / 180.0);
        const double sweep_rad = sweep_deg * (M_PI / 180.0);
        const BLArc bl_arc(cx, cy, rx, ry, start_rad, sweep_rad);
        apply_draw([&](BLContext& ctx) {
            if (has_stroke())
                ctx.stroke_arc(bl_arc);
        });
    }

    void draw_polyline(const float* points, size_t count, bool closed) override
    {
        if (!points || count < 2)
            return;
        BLPath path;
        for (size_t i = 0; i < count; ++i) {
            if (i == 0)
                path.move_to(points[0], points[1]);
            else
                path.line_to(points[i * 2], points[i * 2 + 1]);
        }
        if (closed)
            path.close();

        apply_draw([&](BLContext& ctx) {
            if (closed && has_fill())
                ctx.fill_path(path);
            if (has_stroke())
                ctx.stroke_path(path);
        });
    }

    void draw_path(const PathData& path, bool fill, bool stroke) override
    {
        if (path.empty() || (!fill && !stroke))
            return;
        BLPath bl_path;
        build_bl_path(path, bl_path);
        apply_draw([&](BLContext& ctx) {
            if (fill && has_fill()) {
                ctx.set_fill_rule(path.winding == HELIOSVIEW_WINDING_EVENODD ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO);
                ctx.fill_path(bl_path);
            }
            if (stroke && has_stroke())
                ctx.stroke_path(bl_path);
        });
    }

    /* ---- text ---- */
    void draw_text(const char* utf8, const Rect& box, uint32_t align) override
    {
        if (!utf8 || !*utf8)
            return;

        BLFont font = current_font();
        if (!font.is_valid())
            return;

        TextLayoutResult layout = layout_text(utf8, font, m_state.font.size, m_state.font.flags);
        if (layout.runs.empty())
            return;

        const float text_w = layout.total_width;
        const float text_h = layout.ascent + layout.descent;

        float x = box.x;
        float y = box.y;

        const bool has_box = (box.w > 0.0f && box.h > 0.0f);
        const bool baselined = (align & HELIOSVIEW_ALIGN_BASELINE) != 0;

        if (has_box) {
            if (align & HELIOSVIEW_ALIGN_HCENTER)
                x += (box.w - text_w) * 0.5f;
            else if (align & HELIOSVIEW_ALIGN_RIGHT)
                x += (box.w - text_w);

            if (align & HELIOSVIEW_ALIGN_VCENTER)
                y += (box.h - text_h) * 0.5f;
            else if (align & HELIOSVIEW_ALIGN_BOTTOM)
                y += (box.h - text_h);
        }

        const float baseline_y = baselined ? y : (y + layout.ascent);

        float cur_x = x;
        apply_draw([&](BLContext& ctx) {
            for (auto& run : layout.runs) {
                const BLPoint origin(cur_x, baseline_y);
                ctx.fill_glyph_run(origin, run.font, run.gb->glyph_run());
                cur_x += run.advance_x;
            }
        });
    }

    void measure_text(const char* utf8, heliosview_text_metrics_t* out) override
    {
        if (!out)
            return;
        *out = heliosview_text_metrics_t{};

        BLFont font = current_font();
        if (!font.is_valid())
            return;

        BLFontMetrics fm = font.metrics();
        out->ascent = fm.ascent;
        out->descent = fm.descent;
        out->line_height = fm.ascent + fm.descent + fm.line_gap;

        if (utf8 && *utf8) {
            TextLayoutResult layout = layout_text(utf8, font, m_state.font.size, m_state.font.flags);
            out->width = layout.total_width;
            out->ascent = layout.ascent;
            out->descent = layout.descent;
            out->height = layout.ascent + layout.descent;
            out->line_height = layout.line_height;
        }
        if (out->height <= 0.0f)
            out->height = out->ascent + out->descent;
        if (out->line_height <= 0.0f)
            out->line_height = out->height;
    }

    /* ---- images ---- */
    void draw_image(CanvasAdapter* src_adapter, const CanvasData& src, const Rect& src_rect,
                    const Rect& dst_rect, float alpha) override
    {
        if (!src.pixels || dst_rect.w <= 0.0f || dst_rect.h <= 0.0f)
            return;

        BLImage src_img;
        auto* own = dynamic_cast<CanvasAdapterImpl*>(src_adapter);
        if (own && own->native_bitmap()) {
            src_img = *static_cast<BLImage*>(own->native_bitmap());
        } else if (src.format == HELIOSVIEW_FORMAT_BGRA8_PREMUL) {
            src_img.create_from_data(src.width, src.height, BL_FORMAT_PRGB32, src.pixels, src.stride);
        } else {
            src_img.create(src.width, src.height, BL_FORMAT_PRGB32);
            BLImageData img_data;
            if (src_img.get_data(&img_data) == BL_SUCCESS) {
                const CanvasData dst{src.width, src.height, static_cast<int32_t>(img_data.stride),
                                     HELIOSVIEW_FORMAT_BGRA8_PREMUL, static_cast<uint8_t*>(img_data.pixel_data)};
                std::memset(img_data.pixel_data, 0, img_data.stride * static_cast<size_t>(src.height));
                convert_copy(src, Rect{0.0f, 0.0f, static_cast<float>(src.width), static_cast<float>(src.height)},
                             dst, 0, 0, 1.0f);
            }
        }

        const BLRectI src_area(static_cast<int>(src_rect.x), static_cast<int>(src_rect.y),
                               static_cast<int>(src_rect.w), static_cast<int>(src_rect.h));
        const BLRect dst_area(dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h);

        apply_draw([&](BLContext& ctx) {
            ctx.save();
            ctx.set_global_alpha(m_state.alpha * std::clamp(alpha, 0.0f, 1.0f));
            ctx.blit_image(dst_area, src_img, src_area);
            ctx.restore();
        });
    }

    void flush() override
    {
        m_ctx.flush(BL_CONTEXT_FLUSH_SYNC);
        m_adapter.sync_to_buffer();
    }

private:
    Rect canvas_bounds() const
    {
        return Rect{0.0f, 0.0f, static_cast<float>(m_data.width), static_cast<float>(m_data.height)};
    }

    bool has_fill() const { return ((m_state.fill_color >> 24) & 0xFF) != 0; }
    bool has_stroke() const { return ((m_state.stroke_color >> 24) & 0xFF) != 0; }

    void apply_state(BLContext& ctx)
    {
        ctx.set_global_alpha(m_state.alpha);
        if (has_stroke()) {
            ctx.set_stroke_style(BLRgba32(m_state.stroke_color));
            ctx.set_stroke_width(m_state.stroke_width > 0.0f ? m_state.stroke_width : 1.0f);
            ctx.set_stroke_caps(blend2d_cap(m_state.line_cap));
            ctx.set_stroke_join(blend2d_join(m_state.line_join));
        }
        if (has_fill()) {
            ctx.set_fill_style(BLRgba32(m_state.fill_color));
        }
    }

    BLFont current_font()
    {
        return get_system_font(m_state.font.family, m_state.font.size, m_state.font.flags);
    }

    void ensure_scratch()
    {
        if (m_scratch_image.is_empty() || m_scratch_image.width() != m_data.width || m_scratch_image.height() != m_data.height) {
            m_scratch_image.create(m_data.width, m_data.height, BL_FORMAT_PRGB32);
        }
        if (!m_scratch_ctx.is_valid()) {
            m_scratch_ctx.begin(m_scratch_image);
        }
    }

    template <typename F>
    void apply_draw(F&& func)
    {
        if (m_has_clip_path) {
            ensure_scratch();
            m_scratch_ctx.clear_all();
            m_scratch_ctx.set_transform(m_matrix);
            apply_state(m_scratch_ctx);
            func(m_scratch_ctx);
            m_scratch_ctx.flush(BL_CONTEXT_FLUSH_SYNC);

            m_ctx.save();
            m_ctx.reset_transform();
            m_ctx.fill_mask(BLPointI(0, 0), m_clip_mask, BLPattern(m_scratch_image));
            m_ctx.restore();
        } else {
            apply_state(m_ctx);
            func(m_ctx);
        }
    }

    CanvasAdapterImpl& m_adapter;
    const CanvasData& m_data;
    BLContext m_ctx;
    heliosview_painter_state_t m_state{};
    bool m_state_dirty = true;
    float m_transform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    BLMatrix2D m_matrix{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};

    // Clipping
    Rect m_clip_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    bool m_has_clip_path = false;
    BLImage m_clip_mask;

    // Scratch canvas for path-clipped rendering
    BLImage m_scratch_image;
    BLContext m_scratch_ctx;

    std::vector<SavedState> m_saved_states;
};

/* ================= Engine ================= */

class Blend2dEngine final : public Engine {
public:
    heliosview_canvas_engine_t id() const override { return HELIOSVIEW_ENGINE_BLEND2D; }
    const char* name() const override { return "blend2d"; }
    bool probe() override { return true; }
    heliosview_pixel_format_t preferred_format() const override
    {
        return HELIOSVIEW_FORMAT_BGRA8_PREMUL;
    }

    bool supports_format(heliosview_pixel_format_t format) const override
    {
        return format == HELIOSVIEW_FORMAT_BGRA8_PREMUL ||
               format == HELIOSVIEW_FORMAT_BGRA8 ||
               format == HELIOSVIEW_FORMAT_RGBA8 ||
               format == HELIOSVIEW_FORMAT_GRAY8;
    }

    bool supports_feature(int feature) const override
    {
        // Blend2D supports antialiasing, alpha blending, arbitrary affine transforms,
        // path clipping, winding rules, text, text AA, image drawing, and direct memory pixels.
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
        return std::make_unique<CanvasAdapterImpl>(data);
    }

    std::unique_ptr<Context> create_context(CanvasAdapter& canvas, const CanvasData& data) override
    {
        return std::make_unique<ContextImpl>(static_cast<CanvasAdapterImpl&>(canvas), data);
    }
};

Blend2dEngine g_engine;

const heliosview_canvas_engine_t g_blend2d_names[] = {
    HELIOSVIEW_ENGINE_BLEND2D,
    HELIOSVIEW_ENGINE_BUILTIN
};

struct Blend2dRegistration {
    Blend2dRegistration() { register_engine(&g_engine, g_blend2d_names, 2); }
};

const Blend2dRegistration g_registration;

} // namespace
} // namespace hv::canvas
