#ifndef HELIOSVIEW_CORE_UI_WIDGET_H
#define HELIOSVIEW_CORE_UI_WIDGET_H

/**
 * HeliosView.Core C++ UI Component System
 *
 * Modern header-only C++ wrapper around <HeliosView/heliosview_ui.h>.
 * Supports:
 *   1. OOP Derivation (override onPaint / onMouseEvent)
 *   2. Functional / Compositional Customization (CustomWidget with lambdas)
 *   3. Built-in modern widgets (Button, Label, VStack, HStack, Slider, ...)
 *
 * Drawing goes through the C++ painter (<HeliosViewCore/Canvas.h>), never through
 * the C painter functions: a widget callback is handed a live painter session by
 * the host, and wraps it as a borrowing helios::Painter (see Painter(const Handle&)
 * in Canvas.h) whose destructor leaves the session to its owner:
 *
 *     widget->setPaint([](CustomWidget* w, helios::Painter& p) {
 *         const helios::Rect r = w->bounds();
 *         p.setFill(0xFF1E1E2E);
 *         p.drawRoundRect(0, 0, (float)r.width, (float)r.height, 10.0f);
 *     });
 *
 * On the C side every widget handle is heliosview_ui_widget_t*; here it is a Widget
 * (or one of its subclasses), and the raw handle is only reachable through handle()
 * for the calls this wrapper does not cover.
 */

#include <HeliosView/heliosview_canvas.h>
#include <HeliosView/heliosview_ui.h>

/* Declared before <HeliosViewCore/Canvas.h>, which names this class as a friend so a
 * widget can adopt the live painter session its host owns. */
namespace HeliosView::UI {
class Widget;
}

#include <HeliosViewCore/Canvas.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HeliosView::UI {

class Widget : public std::enable_shared_from_this<Widget> {
public:
    Widget() {
        heliosview_ui_widget_desc_t desc{};
        desc.paint = StaticPaint;
        desc.event = StaticEvent;
        desc.destroy = StaticDestroy;

        m_handle = heliosview_ui_widget_create(&desc, this);

        /* Keyboard input is installed through setters rather than the descriptor, so
         * the public heliosview_ui_widget_desc_t keeps its layout. Having them is what
         * makes the widget focusable to the host. */
        heliosview_ui_widget_set_text_callback(m_handle, StaticText);
        heliosview_ui_widget_set_key_callback(m_handle, StaticKey);
        heliosview_ui_widget_set_composition_callback(m_handle, StaticComposition);
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

    // The widget's allocated box. A paint callback draws in widget-local coordinates,
    // so it usually only needs the size: bounds().width / bounds().height.
    helios::Rect bounds() const {
        int x = 0, y = 0, width = 0, height = 0;
        if (m_handle) heliosview_ui_widget_get_bounds(m_handle, &x, &y, &width, &height);
        return helios::Rect{x, y, width, height};
    }

    void requestRepaint() {
        if (m_handle) heliosview_ui_widget_request_repaint(m_handle);
    }

    Widget* setVisible(bool visible) {
        if (m_handle) heliosview_ui_widget_set_visible(m_handle, visible ? 1 : 0);
        return this;
    }

    bool isVisible() const {
        return m_handle ? (heliosview_ui_widget_is_visible(m_handle) != 0) : false;
    }

    // Children management
    Widget* addChild(std::shared_ptr<Widget> child) {
        if (child && m_handle) {
            heliosview_ui_widget_add_child(m_handle, child->handle());
            child->m_isBorrowed = true;
            m_children.push_back(std::move(child));
        }
        return this;
    }

    // Customization Hooks for OOP subclasses. `painter` borrows the host's live
    // session and is only valid for the duration of this call.
    virtual void onPaint(helios::Painter& painter) {}
    virtual bool onMouseEvent(const heliosview_host_mouse_event_t* event) { return false; }

    /* ---- keyboard input ----
     * A widget takes part in keyboard input by overriding one of these; the host
     * routes an event here only while this widget has focus (see focusWidget()).
     * The C layer learns that the widget is focusable by the existence of the
     * callbacks, which Widget installs for every instance. */

    // Committed text (typed keys and IME commits alike), UTF-8. Return true when the
    // text was consumed. Do not leave the caret stale: repaint and report it.
    virtual bool onTextInput(std::string_view /*utf8*/) { return false; }

    // A key press or release, with the HELIOSVIEW_MOD_* bits that were held.
    virtual bool onKeyEvent(heliosview_keycode_t /*key*/, uint32_t /*modifiers*/, bool /*isDown*/) { return false; }

    // IME composition (pre-edit) text, empty when composition ended. Draw it as "not
    // yet committed"; the committed characters arrive later through onTextInput.
    virtual bool onComposition(std::string_view /*utf8*/) { return false; }

    /* ---- focus ---- */

    // Ask this widget's host to focus it (no-op when the tree is detached)
    void focusWidget() {
        if (!m_handle) return;
        if (heliosview_host_t* host = findHost())
            heliosview_host_ui_set_focus(host, m_handle);
    }

    // Whether this widget holds the keyboard focus of its host
    bool hasFocus() const {
        heliosview_host_t* host = findHost();
        return host != nullptr && heliosview_host_ui_get_focus(host) == m_handle;
    }

protected:
    heliosview_ui_widget_t* m_handle = nullptr;
    bool m_isBorrowed = false;
    std::vector<std::shared_ptr<Widget>> m_children;

    heliosview_host_t* findHost() const { return m_host; }

private:
    heliosview_host_t* m_host = nullptr;

public:
    /* Framework plumbing, not application API: UIHost calls these when the tree becomes
     * its root (or leaves it), so the tree knows which host its focus and IME caret
     * reports belong to. An application attaches a tree with UIHost::setRootWidget(). */
    void attachHost(heliosview_host_t* host) {
        m_host = host;
        for (auto& child : m_children) {
            if (child) child->attachHost(host);
        }
    }

    void detachHost() {
        m_host = nullptr;
        for (auto& child : m_children) {
            if (child) child->detachHost();
        }
    }

private:
    static void StaticPaint(heliosview_ui_widget_t*, heliosview_painter_t* p, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        if (self) {
            helios::Painter painter{helios::Painter::Borrowed(p)};
            self->onPaint(painter);
        }
    }

    static int StaticEvent(heliosview_ui_widget_t*, const heliosview_host_mouse_event_t* e, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        return (self && self->onMouseEvent(e)) ? 1 : 0;
    }

    static void StaticDestroy(void*) {
        // C++ unique/shared ownership handles destruction
    }

    static int StaticText(heliosview_ui_widget_t*, const char* utf8, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        if (!self || !utf8) return 0;
        const bool handled = self->onTextInput(std::string_view(utf8));
        if (handled) self->requestRepaint();
        return handled ? 1 : 0;
    }

    static int StaticKey(heliosview_ui_widget_t*, int key, uint32_t modifiers, int isDown, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        if (!self) return 0;
        const bool handled = self->onKeyEvent(static_cast<heliosview_keycode_t>(key), modifiers, isDown != 0);
        if (handled) self->requestRepaint();
        return handled ? 1 : 0;
    }

    static int StaticComposition(heliosview_ui_widget_t*, const char* utf8, void* udata) {
        auto* self = static_cast<Widget*>(udata);
        if (!self) return 0;
        const bool handled = self->onComposition(std::string_view(utf8 ? utf8 : ""));
        if (handled) self->requestRepaint();
        return handled ? 1 : 0;
    }
};

/* ================= Compositional Custom Widget =================
 * Define any widget in-place using lambdas without creating a class.
 */
class CustomWidget : public Widget {
public:
    using PaintFn = std::function<void(CustomWidget* w, helios::Painter& p)>;
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

    void onPaint(helios::Painter& p) override {
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
    explicit Button(const std::string& label) : Widget(nullptr) {
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

/* ================= Built-in Slider ================= */
class Slider : public Widget {
public:
    static std::shared_ptr<Slider> create(float minVal = 0.0f, float maxVal = 100.0f, float initial = 50.0f) {
        return std::shared_ptr<Slider>(new Slider(minVal, maxVal, initial));
    }

    Slider* setValue(float val) {
        float clamped = std::clamp(val, m_min, m_max);
        if (m_val != clamped) {
            m_val = clamped;
            requestRepaint();
            if (m_onChange) m_onChange(m_val);
        }
        return this;
    }

    float value() const noexcept { return m_val; }

    Slider* onChange(std::function<void(float)> cb) {
        m_onChange = std::move(cb);
        return this;
    }

    void onPaint(helios::Painter& p) override {
        const helios::Rect r = bounds();
        const float w = (float)r.width;
        const float h = (float)r.height;
        float trackH = 6.0f;
        float cy = h / 2.0f;
        float padding = 12.0f;
        float availW = w - padding * 2.0f;

        // Inactive background track
        p.setFill(0xFF313244);
        p.setStroke(0, 0);
        p.drawRoundRect(padding, cy - trackH / 2.0f, availW, trackH, trackH / 2.0f);

        // Active highlighted track
        float fraction = (m_max > m_min) ? ((m_val - m_min) / (m_max - m_min)) : 0.0f;
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        float fillW = availW * fraction;
        p.setFill(m_isDragging ? 0xFFCBA6F7 : (m_isHovered ? 0xFFB4BEFE : 0xFF8AADF4));
        p.drawRoundRect(padding, cy - trackH / 2.0f, fillW, trackH, trackH / 2.0f);

        // Thumb knob
        float thumbX = padding + fillW;
        float thumbR = m_isDragging ? 9.0f : (m_isHovered ? 8.0f : 7.0f);
        p.setFill(0xFFCAD3F5);
        p.setStroke(0xFFB4BEFE, 2.0f);
        p.drawEllipse(thumbX - thumbR, cy - thumbR, thumbR * 2.0f, thumbR * 2.0f);
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        const helios::Rect r = bounds();
        const int w = r.width;
        const int h = r.height;
        float padding = 12.0f;
        float availW = (float)w - padding * 2.0f;

        auto updateFromX = [this, availW, padding](int mouseX) {
            if (availW <= 0.0f) return;
            float frac = std::clamp(((float)mouseX - padding) / availW, 0.0f, 1.0f);
            setValue(m_min + frac * (m_max - m_min));
        };

        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            bool inside = (e->x >= 0 && e->x < w && e->y >= 0 && e->y < h);
            if (m_isDragging) {
                updateFromX(e->x);
                return true;
            } else if (inside != m_isHovered) {
                m_isHovered = inside;
                requestRepaint();
            }
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN && e->button == 1) {
            m_isDragging = true;
            m_isHovered = true;
            updateFromX(e->x);
            requestRepaint();
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_UP) {
            if (m_isDragging) {
                m_isDragging = false;
                m_isHovered = (e->x >= 0 && e->x < w && e->y >= 0 && e->y < h);
                requestRepaint();
                return true;
            }
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            if (!m_isDragging && m_isHovered) {
                m_isHovered = false;
                requestRepaint();
            }
        }
        return false;
    }

private:
    Slider(float minVal, float maxVal, float initial)
        : m_min(minVal), m_max(maxVal), m_val(initial) {
        setSize(240, 32);
    }

    float m_min = 0.0f;
    float m_max = 100.0f;
    float m_val = 50.0f;
    bool m_isDragging = false;
    bool m_isHovered = false;
    std::function<void(float)> m_onChange;
};

/* ================= Built-in Toggle Switch ================= */
class Switch : public Widget {
public:
    static std::shared_ptr<Switch> create(bool initial = false) {
        return std::shared_ptr<Switch>(new Switch(initial));
    }

    Switch* setChecked(bool checked) {
        if (m_checked != checked) {
            m_checked = checked;
            requestRepaint();
            if (m_onToggle) m_onToggle(m_checked);
        }
        return this;
    }

    bool isChecked() const noexcept { return m_checked; }

    Switch* onToggle(std::function<void(bool)> cb) {
        m_onToggle = std::move(cb);
        return this;
    }

    void onPaint(helios::Painter& p) override {
        const float h = (float)bounds().height;
        float pillW = 46.0f;
        float pillH = 24.0f;
        float px = 4.0f;
        float py = (h - pillH) / 2.0f;

        // Pill background
        uint32_t bg = m_checked ? (m_isHovered ? 0xFFA6E3A1 : 0xFFA6DA95)
                                : (m_isHovered ? 0xFF45475A : 0xFF313244);
        p.setFill(bg);
        p.setStroke(m_checked ? 0xFF8AADF4 : 0xFF585B70, 1.0f);
        p.drawRoundRect(px, py, pillW, pillH, pillH / 2.0f);

        // Thumb circle
        float thumbR = 9.0f;
        float thumbX = m_checked ? (px + pillW - thumbR - 3.0f) : (px + thumbR + 3.0f);
        float thumbY = py + pillH / 2.0f;
        p.setFill(0xFFFFFFFF);
        p.setStroke(0, 0);
        p.drawEllipse(thumbX - thumbR, thumbY - thumbR, thumbR * 2.0f, thumbR * 2.0f);
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            if (!m_isHovered) { m_isHovered = true; requestRepaint(); }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            if (m_isHovered) { m_isHovered = false; requestRepaint(); }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN && e->button == 1) {
            setChecked(!m_checked);
            return true;
        }
        return false;
    }

private:
    explicit Switch(bool initial) : m_checked(initial) {
        setSize(54, 32);
    }
    bool m_checked = false;
    bool m_isHovered = false;
    std::function<void(bool)> m_onToggle;
};

/* ================= Built-in Checkbox ================= */
class Checkbox : public Widget {
public:
    static std::shared_ptr<Checkbox> create(const std::string& label, bool initial = false) {
        return std::shared_ptr<Checkbox>(new Checkbox(label, initial));
    }

    Checkbox* setChecked(bool checked) {
        if (m_checked != checked) {
            m_checked = checked;
            requestRepaint();
            if (m_onToggle) m_onToggle(m_checked);
        }
        return this;
    }

    bool isChecked() const noexcept { return m_checked; }

    Checkbox* onToggle(std::function<void(bool)> cb) {
        m_onToggle = std::move(cb);
        return this;
    }

    void onPaint(helios::Painter& p) override {
        const float h = (float)bounds().height;
        float boxSize = 18.0f;
        float bx = 4.0f;
        float by = (h - boxSize) / 2.0f;

        // Box
        uint32_t bg = m_checked ? 0xFF8AADF4 : (m_isHovered ? 0xFF313244 : 0xFF1E1E2E);
        p.setFill(bg);
        p.setStroke(m_isHovered ? 0xFFB4BEFE : 0xFF585B70, 1.5f);
        p.drawRoundRect(bx, by, boxSize, boxSize, 4.0f);

        // Vector Checkmark (✓)
        if (m_checked) {
            p.setStroke(0xFF181926, 2.2f);
            p.drawLine(bx + 4.0f, by + 9.0f, bx + 8.0f, by + 13.5f);
            p.drawLine(bx + 8.0f, by + 13.5f, bx + 14.0f, by + 5.0f);
        }

        // Label
        if (!m_label.empty()) {
            p.setFont(helios::FontDesc{"Segoe UI", 13.0f, helios::FontFlag::None});
            p.setFill(m_isHovered ? 0xFFFFFFFF : 0xFFCDD6F4);
            helios::TextMetrics m{};
            p.measureText(m_label, m);
            p.drawText(m_label, bx + boxSize + 10.0f, (h - m.height) / 2.0f);
        }
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            if (!m_isHovered) { m_isHovered = true; requestRepaint(); }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            if (m_isHovered) { m_isHovered = false; requestRepaint(); }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN && e->button == 1) {
            setChecked(!m_checked);
            return true;
        }
        return false;
    }

private:
    Checkbox(std::string label, bool initial)
        : m_label(std::move(label)), m_checked(initial) {
        setSize(220, 28);
    }
    std::string m_label;
    bool m_checked = false;
    bool m_isHovered = false;
    std::function<void(bool)> m_onToggle;
};

/* ================= Built-in ProgressBar ================= */
class ProgressBar : public Widget {
public:
    static std::shared_ptr<ProgressBar> create(float initial = 0.0f) {
        return std::shared_ptr<ProgressBar>(new ProgressBar(initial));
    }

    ProgressBar* setProgress(float val) {
        m_progress = std::clamp(val, 0.0f, 1.0f);
        requestRepaint();
        return this;
    }

    float progress() const noexcept { return m_progress; }

    ProgressBar* setColor(uint32_t barColor) {
        m_barColor = barColor;
        requestRepaint();
        return this;
    }

    void onPaint(helios::Painter& p) override {
        const helios::Rect r = bounds();
        float trackH = (float)r.height;

        // Background track
        p.setFill(0xFF313244);
        p.setStroke(0, 0);
        p.drawRoundRect(0, 0, (float)r.width, trackH, trackH / 2.0f);

        // Filled bar
        if (m_progress > 0.001f) {
            float fillW = (float)r.width * m_progress;
            p.setFill(m_barColor);
            p.drawRoundRect(0, 0, fillW, trackH, trackH / 2.0f);
        }
    }

private:
    explicit ProgressBar(float initial) : m_progress(initial) {
        setSize(260, 10);
    }
    float m_progress = 0.0f;
    uint32_t m_barColor = 0xFFA6E3A1;
};

/* ================= Built-in SegmentedControl / TabBar ================= */
class SegmentedControl : public Widget {
public:
    static std::shared_ptr<SegmentedControl> create(const std::vector<std::string>& items, int initial = 0) {
        return std::shared_ptr<SegmentedControl>(new SegmentedControl(items, initial));
    }

    int selectedIndex() const noexcept { return m_selectedIndex; }

    SegmentedControl* setSelectedIndex(int idx) {
        if (idx >= 0 && idx < (int)m_items.size() && m_selectedIndex != idx) {
            m_selectedIndex = idx;
            requestRepaint();
            if (m_onChange) m_onChange(m_selectedIndex);
        }
        return this;
    }

    SegmentedControl* onChange(std::function<void(int)> cb) {
        m_onChange = std::move(cb);
        return this;
    }

    void onPaint(helios::Painter& p) override {
        const helios::Rect r = bounds();
        const float w = (float)r.width;
        const float h = (float)r.height;
        if (m_items.empty()) return;

        // Container background
        p.setFill(0xFF181926);
        p.setStroke(0xFF313244, 1.0f);
        p.drawRoundRect(0, 0, w, h, 8.0f);

        float tabW = (w - 8.0f) / (float)m_items.size();
        float tabH = h - 8.0f;

        p.setFont(helios::FontDesc{"Segoe UI", 12.0f, helios::FontFlag::Bold});

        for (size_t i = 0; i < m_items.size(); ++i) {
            float tx = 4.0f + i * tabW;
            float ty = 4.0f;

            if ((int)i == m_selectedIndex) {
                // Active pill
                p.setFill(0xFF8AADF4);
                p.setStroke(0, 0);
                p.drawRoundRect(tx, ty, tabW, tabH, 6.0f);
                p.setFill(0xFF181926);
            } else if ((int)i == m_hoveredIndex) {
                // Hover pill
                p.setFill(0xFF24273A);
                p.setStroke(0, 0);
                p.drawRoundRect(tx, ty, tabW, tabH, 6.0f);
                p.setFill(0xFFCDD6F4);
            } else {
                p.setFill(0xFFA6ADC8);
            }

            helios::TextMetrics m{};
            p.measureText(m_items[i], m);
            p.drawText(m_items[i], tx + (tabW - m.width) / 2.0f, ty + (tabH - m.height) / 2.0f);
        }
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        const helios::Rect r = bounds();
        const int w = r.width;
        const int h = r.height;
        if (m_items.empty()) return false;

        float tabW = (float)(w - 8) / (float)m_items.size();
        int idx = (int)(((float)e->x - 4.0f) / tabW);
        idx = std::clamp(idx, 0, (int)m_items.size() - 1);

        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            if (m_hoveredIndex != idx) {
                m_hoveredIndex = idx;
                requestRepaint();
            }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            if (m_hoveredIndex != -1) {
                m_hoveredIndex = -1;
                requestRepaint();
            }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN && e->button == 1) {
            setSelectedIndex(idx);
            return true;
        }
        (void)h;
        return false;
    }

private:
    SegmentedControl(std::vector<std::string> items, int initial)
        : m_items(std::move(items)), m_selectedIndex(initial) {
        setSize(480, 36);
    }

    std::vector<std::string> m_items;
    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    std::function<void(int)> m_onChange;
};

/* ================= UTF-8 helpers =================
 * The widget text buffer is UTF-8 because that is what the platform hands us (typed
 * keys and IME commits both arrive as UTF-8) and what the painter draws. The editing
 * primitives therefore walk code points, never bytes: splitting a multi-byte sequence
 * would corrupt the string and make the painter draw a replacement box where a
 * Chinese character should be.
 */
namespace detail {

// Byte index of the code point after `index`
inline size_t utf8Next(std::string_view s, size_t index) {
    if (index >= s.size()) return s.size();
    size_t i = index + 1;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

// Byte index of the code point before `index`
inline size_t utf8Prev(std::string_view s, size_t index) {
    if (index == 0) return 0;
    size_t i = index - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

inline size_t utf8ClampToBoundary(std::string_view s, size_t index) {
    if (index >= s.size()) return s.size();
    while (index > 0 && (static_cast<unsigned char>(s[index]) & 0xC0) == 0x80) --index;
    return index;
}

// How many code points the range holds (for count-based limits, not byte limits)
inline size_t utf8Count(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i = utf8Next(s, i)) ++n;
    return n;
}

// Byte index at most `budget` code points into the string
inline size_t utf8Advance(std::string_view s, size_t from, size_t budget) {
    size_t i = from;
    while (budget-- > 0 && i < s.size()) i = utf8Next(s, i);
    return i;
}

} // namespace detail

/* ================= Built-in TextField =================
 * A single-line editable text field drawn with the painter, with real Chinese input:
 * the IME's composition (pre-edit) string is drawn in place, underlined, and the
 * candidate window is kept next to the caret through
 * heliosview_ui_widget_report_ime_caret. Committed text arrives as ordinary
 * onTextInput, so ASCII typing, pasted text and an IME commit all take the same path.
 *
 * Keys handled: Left/Right (with Shift to extend), Home/End, Backspace, Delete,
 * Ctrl+A/C/X/V, Return (fires onSubmit). Clicking places the caret; dragging selects.
 */
class TextField : public Widget {
public:
    static std::shared_ptr<TextField> create(int width = 320, int height = 34) {
        auto field = std::shared_ptr<TextField>(new TextField());
        field->setSize(width, height);
        return field;
    }

    // ---- content ----

    TextField* setText(std::string text) {
        m_text = std::move(text);
        m_cursor = m_text.size();
        m_anchor = m_cursor;
        m_composition.clear();
        trace("setText", m_text);
        sync();
        return this;
    }

    const std::string& text() const noexcept { return m_text; }

    TextField* setPlaceholder(std::string ph) {
        m_placeholder = std::move(ph);
        requestRepaint();
        return this;
    }

    // Text drawn before the editable run, inside the frame (a form's label)
    TextField* setPrefix(std::string prefix) {
        m_prefix = std::move(prefix);
        requestRepaint();
        return this;
    }

    TextField* setMaxLength(size_t codePoints) {
        m_maxLength = codePoints;
        requestRepaint();
        return this;
    }

    TextField* setFont(const helios::FontDesc& font) {
        m_font = font;
        sync();
        return this;
    }

    // Fires on every edit, with the field's current text
    TextField* onChange(std::function<void(const std::string&)> cb) {
        m_onChange = std::move(cb);
        return this;
    }

    // Fires on Return, with the field's current text
    TextField* onSubmit(std::function<void(const std::string&)> cb) {
        m_onSubmit = std::move(cb);
        return this;
    }

    // Whether focusing selects the whole value (browser-like). On by default.
    TextField* setSelectAllOnFocus(bool on) {
        m_selectAllOnFocus = on;
        return this;
    }

    // ---- painting ----

    void onPaint(helios::Painter& p) override {
        const helios::Rect r = bounds();
        const float w = (float)r.width;
        const float h = (float)r.height;
        const bool isFocused = hasFocus();

        refreshMetrics(p);

        // Frame
        p.setFill(0xFF181926);
        p.setStroke(isFocused ? 0xFF8AADF4 : 0xFF45475A, isFocused ? 1.6f : 1.0f);
        p.drawRoundRect(0, 0, w, h, 6.0f);

        p.setFont(m_font);
        const float textX = textOrigin();
        const float textY = (h - m_lineHeight) / 2.0f;
        const float caretHeight = m_lineHeight;

        // Prefix label
        if (!m_prefix.empty()) {
            p.setFill(0xFF6E738D);
            p.drawText(m_prefix, 10.0f, textY);
        }

        // Clip the editable run to the frame's inner box
        p.save();
        p.setClipRect(6.0f, 2.0f, w - 12.0f, h - 4.0f);

        const std::string& value = m_text;
        if (value.empty() && m_composition.empty() && !isFocused && !m_placeholder.empty()) {
            p.setFill(0xFF6E738D);
            p.drawText(m_placeholder, textX, textY);
        }

        // Selection behind the text
        if (m_anchor != m_cursor) {
            const float x0 = textX + widthAtLive(std::min(m_anchor, m_cursor));
            const float x1 = textX + widthAtLive(std::max(m_anchor, m_cursor));
            p.setFill(0x668AADF4);
            p.setStroke(0, 0);
            p.drawRect(x0, textY - 2.0f, x1 - x0, caretHeight + 4.0f);
        }

        // Value
        p.setFill(0xFFCDD6F4);
        p.drawText(value, textX, textY);

        // IME composition (pre-edit): shown where it will be inserted, underlined so
        // it reads as "not committed yet"
        const float compositionX = textX + widthAtLive(m_cursor);
        if (!m_composition.empty()) {
            p.setFill(0xFFFFD479);
            p.drawText(m_composition, compositionX, textY);

            helios::TextMetrics cm{};
            p.measureText(m_composition, cm);
            p.setStroke(0xFFF9E2AF, 1.5f);
            p.drawLine(compositionX, textY + caretHeight, compositionX + cm.width, textY + caretHeight);
        }

        // Caret: at the composition start while composing, else at the cursor
        if (isFocused) {
            p.setStroke(0xFFF5C2E7, 1.6f);
            p.drawLine(compositionX, textY, compositionX, textY + caretHeight);
        }

        p.restore();
    }

    // ---- mouse ----

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN && e->button == 1) {
            /* Focus is already set by the host before this callback, so the branch below
             * only runs when the click lands on an unfocused field (a tree without the
             * host's focus handling). Either way the caret goes where the click was. */
            const bool wasFocused = hasFocus();
            if (!wasFocused) {
                const bool selectAll = m_selectAllOnFocus;
                focusWidget();
                if (selectAll) {
                    m_anchor = 0;
                    m_cursor = m_text.size();
                    m_dragging = true; /* the drag may extend this selection */
                    trace("mouseDown/selectAll", {});
                    requestRepaint();
                    return true;
                }
            }
            m_dragging = true;
            setCursorFromX(e->x, false);
            trace("mouseDown/caret", {});
            return true;
        }
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE && m_dragging) {
            setCursorFromX(e->x, true);
            trace("mouseMove/select", {});
            return true;
        }
        if (e->action == HELIOSVIEW_HOST_MOUSE_UP) {
            /* Always end the drag, wherever the release lands: a drag that stays "on"
             * turns every later mouse move into a selection extension, and the next
             * keystroke then replaces that selection -- text disappears. */
            m_dragging = false;
            trace("mouseUp", {});
            return true;
        }
        if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            /* Leaving the widget is not a release, but it must not leave a live drag
             * either: the release may never reach this widget. */
            m_dragging = false;
            return true;
        }
        return false;
    }

    // ---- keyboard ----

    bool onKeyEvent(heliosview_keycode_t key, uint32_t modifiers, bool isDown) override {
        if (!isDown) return true;

        const bool shift = (modifiers & HELIOSVIEW_MOD_SHIFT) != 0;
        const bool ctrl = (modifiers & HELIOSVIEW_MOD_CTRL) != 0;

        switch (key) {
        case HELIOSVIEW_KEY_LEFT:
            moveCursor(detail::utf8Prev(m_text, m_cursor), shift);
            return true;
        case HELIOSVIEW_KEY_RIGHT:
            moveCursor(detail::utf8Next(m_text, m_cursor), shift);
            return true;
        case HELIOSVIEW_KEY_HOME:
            moveCursor(0, shift);
            return true;
        case HELIOSVIEW_KEY_END:
            moveCursor(m_text.size(), shift);
            return true;
        case HELIOSVIEW_KEY_BACKSPACE:
            if (deleteSelection()) break;
            if (m_cursor > 0) {
                const size_t from = detail::utf8Prev(m_text, m_cursor);
                m_text.erase(from, m_cursor - from);
                m_cursor = from;
                m_anchor = from;
                trace("key/backspace", {});
                notifyChanged();
            }
            return true;
        case HELIOSVIEW_KEY_DELETE:
            if (deleteSelection()) break;
            if (m_cursor < m_text.size()) {
                m_text.erase(m_cursor, detail::utf8Next(m_text, m_cursor) - m_cursor);
                m_anchor = m_cursor;
                trace("key/delete", {});
                notifyChanged();
            }
            return true;
        case HELIOSVIEW_KEY_RETURN:
            if (m_onSubmit) m_onSubmit(m_text);
            return true;
        case HELIOSVIEW_KEY_A:
            if (ctrl) {
                m_anchor = 0;
                m_cursor = m_text.size();
                requestRepaint();
                return true;
            }
            return false;
        case HELIOSVIEW_KEY_C:
            if (ctrl) {
                helios::clipboardSetText(selectedText());
                return true;
            }
            return false;
        case HELIOSVIEW_KEY_X:
            if (ctrl) {
                const std::string sel = selectedText();
                if (!sel.empty()) {
                    helios::clipboardSetText(sel);
                    if (deleteSelection()) return true;
                }
                return true;
            }
            return false;
        case HELIOSVIEW_KEY_V:
            if (ctrl) {
                std::string pasted;
                if (helios::clipboardGetText(pasted) && !pasted.empty()) {
                    // A single-line field takes the first line of a multi-line paste.
                    // Truncate AT the break rather than removing it: removing only the
                    // '\n' of a CRLF pair would leave a bare '\r' in the value, which
                    // renders as a stray control character.
                    if (const size_t nl = pasted.find_first_of("\r\n"); nl != std::string::npos)
                        pasted.resize(nl);
                    insertText(pasted);
                }
                return true;
            }
            return false;
        case HELIOSVIEW_KEY_ESCAPE:
        case HELIOSVIEW_KEY_TAB:
            return false; /* let the app use these */
        default:
            return false; /* not an editing key: do not claim it */
        }

        return false; /* unreachable: every case above returns */
    }
    // ---- text / IME ----

    bool onTextInput(std::string_view utf8) override {
        if (utf8.empty()) return true;

        /* Trace when HELIOSVIEW_UI_LOG=1: the value's byte count before and after, and
         * where the caret sits, is what shows a byte-level edit problem. */
        trace("onTextInput", utf8);

        std::string incoming(utf8);
        // An IME commit replaces the composition string it was previewing
        if (!m_composition.empty()) {
            m_text.replace(m_cursor, m_composition.size(), incoming);
            m_cursor += incoming.size();
            m_composition.clear();
            m_anchor = m_cursor;
            notifyChanged();
            trace("onTextInput/after", {});
            return true;
        }
        insertText(incoming);
        trace("onTextInput/after", {});
        return true;
    }

    bool onComposition(std::string_view utf8) override {
        /* The composition string is not part of the value; it is previewed at the
         * caret until the IME commits it (which arrives through onTextInput). */
        m_composition.assign(utf8);
        reportCaretToIme();
        requestRepaint();
        return true;
    }

protected:
    // Protected rather than private: a field with different editing rules is a
    // subclass (Widget's documented OOP path), and create() stays the usual way to
    // make a plain one.
    TextField() = default;

private:

    /* Diagnostic trace, compiled in but inert unless the environment enables it (see
     * heliosview_ui_debug_log). Kept out of the edit logic so a failing edit points at
     * the operation, not at the logging. */
    void trace(const char* what, std::string_view incoming) const {
        char line[512];
        std::snprintf(line, sizeof line,
                      "TextField %p %-22s in=%zu cursor=%zu anchor=%zu value=%zu bytes",
                      static_cast<const void*>(this), what,
                      incoming.size(), m_cursor, m_anchor, m_text.size());
        heliosview_ui_debug_log(line);
    }

    // ---- text access ----

    std::string selectedText() const {
        if (m_anchor == m_cursor) return {};
        const size_t from = std::min(m_anchor, m_cursor);
        const size_t to = std::max(m_anchor, m_cursor);
        return m_text.substr(from, to - from);
    }

    void moveCursor(size_t to, bool extendSelection) {
        m_cursor = std::min(to, m_text.size());
        if (!extendSelection) m_anchor = m_cursor;
        reportCaretToIme();
        requestRepaint();
    }

    bool deleteSelection() {
        if (m_anchor == m_cursor) return false;
        const size_t from = std::min(m_anchor, m_cursor);
        const size_t to = std::max(m_anchor, m_cursor);
        m_text.erase(from, to - from);
        m_cursor = from;
        m_anchor = from;
        trace("deleteSelection", {});
        notifyChanged();
        return true;
    }

    void insertText(const std::string& incoming) {
        deleteSelection();
        std::string insert = incoming;
        if (m_maxLength > 0) {
            const size_t room = m_maxLength > detail::utf8Count(m_text) ? m_maxLength - detail::utf8Count(m_text) : 0;
            if (detail::utf8Count(insert) > room)
                insert.resize(detail::utf8Advance(insert, 0, room));
        }
        trace("insertText", insert);
        m_text.insert(m_cursor, insert);
        m_cursor += insert.size();
        m_anchor = m_cursor;
        notifyChanged();
    }

    void notifyChanged() {
        reportCaretToIme();
        requestRepaint();
        if (m_onChange) m_onChange(m_text);
    }

    void sync() {
        m_cursor = std::min(m_cursor, m_text.size());
        m_cursor = detail::utf8ClampToBoundary(m_text, m_cursor);
        m_anchor = std::min(m_anchor, m_text.size());
        m_anchor = detail::utf8ClampToBoundary(m_text, m_anchor);
        requestRepaint();
    }

    // ---- geometry ----

    float textOrigin() const {
        return m_prefix.empty() ? 10.0f : 10.0f + m_prefixWidth + 8.0f;
    }

    // Width of m_text[0, index) with the field's font.
    //
    // Text measurement needs a painter, and a painter is only valid inside the paint
    // call it belongs to. Input handling (caret moves, IME caret reporting) runs
    // outside the paint cycle, so it measures on a scratch canvas of its own rather
    // than caching the live painter -- caching one would leave the field holding a
    // painter whose session the host already ended.
    float widthAtLive(size_t index) const {
        if (index == 0) return 0.0f;

        helios::Canvas scratch(64, 32);
        if (!scratch.valid()) return 0.0f;
        helios::Painter painter(scratch);
        if (!painter.valid()) return 0.0f;

        painter.setFont(m_font);
        helios::TextMetrics m{};
        painter.measureText(std::string_view(m_text).substr(0, detail::utf8ClampToBoundary(m_text, index)), m);
        return m.width;
    }

    // The caret's x inside the field: the composition start while composing, since
    // that is where the previewed text begins.
    float caretX() const {
        return textOrigin() + widthAtLive(m_cursor);
    }

    void setCursorFromX(int mouseX, bool extendSelection) {
        // Nearest code point boundary to the click
        const float target = (float)mouseX - textOrigin();
        size_t best = 0;
        if (target > 0.0f) {
            float bestDistance = target;
            for (size_t i = 0; i < m_text.size();) {
                const size_t next = detail::utf8Next(m_text, i);
                const float w = widthAtLive(next);
                const float distance = w > target ? w - target : target - w;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = next;
                }
                i = next;
            }
        }
        moveCursor(best, extendSelection);
    }

    void reportCaretToIme() {
        // Keep the IME's composition window and candidate list next to the caret; the
        // C layer translates this local point into the host's client space.
        const int height = m_lineHeight > 0.0f ? (int)(m_lineHeight + 0.5f) : (int)(m_font.size + 0.5f);
        heliosview_ui_widget_report_ime_caret(m_handle, (int)(caretX() + 0.5f), 0, (float)height);
    }

    // Cache the metrics the mouse and IME paths need; called from onPaint where a
    // painter is live.
    void refreshMetrics(helios::Painter& p) {
        if (!m_prefix.empty()) {
            helios::TextMetrics m{};
            p.measureText(m_prefix, m);
            m_prefixWidth = m.width;
        }
        helios::TextMetrics line{};
        p.measureText("Ag", line);
        m_lineHeight = line.height > 0.0f ? line.height : m_font.size;
    }

    // ---- state ----
    std::string m_text;
    std::string m_placeholder = "Type here (\xe4\xb8\xad\xe6\x96\x87\xe5\x8f\xaf\xe8\xbe\x93\xe5\x85\xa5)";
    std::string m_prefix;
    std::string m_composition; /* IME pre-edit, not part of m_text */
    helios::FontDesc m_font{"Segoe UI", 14.0f, helios::FontFlag::None};

    size_t m_cursor = 0; /* byte offset into m_text, always on a code point boundary */
    size_t m_anchor = 0; /* the other end of the selection; == m_cursor means none */
    bool m_dragging = false;
    bool m_selectAllOnFocus = true;
    size_t m_maxLength = 0; /* 0 = unlimited (code points) */

    float m_lineHeight = 18.0f;
    float m_prefixWidth = 0.0f;

    std::function<void(const std::string&)> m_onChange;
    std::function<void(const std::string&)> m_onSubmit;
};

/* ================= Built-in Card Panel ================= */
class Card : public Widget {
public:
    static std::shared_ptr<Card> create(int width, int height, uint32_t bg = 0xFF1E1E2E, uint32_t border = 0xFF313244) {
        return std::shared_ptr<Card>(new Card(width, height, bg, border));
    }

    void onPaint(helios::Painter& p) override {
        const helios::Rect r = bounds();
        p.setFill(m_bg);
        p.setStroke(m_border, 1.0f);
        p.drawRoundRect(0, 0, (float)r.width, (float)r.height, 10.0f);
    }

private:
    Card(int width, int height, uint32_t bg, uint32_t border)
        : m_bg(bg), m_border(border) {
        setSize(width, height);
    }
    uint32_t m_bg;
    uint32_t m_border;
};

} // namespace HeliosView::UI

#endif /* HELIOSVIEW_CORE_UI_WIDGET_H */
