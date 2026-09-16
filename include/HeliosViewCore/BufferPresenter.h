#pragma once

/**
 * HeliosView.Core -- BufferPresenter (Window Bit-Presentation middleware).
 *
 * BufferPresenter binds to a top-level window and presents arbitrary pixel views
 * (helios::PixelView) into the window's client area without any dependency on
 * drawing/rendering engines.
 *
 * It intercepts background erasing to guarantee flicker-free rendering and
 * automatically handles repaint events.
 */

#include <HeliosView/heliosview_presenter.h>
#include <HeliosViewCore/PixelView.h>
#include <HeliosViewCore/Window.h>

#include <functional>
#include <utility>

namespace helios {

class BufferPresenter {
public:
    BufferPresenter() = default;

    explicit BufferPresenter(Window& window)
        : BufferPresenter(window.nativeHandle())
    {
    }

    explicit BufferPresenter(heliosview_window_t* window)
        : m_handle(heliosview_buffer_presenter_create(window))
    {
        setupCallbacks();
    }

    ~BufferPresenter() {
        destroy();
    }

    BufferPresenter(const BufferPresenter&) = delete;
    BufferPresenter& operator=(const BufferPresenter&) = delete;

    BufferPresenter(BufferPresenter&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)),
          m_resizeCb(std::move(other.m_resizeCb)),
          m_paintCb(std::move(other.m_paintCb))
    {
        if (m_handle) {
            setupCallbacks();
        }
    }

    BufferPresenter& operator=(BufferPresenter&& other) noexcept {
        if (this != &other) {
            destroy();
            m_handle = std::exchange(other.m_handle, nullptr);
            m_resizeCb = std::move(other.m_resizeCb);
            m_paintCb = std::move(other.m_paintCb);
            if (m_handle) {
                setupCallbacks();
            }
        }
        return *this;
    }

    bool isValid() const noexcept { return m_handle != nullptr; }
    explicit operator bool() const noexcept { return isValid(); }

    bool present(const PixelView& view) {
        if (!m_handle) return false;
        return heliosview_buffer_presenter_present(m_handle, view.raw()) == 0;
    }

    bool present(const PixelView& view, int dstX, int dstY) {
        if (!m_handle) return false;
        return heliosview_buffer_presenter_present_rect(m_handle, view.raw(), dstX, dstY) == 0;
    }

    void invalidate() {
        if (m_handle) {
            heliosview_buffer_presenter_invalidate(m_handle);
        }
    }

    void onResize(std::function<void(int width, int height)> cb) {
        m_resizeCb = std::move(cb);
    }

    void onPaint(std::function<void()> cb) {
        m_paintCb = std::move(cb);
    }

    heliosview_buffer_presenter_t* raw() const noexcept { return m_handle; }

private:
    void destroy() {
        if (m_handle) {
            heliosview_buffer_presenter_destroy(m_handle);
            m_handle = nullptr;
        }
    }

    void setupCallbacks() {
        if (!m_handle) return;
        heliosview_buffer_presenter_set_resize_callback(
            m_handle,
            [](heliosview_buffer_presenter_t*, int32_t w, int32_t h, void* ud) {
                auto* self = static_cast<BufferPresenter*>(ud);
                if (self && self->m_resizeCb) {
                    self->m_resizeCb(w, h);
                }
            },
            this
        );

        heliosview_buffer_presenter_set_paint_callback(
            m_handle,
            [](heliosview_buffer_presenter_t*, void* ud) {
                auto* self = static_cast<BufferPresenter*>(ud);
                if (self && self->m_paintCb) {
                    self->m_paintCb();
                }
            },
            this
        );
    }

    heliosview_buffer_presenter_t* m_handle = nullptr;
    std::function<void(int, int)> m_resizeCb;
    std::function<void()> m_paintCb;
};

} // namespace helios
