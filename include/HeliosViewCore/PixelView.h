#pragma once

/**
 * HeliosView.Core -- PixelView (non-owning rectangular pixel slice).
 *
 * A lightweight, zero-copy, zero-dependency view into a block of 2D pixel memory.
 * Decoupled from any drawing engine and any windowing system.
 */

#include <HeliosView/heliosview_base.h>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace helios {

enum class PixelFormat : int32_t {
    Auto = HELIOSVIEW_FORMAT_AUTO,
    Bgra8Premul = HELIOSVIEW_FORMAT_BGRA8_PREMUL,
    Bgra8 = HELIOSVIEW_FORMAT_BGRA8,
    Rgba8 = HELIOSVIEW_FORMAT_RGBA8,
    Gray8 = HELIOSVIEW_FORMAT_GRAY8,
};


constexpr inline int bytesPerPixel(PixelFormat fmt) noexcept {
    switch (fmt) {
    case PixelFormat::Gray8: return 1;
    case PixelFormat::Bgra8Premul:
    case PixelFormat::Bgra8:
    case PixelFormat::Rgba8: return 4;
    default: return 4;
    }
}

class PixelView {
public:
    constexpr PixelView() noexcept
        : m_view{nullptr, 0, 0, 0, HELIOSVIEW_FORMAT_AUTO} {}

    constexpr PixelView(const void* pixels, int width, int height, int stride,
                        PixelFormat format = PixelFormat::Bgra8Premul) noexcept
        : m_view{pixels, width, height, stride, static_cast<heliosview_pixel_format_t>(format)} {}

    constexpr PixelView(const heliosview_pixel_view_t& raw) noexcept
        : m_view(raw) {}

    constexpr const void* data() const noexcept { return m_view.pixels; }
    constexpr const uint8_t* bytes() const noexcept { return static_cast<const uint8_t*>(m_view.pixels); }
    constexpr int width() const noexcept { return m_view.width; }
    constexpr int height() const noexcept { return m_view.height; }
    constexpr int stride() const noexcept { return m_view.stride; }
    constexpr PixelFormat format() const noexcept { return static_cast<PixelFormat>(m_view.format); }

    constexpr bool empty() const noexcept {
        return !m_view.pixels || m_view.width <= 0 || m_view.height <= 0;
    }

    constexpr explicit operator bool() const noexcept { return !empty(); }

    constexpr int bytesPerPixel() const noexcept {
        return helios::bytesPerPixel(format());
    }

    // Direct row pointer
    const uint8_t* row(int y) const noexcept {
        if (empty() || y < 0 || y >= m_view.height) return nullptr;
        return bytes() + static_cast<size_t>(y) * m_view.stride;
    }

    // Subview slicing without copying pixels
    PixelView subview(int x, int y, int w, int h) const noexcept {
        if (empty() || x < 0 || y < 0 || w <= 0 || h <= 0 ||
            x + w > m_view.width || y + h > m_view.height) {
            return PixelView();
        }
        const uint8_t* subPtr = row(y) + static_cast<size_t>(x) * bytesPerPixel();
        return PixelView(subPtr, w, h, m_view.stride, format());
    }

    constexpr const heliosview_pixel_view_t* raw() const noexcept { return &m_view; }
    constexpr operator const heliosview_pixel_view_t*() const noexcept { return &m_view; }
    constexpr operator heliosview_pixel_view_t() const noexcept { return m_view; }

private:
    heliosview_pixel_view_t m_view{};
};

} // namespace helios
