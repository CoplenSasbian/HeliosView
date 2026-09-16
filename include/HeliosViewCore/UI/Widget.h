#ifndef HELIOSVIEW_CORE_UI_WIDGET_H
#define HELIOSVIEW_CORE_UI_WIDGET_H

/**
 * HeliosView.Core C++ UI Component System
 *
 * Modern header-only C++ wrapper around <HeliosView/heliosview_ui.h>.
 * Supports:
 *   1. OOP Derivation (override onPaint / onMouseEvent)
 *   2. Functional / Compositional Customization (CustomWidget with lambdas)
 *   3. Built-in modern widgets (Button, Label, VStack, HStack)
 */

#include <HeliosView/heliosview_ui.h>
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace HeliosView::UI {

class Widget : public std::enable_shared_from_this<Widget> {
public:
    Widget() {
        heliosview_ui_widget_desc_t desc{};
        desc.paint = StaticPaint;
        desc.event = StaticEvent;
        desc.destroy = StaticDestroy;

        m_handle = heliosview_ui_widget_create(&desc, this);
    }

    explicit Widget(heliosview_ui_widget_t* rawHandle) : m_handle(rawHandle), m_isBorrowed(true) {}

    virtual ~Widget() {
        if (m_handle && !m_isBorrowed) {
            heliosview_ui_widget_destroy(m_handle);
            m_handle = nullptr;
        }
    }

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    heliosview_ui_widget_t* handle() const noexcept { return m_handle; }

    // Geometry & Layout
    Widget* setBounds(int x, int y, int width, int height) {
        if (m_handle) heliosview_ui_widget_set_bounds(m_handle, x, y, width, height);
        return this;
    }

    Widget* setSize(int width, int height) {
        int x = 0, y = 0;
        if (m_handle) {
            heliosview_ui_widget_get_bounds(m_handle, &x, &y, nullptr, nullptr);
            heliosview_ui_widget_set_bounds(m_handle, x, y, width, height);
        }
        return this;
    }

    void requestRepaint() {
        if (m_handle) heliosview_ui_widget_request_repaint(m_handle);
    }

    // Children management
    Widget* addChild(std::shared_ptr<Widget> child) {
        if (child && m_handle) {
            heliosview_ui_widget_add_child(m_handle, child->handle());
            m_children.push_back(std::move(child));
        }
        return this;
    }

    // Customization Hooks for OOP subclasses
    virtual void onPaint(heliosview_painter_t* painter) {}
    virtual bool onMouseEvent(const heliosview_host_mouse_event_t* event) { return false; }

protected:
    heliosview_ui_widget_t* m_handle = nullptr;
    bool m_isBorrowed = false;
    std::vector<std::shared_ptr<Widget>> m_children;

private:
    static void StaticPaint(heliosview_ui_widget_t*, heliosview_painter_t* p, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        if (self) self->onPaint(p);
    }

    static int StaticEvent(heliosview_ui_widget_t*, const heliosview_host_mouse_event_t* e, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        return (self && self->onMouseEvent(e)) ? 1 : 0;
    }

    static void StaticDestroy(void*) {
        // C++ unique/shared ownership handles destruction
    }
};

/* ================= Compositional Custom Widget =================
 * Define any widget in-place using lambdas without creating a class.
 */
class CustomWidget : public Widget {
public:
    using PaintFn = std::function<void(CustomWidget* w, heliosview_painter_t* p)>;
    using EventFn = std::function<bool(CustomWidget* w, const heliosview_host_mouse_event_t* e)>;

    static std::shared_ptr<CustomWidget> create() {
        return std::make_shared<CustomWidget>();
    }

    CustomWidget* setPaint(PaintFn fn) {
        m_paintFn = std::move(fn);
        return this;
    }

    CustomWidget* setMouse(EventFn fn) {
        m_eventFn = std::move(fn);
        return this;
    }

    void onPaint(heliosview_painter_t* p) override {
        if (m_paintFn) m_paintFn(this, p);
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        if (m_eventFn) return m_eventFn(this, e);
        return false;
    }

private:
    PaintFn m_paintFn;
    EventFn m_eventFn;
};

/* ================= Built-in Label ================= */
class Label : public Widget {
public:
    static std::shared_ptr<Label> create(const std::string& text) {
        auto* raw = heliosview_ui_label_create(text.c_str());
        return std::shared_ptr<Label>(new Label(raw));
    }

    Label* setText(const std::string& text) {
        heliosview_ui_label_set_text(m_handle, text.c_str());
        return this;
    }

    Label* setColor(uint32_t argb) {
        heliosview_ui_label_set_color(m_handle, argb);
        return this;
    }

    Label* setFontSize(float size) {
        heliosview_ui_label_set_font_size(m_handle, size);
        return this;
    }

private:
    explicit Label(heliosview_ui_widget_t* raw) : Widget(raw) {
        m_isBorrowed = false;
    }
};

/* ================= Built-in Button ================= */
class Button : public Widget {
public:
    static std::shared_ptr<Button> create(const std::string& label) {
        auto btn = std::shared_ptr<Button>(new Button(label));
        return btn;
    }

    Button* setLabel(const std::string& label) {
        heliosview_ui_button_set_label(m_handle, label.c_str());
        return this;
    }

    Button* onClick(std::function<void()> cb) {
        m_onClick = std::move(cb);
        return this;
    }

private:
    explicit Button(const std::string& label) {
        m_handle = heliosview_ui_button_create(label.c_str(), StaticClick, this);
        m_isBorrowed = false;
    }

    static void StaticClick(heliosview_ui_widget_t*, void* udata) {
        auto* self = static_cast<Button*>(udata);
        if (self && self->m_onClick) self->m_onClick();
    }

    std::function<void()> m_onClick;
};

/* ================= Built-in Layout Containers ================= */
class VStack : public Widget {
public:
    static std::shared_ptr<VStack> create(int spacing = 10, int padding = 15) {
        auto* raw = heliosview_ui_vstack_create(spacing, padding);
        return std::shared_ptr<VStack>(new VStack(raw));
    }

    VStack* add(std::shared_ptr<Widget> child) {
        addChild(std::move(child));
        return this;
    }

private:
    explicit VStack(heliosview_ui_widget_t* raw) : Widget(raw) {
        m_isBorrowed = false;
    }
};

class HStack : public Widget {
public:
    static std::shared_ptr<HStack> create(int spacing = 10, int padding = 15) {
        auto* raw = heliosview_ui_hstack_create(spacing, padding);
        return std::shared_ptr<HStack>(new HStack(raw));
    }

    HStack* add(std::shared_ptr<Widget> child) {
        addChild(std::move(child));
        return this;
    }

private:
    explicit HStack(heliosview_ui_widget_t* raw) : Widget(raw) {
        m_isBorrowed = false;
    }
};

} // namespace HeliosView::UI

#endif /* HELIOSVIEW_CORE_UI_WIDGET_H */
