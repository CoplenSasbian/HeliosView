// HeliosView Demo -- Comprehensive Custom UI Showcase
// Demonstrates 3 tiers of custom widget creation:
//   1. Tier 1: Pure C ABI custom widget via heliosview_ui_widget_desc_t
//   2. Tier 2: Modern C++ OOP class derivation (Widget -> CustomCard)
//   3. Tier 3: In-line Lambda composition (CustomWidget::create()->setPaint(...))

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/UI/Widget.h>
#include <iostream>
#include <cmath>

using namespace HeliosView::UI;

// ================= Tier 1: Pure C ABI Custom Widget =================
// A Circular Gauge indicator built with pure C descriptor
struct CircleGaugeData {
    float percent = 0.65f;
    bool hovered = false;
};

static void CircleGaugePaint(heliosview_ui_widget_t* w, heliosview_painter_t* p, void* udata) {
    auto* data = static_cast<CircleGaugeData*>(udata);
    int width = 0, height = 0;
    heliosview_ui_widget_get_bounds(w, nullptr, nullptr, &width, &height);

    float cx = width / 2.0f;
    float cy = height / 2.0f;
    float r = std::min(width, height) / 2.0f - 6.0f;

    // Track circle
    heliosview_painter_set_stroke(p, 0xFF313244, 8.0f);
    heliosview_painter_set_fill(p, 0);
    heliosview_painter_draw_ellipse(p, cx - r, cy - r, r * 2, r * 2);

    // Active progress arc
    uint32_t activeColor = data->hovered ? 0xFFF5BDE6 : 0xFF8AADF4;
    heliosview_painter_set_stroke(p, activeColor, 8.0f);
    heliosview_painter_draw_arc(p, cx - r, cy - r, r * 2, r * 2, -90.0f, data->percent * 360.0f);

    // Center text
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", (int)(data->percent * 100.0f));
    heliosview_font_desc_t font{"Segoe UI", 16.0f, HELIOSVIEW_FONT_BOLD};
    heliosview_painter_set_font(p, &font);
    heliosview_painter_set_fill(p, 0xFFCAD3F5);

    heliosview_text_metrics_t m{};
    heliosview_painter_measure_text(p, buf, &m);
    heliosview_painter_draw_text(p, buf, cx - m.width / 2.0f, cy - m.height / 2.0f);
}

static int CircleGaugeEvent(heliosview_ui_widget_t* w, const heliosview_host_mouse_event_t* e, void* udata) {
    auto* data = static_cast<CircleGaugeData*>(udata);
    if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
        if (!data->hovered) {
            data->hovered = true;
            heliosview_ui_widget_request_repaint(w);
        }
        return 1;
    } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
        if (data->hovered) {
            data->hovered = false;
            heliosview_ui_widget_request_repaint(w);
        }
        return 1;
    } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN) {
        data->percent += 0.10f;
        if (data->percent > 1.0f) data->percent = 0.1f;
        heliosview_ui_widget_request_repaint(w);
        return 1;
    }
    return 0;
}

static void CircleGaugeDestroy(void* udata) {
    delete static_cast<CircleGaugeData*>(udata);
}

std::shared_ptr<Widget> CreatePureCCircleGauge() {
    auto* data = new CircleGaugeData();
    heliosview_ui_widget_desc_t desc{};
    desc.paint = CircleGaugePaint;
    desc.event = CircleGaugeEvent;
    desc.destroy = CircleGaugeDestroy;

    auto* raw = heliosview_ui_widget_create(&desc, data);
    heliosview_ui_widget_set_bounds(raw, 0, 0, 90, 90);
    return std::make_shared<Widget>(raw);
}

// ================= Tier 2: Modern C++ OOP Subclass =================
// A stylish Card component that encapsulates custom drawing
class CustomCard : public Widget {
public:
    static std::shared_ptr<CustomCard> create(std::string title) {
        auto card = std::make_shared<CustomCard>();
        card->m_title = std::move(title);
        card->setSize(340, 110);
        return card;
    }

    void onPaint(heliosview_painter_t* p) override {
        int w = 0, h = 0;
        heliosview_ui_widget_get_bounds(m_handle, nullptr, nullptr, &w, &h);

        // Background with subtle border
        heliosview_painter_set_fill(p, m_isHovered ? 0xFF24273A : 0xFF1E1E2E);
        heliosview_painter_set_stroke(p, 0xFF363A4F, 1.5f);
        heliosview_painter_draw_round_rect(p, 0, 0, (float)w, (float)h, 12.0f);

        // Title
        heliosview_font_desc_t fontTitle{"Segoe UI", 15.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontTitle);
        heliosview_painter_set_fill(p, 0xFFF5BDE6);
        heliosview_painter_draw_text(p, m_title.c_str(), 16.0f, 16.0f);

        // Subtitle
        heliosview_font_desc_t fontSub{"Segoe UI", 12.0f, 0};
        heliosview_painter_set_font(p, &fontSub);
        heliosview_painter_set_fill(p, 0xFFA5ADCB);
        heliosview_painter_draw_text(p, "Derived C++ class widget with custom onPaint", 16.0f, 40.0f);
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            if (!m_isHovered) {
                m_isHovered = true;
                requestRepaint();
            }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            if (m_isHovered) {
                m_isHovered = false;
                requestRepaint();
            }
            return true;
        }
        return false;
    }

private:
    std::string m_title;
    bool m_isHovered = false;
};

// ================= Main App =================
int main() {
    // 1. Create window shell (600 x 700)
    heliosview_window_t* win = heliosview_window_create(600, 700, "HeliosView Custom UI Showcase");
    if (!win) return 1;
    
    // 2. Create UIHost
    heliosview_host_t* host = heliosview_host_create_ui(win, 0, 0, 600, 700, HELIOSVIEW_ENGINE_BLEND2D);

    // 3. Assemble UI Tree
    auto rootStack = VStack::create(15, 25);

    // Top Header (Built-in Label)
    auto header = Label::create("Custom UI Component Showcase");
    header->setFontSize(22.0f);
    header->setColor(0xFFCAD3F5);
    rootStack->add(header);

    // Tier 2: OOP Card
    auto card = CustomCard::create("Tier 2: OOP CustomCard");
    rootStack->add(card);

    // Tier 1 & 3 inside an HStack
    auto hstack = HStack::create(20, 0);

    // Tier 1: Pure C Circle Gauge
    auto gauge = CreatePureCCircleGauge();
    hstack->add(gauge);

    // Tier 3: In-line Lambda Custom Waveform
    float wavePhase = 0.0f;
    auto waveWidget = CustomWidget::create();
    waveWidget->setSize(230, 90);
    waveWidget->setPaint([&](CustomWidget* self, heliosview_painter_t* p) {
        int w = 0, h = 0;
        heliosview_ui_widget_get_bounds(self->handle(), nullptr, nullptr, &w, &h);

        // Container box
        heliosview_painter_set_fill(p, 0xFF181926);
        heliosview_painter_set_stroke(p, 0xFF363A4F, 1.0f);
        heliosview_painter_draw_round_rect(p, 0, 0, (float)w, (float)h, 8.0f);

        // Sine wave
        heliosview_painter_set_stroke(p, 0xFFA6DA95, 2.0f);
        float prev_x = 10, prev_y = h / 2.0f;
        for (float x = 10; x < w - 10; x += 6) {
            float y = h / 2.0f + std::sin((x * 0.05f) + wavePhase) * 25.0f;
            heliosview_painter_draw_line(p, prev_x, prev_y, x, y);
            prev_x = x;
            prev_y = y;
        }
    });

    waveWidget->setMouse([&](CustomWidget*, const heliosview_host_mouse_event_t* e) {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            wavePhase += 0.2f;
            waveWidget->requestRepaint();
            return true;
        }
        return false;
    });

    hstack->add(waveWidget);
    rootStack->add(hstack);

    // Built-in Button at the bottom
    auto btn = Button::create("Click to Shift Wave");
    btn->setSize(200, 42);
    btn->onClick([&]() {
        wavePhase += 1.5f;
        waveWidget->requestRepaint();
        std::cout << "[Event] Button Clicked! Shifting wave phase...\n";
    });
    rootStack->add(btn);

    // 4. Attach widget tree to UIHost
    heliosview_host_ui_set_root(host, rootStack->handle());

    heliosview_window_show(win);

    // 5. Message loop with close request handling
    struct DemoContext {
        heliosview_window_t* win;
        heliosview_host_t* host;
    } ctx{win, host};

    heliosview_run([](void* udata) -> int {
        auto* c = static_cast<DemoContext*>(udata);
        heliosview_event_t ev;
        while (heliosview_poll(&ev)) {
            if (ev.type == HELIOSVIEW_EVENT_WINDOW_CLOSE) {
                if (c->host) {
                    heliosview_host_destroy(c->host);
                    c->host = nullptr;
                }
                if (c->win) {
                    heliosview_window_destroy(c->win);
                    c->win = nullptr;
                }
                heliosview_quit();
                return 1;
            }
        }
        return 0;
    }, &ctx);

    return 0;
}
