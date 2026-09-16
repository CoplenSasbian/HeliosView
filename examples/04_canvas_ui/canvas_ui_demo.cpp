// ============================================================================
// HeliosView Example 04: Canvas DirectDraw & Retained UI Component Gallery
// ============================================================================
// A comprehensive showcase of HeliosView's 2D vector drawing engine & modern
// retained-mode UI component system:
//   Tab 0: Controls & Forms      - Buttons, Sliders, Switches, Checkboxes, Progress, Badges
//   Tab 1: Charts & Analytics    - Real-time Area Line Chart with Crosshair, Bar Chart, Donut Gauges
//   Tab 2: Vector & Paths        - Even-Odd Stars, Bezier Curves, Rotating Interlocking Gears
//   Tab 3: Gauges & Oscilloscope - Semicircular Analog Tachometer, Dual-Channel 60FPS Waveform
// ============================================================================

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/UI/Widget.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace HeliosView::UI;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ----------------------------------------------------------------------------
// Shared Global State for Interactive Showcase
// ----------------------------------------------------------------------------
struct AppState {
    int activeTab = 0;
    int clickCounter = 0;
    bool jitEnabled = true;
    bool antialiasEnabled = true;
    bool vsyncEnabled = true;
    bool deepDarkEnabled = true;
    bool highDpiEnabled = false;

    float volume = 74.0f;
    float brightness = 85.0f;
    float animSpeed = 1.0f;

    // Animation & Waveform State
    float timeSec = 0.0f;
    float gearAngle = 0.0f;
    float tachometerVal = 88.0f;

    // Line Chart Crosshair
    int chartHoverX = -1;
    int chartHoverY = -1;
    bool chartHovered = false;

    // Bar Chart Hover
    int barHoveredIndex = -1;
};

static AppState g_state;

// ----------------------------------------------------------------------------
// Helpers for Vector Drawing
// ----------------------------------------------------------------------------
static void DrawStar(heliosview_painter_t* p, float cx, float cy, float outerR, float innerR, int points, uint32_t fill, uint32_t stroke) {
    std::vector<float> pts;
    pts.reserve(points * 4);
    for (int i = 0; i < points * 2; ++i) {
        float r = (i % 2 == 0) ? outerR : innerR;
        float angle = -kPi / 2.0f + i * (kPi / points);
        pts.push_back(cx + std::cos(angle) * r);
        pts.push_back(cy + std::sin(angle) * r);
    }
    heliosview_painter_set_fill(p, fill);
    heliosview_painter_set_stroke(p, stroke, 1.5f);
    heliosview_painter_draw_polygon(p, pts.data(), pts.size() / 2);
}

static void DrawGear(heliosview_painter_t* p, float cx, float cy, float radius, int teeth, float angleRad, uint32_t color) {
    heliosview_painter_save(p);
    heliosview_painter_translate(p, cx, cy);
    heliosview_painter_rotate(p, angleRad);

    std::vector<float> pts;
    float dAngle = (2.0f * kPi) / teeth;
    float toothH = radius * 0.18f;

    for (int i = 0; i < teeth; ++i) {
        float a0 = i * dAngle;
        float a1 = a0 + dAngle * 0.25f;
        float a2 = a0 + dAngle * 0.50f;
        float a3 = a0 + dAngle * 0.75f;

        pts.push_back(std::cos(a0) * radius);
        pts.push_back(std::sin(a0) * radius);

        pts.push_back(std::cos(a1) * (radius + toothH));
        pts.push_back(std::sin(a1) * (radius + toothH));

        pts.push_back(std::cos(a2) * (radius + toothH));
        pts.push_back(std::sin(a2) * (radius + toothH));

        pts.push_back(std::cos(a3) * radius);
        pts.push_back(std::sin(a3) * radius);
    }

    heliosview_painter_set_fill(p, color);
    heliosview_painter_set_stroke(p, 0xFF181926, 1.5f);
    heliosview_painter_draw_polygon(p, pts.data(), pts.size() / 2);

    // Center hole
    heliosview_painter_set_fill(p, 0xFF1E1E2E);
    heliosview_painter_set_stroke(p, 0xFF45475A, 1.5f);
    heliosview_painter_draw_ellipse(p, -radius * 0.35f, -radius * 0.35f, radius * 0.70f, radius * 0.70f);

    heliosview_painter_restore(p);
}

// ----------------------------------------------------------------------------
// Tab 0: Controls & Forms View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabControls() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: Command & State Controls ----
    auto leftCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto leftStack = VStack::create(14, 18);

    auto titleL = Label::create("Command & Toggle Controls");
    titleL->setFontSize(16.0f);
    titleL->setColor(0xFF8AADF4);
    leftStack->add(titleL);

    auto btnRow = HStack::create(12, 0);
    auto clickBtn = Button::create("Primary Click (0)");
    clickBtn->setSize(160, 36);
    auto resetBtn = Button::create("Reset Counter");
    resetBtn->setSize(120, 36);

    clickBtn->onClick([clickBtn]() {
        g_state.clickCounter++;
        clickBtn->setLabel(std::format("Primary Click ({})", g_state.clickCounter));
    });
    resetBtn->onClick([clickBtn]() {
        g_state.clickCounter = 0;
        clickBtn->setLabel("Primary Click (0)");
    });
    btnRow->add(clickBtn);
    btnRow->add(resetBtn);
    leftStack->add(btnRow);

    auto sep1 = CustomWidget::create();
    sep1->setSize(434, 1);
    sep1->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        heliosview_painter_set_fill(p, 0xFF313244);
        heliosview_painter_draw_rect(p, 0, 0, 434, 1);
    });
    leftStack->add(sep1);

    // Switches
    auto swRow1 = HStack::create(12, 0);
    auto sw1 = Switch::create(g_state.jitEnabled);
    sw1->onToggle([](bool on) { g_state.jitEnabled = on; });
    auto sw1Lbl = Label::create("Hardware JIT Code Generation (Blend2D)");
    sw1Lbl->setFontSize(13.0f)->setColor(0xFFCDD6F4);
    swRow1->add(sw1)->add(sw1Lbl);
    leftStack->add(swRow1);

    auto swRow2 = HStack::create(12, 0);
    auto sw2 = Switch::create(g_state.antialiasEnabled);
    sw2->onToggle([](bool on) { g_state.antialiasEnabled = on; });
    auto sw2Lbl = Label::create("Analytic Subpixel Anti-Aliasing");
    sw2Lbl->setFontSize(13.0f)->setColor(0xFFCDD6F4);
    swRow2->add(sw2)->add(sw2Lbl);
    leftStack->add(swRow2);

    // Checkboxes
    auto cb1 = Checkbox::create("Enable VSync (Lock 60 FPS)", g_state.vsyncEnabled);
    cb1->onToggle([](bool on) { g_state.vsyncEnabled = on; });
    leftStack->add(cb1);

    auto cb2 = Checkbox::create("Deep Dark High-Contrast Canvas Mode", g_state.deepDarkEnabled);
    cb2->onToggle([](bool on) { g_state.deepDarkEnabled = on; });
    leftStack->add(cb2);

    auto cb3 = Checkbox::create("High-DPI Subpixel Font Kerning", g_state.highDpiEnabled);
    cb3->onToggle([](bool on) { g_state.highDpiEnabled = on; });
    leftStack->add(cb3);

    leftCard->addChild(leftStack);
    tabRoot->add(leftCard);

    // ---- Right Card: Sliders, Progress & Badges ----
    auto rightCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto rightStack = VStack::create(14, 18);

    auto titleR = Label::create("Sliders, Indicators & Badges");
    titleR->setFontSize(16.0f);
    titleR->setColor(0xFFA6E3A1);
    rightStack->add(titleR);

    // Volume Slider & Label
    auto volLbl = Label::create(std::format("Master Volume Output: {:.0f}%", g_state.volume));
    volLbl->setFontSize(13.0f)->setColor(0xFFCAD3F5);
    rightStack->add(volLbl);

    auto volSlider = Slider::create(0.0f, 100.0f, g_state.volume);
    volSlider->setSize(434, 28);

    // Progress Bar connected to Slider
    auto volBar = ProgressBar::create(g_state.volume / 100.0f);
    volBar->setSize(434, 8);
    volBar->setColor(0xFF8AADF4);

    volSlider->onChange([volLbl, volBar](float val) {
        g_state.volume = val;
        volLbl->setText(std::format("Master Volume Output: {:.0f}%", val));
        volBar->setProgress(val / 100.0f);
    });
    rightStack->add(volSlider);
    rightStack->add(volBar);

    // Brightness Slider
    auto brLbl = Label::create(std::format("Backlight Panel Brightness: {:.0f}%", g_state.brightness));
    brLbl->setFontSize(13.0f)->setColor(0xFFCAD3F5);
    rightStack->add(brLbl);

    auto brSlider = Slider::create(0.0f, 100.0f, g_state.brightness);
    brSlider->setSize(434, 28);
    brSlider->onChange([brLbl](float val) {
        g_state.brightness = val;
        brLbl->setText(std::format("Backlight Panel Brightness: {:.0f}%", val));
    });
    rightStack->add(brSlider);

    // Striped Animated Progress Bar
    auto animBarLbl = Label::create("Live Animated Streaming Buffer (60FPS Striped)");
    animBarLbl->setFontSize(13.0f)->setColor(0xFFBAC2DE);
    rightStack->add(animBarLbl);

    auto stripedBar = CustomWidget::create();
    stripedBar->setSize(434, 14);
    stripedBar->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        int width = 0, height = 0;
        heliosview_ui_widget_get_bounds(w->handle(), nullptr, nullptr, &width, &height);

        // Track
        heliosview_painter_set_fill(p, 0xFF313244);
        heliosview_painter_draw_round_rect(p, 0, 0, (float)width, (float)height, height / 2.0f);

        // Active animated stripes
        float fillW = width * 0.78f;
        heliosview_painter_save(p);
        heliosview_painter_set_clip_rect(p, 0, 0, fillW, (float)height);

        heliosview_painter_set_fill(p, 0xFFCBA6F7);
        heliosview_painter_draw_round_rect(p, 0, 0, fillW, (float)height, height / 2.0f);

        float stripeW = 16.0f;
        float offset = std::fmod(g_state.timeSec * 35.0f, stripeW * 2.0f);
        heliosview_painter_set_fill(p, 0xFFB4BEFE);

        for (float sx = -stripeW * 2.0f + offset; sx < fillW + stripeW; sx += stripeW * 2.0f) {
            float pts[] = {
                sx, (float)height,
                sx + stripeW, (float)height,
                sx + stripeW + 8.0f, 0.0f,
                sx + 8.0f, 0.0f
            };
            heliosview_painter_draw_polygon(p, pts, 4);
        }
        heliosview_painter_restore(p);
    });
    rightStack->add(stripedBar);

    // Badges Row
    auto badgeRow = HStack::create(12, 0);
    auto makeBadge = [](const char* text, uint32_t bg, uint32_t fg) {
        auto badge = CustomWidget::create();
        badge->setSize(130, 32);
        badge->setPaint([text, bg, fg](CustomWidget* w, heliosview_painter_t* p) {
            int bw = 0, bh = 0;
            heliosview_ui_widget_get_bounds(w->handle(), nullptr, nullptr, &bw, &bh);
            heliosview_painter_set_fill(p, bg);
            heliosview_painter_set_stroke(p, fg, 1.0f);
            heliosview_painter_draw_round_rect(p, 0, 0, (float)bw, (float)bh, 6.0f);

            heliosview_font_desc_t font{"Segoe UI", 11.0f, HELIOSVIEW_FONT_BOLD};
            heliosview_painter_set_font(p, &font);
            heliosview_painter_set_fill(p, fg);
            heliosview_text_metrics_t m{};
            heliosview_painter_measure_text(p, text, &m);
            heliosview_painter_draw_text(p, text, (bw - m.width) / 2.0f, (bh - m.height) / 2.0f);
        });
        return badge;
    };

    badgeRow->add(makeBadge("[STATUS: READY]", 0xFF1E2E24, 0xFFA6E3A1));
    badgeRow->add(makeBadge("[AVX2 / JIT]", 0xFF1E243A, 0xFF8AADF4));
    badgeRow->add(makeBadge("[SANDBOXED]", 0xFF2A1E34, 0xFFCBA6F7));
    rightStack->add(badgeRow);

    rightCard->addChild(rightStack);
    tabRoot->add(rightCard);

    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 1: Charts & Analytics View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabCharts() {
    auto tabRoot = VStack::create(16, 0);

    // Top: Real-Time Area Line Chart with Interactive Crosshair & Tooltip
    auto lineChartCard = Card::create(956, 260, 0xFF1E1E2E, 0xFF313244);
    auto lineChart = CustomWidget::create();
    lineChart->setSize(956, 260);

    lineChart->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        int cw = 0, ch = 0;
        heliosview_ui_widget_get_bounds(w->handle(), nullptr, nullptr, &cw, &ch);

        // Header Title
        heliosview_font_desc_t fontH{"Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFF8AADF4);
        heliosview_painter_draw_text(p, "Real-Time Multi-Point Telemetry & Load Monitor (Area Gradient)", 20.0f, 16.0f);

        float left = 50.0f;
        float right = (float)cw - 24.0f;
        float top = 46.0f;
        float bottom = (float)ch - 30.0f;
        float plotW = right - left;
        float plotH = bottom - top;

        // Background Grid & Y-Axis Labels
        heliosview_font_desc_t fontAxis{"Segoe UI", 10.0f, 0};
        heliosview_painter_set_font(p, &fontAxis);

        for (int i = 0; i <= 4; ++i) {
            float y = bottom - i * (plotH / 4.0f);
            heliosview_painter_set_stroke(p, 0xFF282A3A, 1.0f);
            heliosview_painter_draw_line(p, left, y, right, y);

            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", i * 25);
            heliosview_painter_set_fill(p, 0xFF6E738D);
            heliosview_painter_draw_text(p, buf, 14.0f, y - 6.0f);
        }

        // Generate dynamic waveform data points
        const int numPts = 64;
        std::vector<float> pts;
        pts.reserve((numPts + 2) * 2);

        for (int i = 0; i < numPts; ++i) {
            float px = left + (float)i / (numPts - 1) * plotW;
            float t = g_state.timeSec * 2.0f + (float)i * 0.12f;
            float val = 0.45f + 0.28f * std::sin(t) + 0.12f * std::cos(t * 2.3f + 1.2f);
            val = std::clamp(val, 0.05f, 0.95f);
            float py = bottom - val * plotH;
            pts.push_back(px);
            pts.push_back(py);
        }

        // Area under curve
        std::vector<float> poly = pts;
        poly.push_back(right);
        poly.push_back(bottom);
        poly.push_back(left);
        poly.push_back(bottom);

        heliosview_painter_set_fill(p, 0x30A6E3A1);
        heliosview_painter_set_stroke(p, 0, 0);
        heliosview_painter_draw_polygon(p, poly.data(), poly.size() / 2);

        // Curve stroke
        heliosview_painter_set_stroke(p, 0xFFA6E3A1, 2.2f);
        heliosview_painter_draw_polyline(p, pts.data(), pts.size() / 2, 0);

        // Secondary Telemetry Stream (Mauve)
        std::vector<float> pts2;
        pts2.reserve(numPts * 2);
        for (int i = 0; i < numPts; ++i) {
            float px = left + (float)i / (numPts - 1) * plotW;
            float t = g_state.timeSec * 1.5f + (float)i * 0.09f;
            float val = 0.32f + 0.20f * std::sin(t * 1.6f + 2.0f);
            val = std::clamp(val, 0.05f, 0.95f);
            float py = bottom - val * plotH;
            pts2.push_back(px);
            pts2.push_back(py);
        }
        heliosview_painter_set_stroke(p, 0xFFCBA6F7, 1.8f);
        heliosview_painter_draw_polyline(p, pts2.data(), pts2.size() / 2, 0);

        // Crosshair & Data Tooltip when hovered
        if (g_state.chartHovered && g_state.chartHoverX >= left && g_state.chartHoverX <= right) {
            float hx = (float)g_state.chartHoverX;
            heliosview_painter_set_stroke(p, 0x808AADF4, 1.2f);
            heliosview_painter_draw_line(p, hx, top, hx, bottom);

            // Interpolated value at crosshair
            float frac = (hx - left) / plotW;
            float t = g_state.timeSec * 2.0f + frac * (numPts - 1) * 0.12f;
            float val = 0.45f + 0.28f * std::sin(t) + 0.12f * std::cos(t * 2.3f + 1.2f);
            val = std::clamp(val, 0.05f, 0.95f);
            float hy = bottom - val * plotH;

            // Highlight point
            heliosview_painter_set_fill(p, 0xFFFFFFFF);
            heliosview_painter_set_stroke(p, 0xFFA6E3A1, 2.5f);
            heliosview_painter_draw_ellipse(p, hx - 5.0f, hy - 5.0f, 10.0f, 10.0f);

            // Tooltip box
            char tip[64];
            std::snprintf(tip, sizeof(tip), "CH01 Load: %.1f%%", val * 100.0f);
            float tipW = 120.0f, tipH = 26.0f;
            float tipX = (hx + 10.0f + tipW > right) ? (hx - tipW - 10.0f) : (hx + 10.0f);
            float tipY = std::clamp(hy - 30.0f, top, bottom - tipH);

            heliosview_painter_set_fill(p, 0xEE181926);
            heliosview_painter_set_stroke(p, 0xFF8AADF4, 1.0f);
            heliosview_painter_draw_round_rect(p, tipX, tipY, tipW, tipH, 4.0f);

            heliosview_font_desc_t fTip{"Segoe UI", 11.0f, HELIOSVIEW_FONT_BOLD};
            heliosview_painter_set_font(p, &fTip);
            heliosview_painter_set_fill(p, 0xFFCDD6F4);
            heliosview_painter_draw_text(p, tip, tipX + 8.0f, tipY + 6.0f);
        }
    });

    lineChart->setMouse([](CustomWidget* w, const heliosview_host_mouse_event_t* e) {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            g_state.chartHovered = true;
            g_state.chartHoverX = e->x;
            g_state.chartHoverY = e->y;
            w->requestRepaint();
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            g_state.chartHovered = false;
            w->requestRepaint();
            return true;
        }
        return false;
    });

    lineChartCard->addChild(lineChart);
    tabRoot->add(lineChartCard);

    // Bottom Row: Bar Chart (Left) + Multi-Donut Gauge (Right)
    auto bottomRow = HStack::create(16, 0);

    // ---- Bar Chart ----
    auto barCard = Card::create(470, 244, 0xFF1E1E2E, 0xFF313244);
    auto barWidget = CustomWidget::create();
    barWidget->setSize(470, 244);

    barWidget->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        int bw = 0, bh = 0;
        heliosview_ui_widget_get_bounds(w->handle(), nullptr, nullptr, &bw, &bh);

        heliosview_font_desc_t fontH{"Segoe UI", 13.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFFF5BDE6);
        heliosview_painter_draw_text(p, "Weekly Network Throughput (GB)", 18.0f, 14.0f);

        const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        float vals[] = {4.2f, 7.8f, 5.4f, 9.2f, 8.5f, 3.1f, 6.4f};
        float maxVal = 10.0f;

        float startX = 36.0f;
        float startY = (float)bh - 42.0f;
        float chartH = 140.0f;
        float slotW = ((float)bw - startX - 24.0f) / 7.0f;
        float barWidth = 32.0f;

        heliosview_font_desc_t fontDay{"Segoe UI", 11.0f, 0};
        heliosview_painter_set_font(p, &fontDay);

        for (int i = 0; i < 7; ++i) {
            float x = startX + i * slotW + (slotW - barWidth) / 2.0f;
            float h = (vals[i] / maxVal) * chartH;
            float y = startY - h;

            bool hovered = (g_state.barHoveredIndex == i);
            uint32_t fill = hovered ? 0xFF8AADF4 : 0xFF585B70;
            if (i == 3) fill = hovered ? 0xFFF5BDE6 : 0xFFCBA6F7; // Peak highlight

            heliosview_painter_set_fill(p, fill);
            heliosview_painter_set_stroke(p, 0, 0);
            heliosview_painter_draw_round_rect(p, x, y, barWidth, h, 4.0f);

            // Day label
            heliosview_painter_set_fill(p, hovered ? 0xFFFFFFFF : 0xFFA6ADC8);
            heliosview_painter_draw_text(p, days[i], x + 4.0f, startY + 8.0f);

            // Value on top if hovered
            if (hovered) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.1fG", vals[i]);
                heliosview_painter_set_fill(p, 0xFFCAD3F5);
                heliosview_painter_draw_text(p, buf, x - 2.0f, y - 16.0f);
            }
        }
    });

    barWidget->setMouse([](CustomWidget* w, const heliosview_host_mouse_event_t* e) {
        float startX = 36.0f;
        float slotW = (470.0f - startX - 24.0f) / 7.0f;
        int idx = (int)((e->x - startX) / slotW);

        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE && idx >= 0 && idx < 7) {
            if (g_state.barHoveredIndex != idx) {
                g_state.barHoveredIndex = idx;
                w->requestRepaint();
            }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            g_state.barHoveredIndex = -1;
            w->requestRepaint();
            return true;
        }
        return false;
    });

    barCard->addChild(barWidget);
    bottomRow->add(barCard);

    // ---- Multi-Donut Gauge ----
    auto donutCard = Card::create(470, 244, 0xFF1E1E2E, 0xFF313244);
    auto donutWidget = CustomWidget::create();
    donutWidget->setSize(470, 244);

    donutWidget->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        int dw = 0, dh = 0;
        heliosview_ui_widget_get_bounds(w->handle(), nullptr, nullptr, &dw, &dh);

        heliosview_font_desc_t fontH{"Segoe UI", 13.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFFA6E3A1);
        heliosview_painter_draw_text(p, "System Quota Allocation (Nested Rings)", 18.0f, 14.0f);

        float cx = 130.0f;
        float cy = (float)dh / 2.0f + 10.0f;

        // Outer Ring: Disk 74%
        float r1 = 68.0f;
        heliosview_painter_set_fill(p, 0);
        heliosview_painter_set_stroke(p, 0xFF313244, 8.0f);
        heliosview_painter_draw_ellipse(p, cx - r1, cy - r1, r1 * 2, r1 * 2);
        heliosview_painter_set_stroke(p, 0xFF8AADF4, 8.0f);
        heliosview_painter_draw_arc(p, cx - r1, cy - r1, r1 * 2, r1 * 2, -90.0f, 0.74f * 360.0f);

        // Middle Ring: Memory 58%
        float r2 = 52.0f;
        heliosview_painter_set_stroke(p, 0xFF313244, 8.0f);
        heliosview_painter_draw_ellipse(p, cx - r2, cy - r2, r2 * 2, r2 * 2);
        heliosview_painter_set_stroke(p, 0xFFCBA6F7, 8.0f);
        heliosview_painter_draw_arc(p, cx - r2, cy - r2, r2 * 2, r2 * 2, -90.0f, 0.58f * 360.0f);

        // Inner Ring: GPU VRAM 82%
        float r3 = 36.0f;
        heliosview_painter_set_stroke(p, 0xFF313244, 8.0f);
        heliosview_painter_draw_ellipse(p, cx - r3, cy - r3, r3 * 2, r3 * 2);
        heliosview_painter_set_stroke(p, 0xFFA6E3A1, 8.0f);
        heliosview_painter_draw_arc(p, cx - r3, cy - r3, r3 * 2, r3 * 2, -90.0f, 0.82f * 360.0f);

        // Center Score
        heliosview_font_desc_t fScore{"Segoe UI", 15.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fScore);
        heliosview_painter_set_fill(p, 0xFFCAD3F5);
        heliosview_painter_draw_text(p, "71%", cx - 14.0f, cy - 8.0f);

        // Legend on Right
        float lx = 240.0f;
        float ly = 60.0f;
        auto drawLegend = [p, &lx, &ly](uint32_t col, const char* name, const char* pct) {
            heliosview_painter_set_fill(p, col);
            heliosview_painter_set_stroke(p, 0, 0);
            heliosview_painter_draw_round_rect(p, lx, ly, 12.0f, 12.0f, 3.0f);

            heliosview_font_desc_t f{"Segoe UI", 12.0f, 0};
            heliosview_painter_set_font(p, &f);
            heliosview_painter_set_fill(p, 0xFFCDD6F4);
            heliosview_painter_draw_text(p, name, lx + 20.0f, ly);

            heliosview_font_desc_t fBold{"Segoe UI", 12.0f, HELIOSVIEW_FONT_BOLD};
            heliosview_painter_set_font(p, &fBold);
            heliosview_painter_set_fill(p, col);
            heliosview_painter_draw_text(p, pct, lx + 140.0f, ly);
            ly += 32.0f;
        };

        drawLegend(0xFF8AADF4, "NVMe Storage", "74%");
        drawLegend(0xFFCBA6F7, "Unified RAM", "58%");
        drawLegend(0xFFA6E3A1, "Dedicated GPU", "82%");
    });

    donutCard->addChild(donutWidget);
    bottomRow->add(donutCard);

    tabRoot->add(bottomRow);
    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 2: Vector Graphics & Paths View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabVectors() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: Complex Geometric Paths & Winding ----
    auto pathCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto pathWidget = CustomWidget::create();
    pathWidget->setSize(470, 520);

    pathWidget->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        heliosview_font_desc_t fontH{"Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFFF9E2AF);
        heliosview_painter_draw_text(p, "Complex 2D Paths, Stars & Bezier Curves", 18.0f, 16.0f);

        // 1. Golden 5-Point Star
        DrawStar(p, 110.0f, 110.0f, 60.0f, 26.0f, 5, 0xFFF9E2AF, 0xFFFAB387);

        // 2. 8-Point Cyan Radiant Star
        DrawStar(p, 340.0f, 110.0f, 62.0f, 32.0f, 8, 0xFF89DCEB, 0xFF74C7EC);

        // Labels
        heliosview_font_desc_t fontL{"Segoe UI", 11.0f, 0};
        heliosview_painter_set_font(p, &fontL);
        heliosview_painter_set_fill(p, 0xFFA6ADC8);
        heliosview_painter_draw_text(p, "5-Point Vector Polygon", 44.0f, 185.0f);
        heliosview_painter_draw_text(p, "8-Point Radiant Compass", 270.0f, 185.0f);

        // Separator
        heliosview_painter_set_fill(p, 0xFF313244);
        heliosview_painter_draw_rect(p, 20.0f, 210.0f, 430.0f, 1.0f);

        // 3. Smooth Cubic Bezier Ribbon
        heliosview_font_desc_t fontSub{"Segoe UI", 12.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontSub);
        heliosview_painter_set_fill(p, 0xFFCBA6F7);
        heliosview_painter_draw_text(p, "Dynamic Cubic Bezier Waveform", 20.0f, 226.0f);

        float bx0 = 30.0f, by0 = 340.0f;
        float bx3 = 440.0f, by3 = 340.0f;
        float bx1 = 150.0f, by1 = 250.0f + std::sin(g_state.timeSec * 3.0f) * 60.0f;
        float bx2 = 320.0f, by2 = 430.0f - std::sin(g_state.timeSec * 3.0f) * 60.0f;

        // Draw control lines
        heliosview_painter_set_stroke(p, 0xFF45475A, 1.0f);
        heliosview_painter_draw_line(p, bx0, by0, bx1, by1);
        heliosview_painter_draw_line(p, bx3, by3, bx2, by2);

        // Control point handles
        heliosview_painter_set_fill(p, 0xFFF38BA8);
        heliosview_painter_draw_ellipse(p, bx1 - 4.0f, by1 - 4.0f, 8.0f, 8.0f);
        heliosview_painter_draw_ellipse(p, bx2 - 4.0f, by2 - 4.0f, 8.0f, 8.0f);

        // Approximate cubic bezier curve with polyline
        const int steps = 40;
        std::vector<float> bPts;
        bPts.reserve((steps + 1) * 2);
        for (int i = 0; i <= steps; ++i) {
            float u = (float)i / steps;
            float inv = 1.0f - u;
            float px = inv * inv * inv * bx0 + 3.0f * inv * inv * u * bx1 + 3.0f * inv * u * u * bx2 + u * u * u * bx3;
            float py = inv * inv * inv * by0 + 3.0f * inv * inv * u * by1 + 3.0f * inv * u * u * bx2 + u * u * u * by3;
            bPts.push_back(px);
            bPts.push_back(py);
        }
        heliosview_painter_set_stroke(p, 0xFFF5C2E7, 3.0f);
        heliosview_painter_draw_polyline(p, bPts.data(), bPts.size() / 2, 0);

        // 4. Concentric Nested Polygons (Hexagon)
        float hx = 235.0f, hy = 445.0f;
        for (int r = 16; r <= 48; r += 16) {
            std::vector<float> hex;
            for (int k = 0; k < 6; ++k) {
                float a = k * (kPi / 3.0f) + g_state.timeSec * 0.5f;
                hex.push_back(hx + std::cos(a) * r);
                hex.push_back(hy + std::sin(a) * r);
            }
            heliosview_painter_set_fill(p, 0);
            heliosview_painter_set_stroke(p, (r == 48) ? 0xFF8AADF4 : 0xFFCBA6F7, 1.5f);
            heliosview_painter_draw_polygon(p, hex.data(), 6);
        }
    });

    pathCard->addChild(pathWidget);
    tabRoot->add(pathCard);

    // ---- Right Card: Affine Transforms & Intermeshing Gears ----
    auto gearCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto gearWidget = CustomWidget::create();
    gearWidget->setSize(470, 520);

    gearWidget->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        heliosview_font_desc_t fontH{"Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFF8AADF4);
        heliosview_painter_draw_text(p, "Mechanical Gear Train (Affine Transforms)", 18.0f, 16.0f);

        heliosview_font_desc_t fontSub{"Segoe UI", 11.0f, 0};
        heliosview_painter_set_font(p, &fontSub);
        heliosview_painter_set_fill(p, 0xFFA6ADC8);
        heliosview_painter_draw_text(p, "Real-time affine matrix rotation with synchronized gear ratios", 18.0f, 38.0f);

        // Main drive gear (18 teeth, Radius 80)
        float cx1 = 175.0f, cy1 = 200.0f, r1 = 80.0f;
        int t1 = 18;
        DrawGear(p, cx1, cy1, r1, t1, g_state.gearAngle, 0xFF8AADF4);

        // Secondary driven gear (12 teeth, Radius 53.33)
        // Ratio = 18/12 = 1.5x speed in opposite direction
        float r2 = r1 * (12.0f / 18.0f);
        float dist = r1 + r2 + 6.0f;
        float anglePos = kPi * 0.15f;
        float cx2 = cx1 + std::cos(anglePos) * dist;
        float cy2 = cy1 + std::sin(anglePos) * dist;
        DrawGear(p, cx2, cy2, r2, 12, -g_state.gearAngle * 1.5f + 0.2f, 0xFFA6E3A1);

        // Third small pinion gear (9 teeth, Radius 40)
        // Ratio = 18/9 = 2.0x speed
        float r3 = r1 * (9.0f / 18.0f);
        float dist3 = r1 + r3 + 6.0f;
        float anglePos3 = -kPi * 0.55f;
        float cx3 = cx1 + std::cos(anglePos3) * dist3;
        float cy3 = cy1 + std::sin(anglePos3) * dist3;
        DrawGear(p, cx3, cy3, r3, 9, -g_state.gearAngle * 2.0f, 0xFFF5BDE6);

        // Bottom Info Card
        float infoY = 380.0f;
        heliosview_painter_set_fill(p, 0xFF181926);
        heliosview_painter_set_stroke(p, 0xFF313244, 1.0f);
        heliosview_painter_draw_round_rect(p, 20.0f, infoY, 430.0f, 110.0f, 8.0f);

        heliosview_font_desc_t fBold{"Segoe UI", 12.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fBold);
        heliosview_painter_set_fill(p, 0xFFCAD3F5);
        heliosview_painter_draw_text(p, "Kinematic Drive Metrics:", 34.0f, infoY + 16.0f);

        heliosview_font_desc_t fInfo{"Segoe UI", 11.0f, 0};
        heliosview_painter_set_font(p, &fInfo);
        heliosview_painter_set_fill(p, 0xFFA6ADC8);
        heliosview_painter_draw_text(p, "> Primary Drive: 18T @ 1.0x Angular Velocity", 34.0f, infoY + 40.0f);
        heliosview_painter_draw_text(p, "> Intermediate Planet: 12T @ -1.5x Meshed Velocity", 34.0f, infoY + 60.0f);
        heliosview_painter_draw_text(p, "> High-Speed Pinion: 9T @ -2.0x Overdrive", 34.0f, infoY + 80.0f);
    });

    gearCard->addChild(gearWidget);
    tabRoot->add(gearCard);

    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 3: Gauges & Oscilloscope View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabGauges() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: Analog Tachometer / Speedometer ----
    auto tachoCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto tachoWidget = CustomWidget::create();
    tachoWidget->setSize(470, 520);

    tachoWidget->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        heliosview_font_desc_t fontH{"Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFFF38BA8);
        heliosview_painter_draw_text(p, "Analog Precision Speedometer / Tachometer", 18.0f, 16.0f);

        float cx = 235.0f;
        float cy = 250.0f;
        float r = 160.0f;

        // Dial Bezel (Gradient-like concentric rings)
        heliosview_painter_set_fill(p, 0xFF181926);
        heliosview_painter_set_stroke(p, 0xFF45475A, 3.0f);
        heliosview_painter_draw_ellipse(p, cx - r, cy - r, r * 2.0f, r * 2.0f);

        // Color Arc Zones:
        // Semicircular range: 135 deg to 405 deg (270 deg span)
        float startAngle = 135.0f;
        // Green zone (0 - 80 km/h) -> 135 to 270 deg (135 deg span)
        heliosview_painter_set_fill(p, 0);
        heliosview_painter_set_stroke(p, 0xFFA6E3A1, 6.0f);
        heliosview_painter_draw_arc(p, cx - r + 14.0f, cy - r + 14.0f, (r - 14.0f) * 2.0f, (r - 14.0f) * 2.0f, 135.0f, 135.0f);

        // Amber zone (80 - 120 km/h) -> 270 to 337.5 deg (67.5 deg span)
        heliosview_painter_set_stroke(p, 0xFFF9E2AF, 6.0f);
        heliosview_painter_draw_arc(p, cx - r + 14.0f, cy - r + 14.0f, (r - 14.0f) * 2.0f, (r - 14.0f) * 2.0f, 270.0f, 67.5f);

        // Red danger zone (120 - 160 km/h) -> 337.5 to 405 deg (67.5 deg span)
        heliosview_painter_set_stroke(p, 0xFFF38BA8, 6.0f);
        heliosview_painter_draw_arc(p, cx - r + 14.0f, cy - r + 14.0f, (r - 14.0f) * 2.0f, (r - 14.0f) * 2.0f, 337.5f, 67.5f);

        // Major and Minor Tick Marks
        heliosview_font_desc_t fontTick{"Segoe UI", 11.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontTick);

        for (int val = 0; val <= 160; val += 20) {
            float frac = (float)val / 160.0f;
            float deg = startAngle + frac * 270.0f;
            float rad = deg * (kPi / 180.0f);

            float cosA = std::cos(rad);
            float sinA = std::sin(rad);

            float x1 = cx + cosA * (r - 28.0f);
            float y1 = cy + sinA * (r - 28.0f);
            float x2 = cx + cosA * (r - 16.0f);
            float y2 = cy + sinA * (r - 16.0f);

            heliosview_painter_set_stroke(p, (val >= 120) ? 0xFFF38BA8 : 0xFFCAD3F5, 2.5f);
            heliosview_painter_draw_line(p, x1, y1, x2, y2);

            // Number Label
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", val);
            float lx = cx + cosA * (r - 46.0f) - 8.0f;
            float ly = cy + sinA * (r - 46.0f) - 6.0f;
            heliosview_painter_set_fill(p, 0xFFA6ADC8);
            heliosview_painter_draw_text(p, buf, lx, ly);
        }

        // Animated Needle with Spring Physics Simulation
        float targetVal = 70.0f + 55.0f * std::sin(g_state.timeSec * 1.8f) + 12.0f * std::cos(g_state.timeSec * 4.2f);
        targetVal = std::clamp(targetVal, 0.0f, 160.0f);
        g_state.tachometerVal += (targetVal - g_state.tachometerVal) * 0.15f;

        float needleFrac = g_state.tachometerVal / 160.0f;
        float needleDeg = startAngle + needleFrac * 270.0f;
        float needleRad = needleDeg * (kPi / 180.0f);

        float nx = cx + std::cos(needleRad) * (r - 26.0f);
        float ny = cy + std::sin(needleRad) * (r - 26.0f);
        float tailX = cx - std::cos(needleRad) * 22.0f;
        float tailY = cy - std::sin(needleRad) * 22.0f;

        heliosview_painter_set_stroke(p, 0xFFF38BA8, 3.5f);
        heliosview_painter_draw_line(p, tailX, tailY, nx, ny);

        // Center Pivot Hub
        heliosview_painter_set_fill(p, 0xFF313244);
        heliosview_painter_set_stroke(p, 0xFFCAD3F5, 2.0f);
        heliosview_painter_draw_ellipse(p, cx - 14.0f, cy - 14.0f, 28.0f, 28.0f);

        // Digital Readout Display
        heliosview_painter_set_fill(p, 0xFF11111B);
        heliosview_painter_set_stroke(p, 0xFF313244, 1.0f);
        heliosview_painter_draw_round_rect(p, cx - 75.0f, cy + 60.0f, 150.0f, 48.0f, 8.0f);

        char digBuf[32];
        std::snprintf(digBuf, sizeof(digBuf), "%.1f", g_state.tachometerVal);
        heliosview_font_desc_t fDigital{"Segoe UI", 20.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fDigital);
        heliosview_painter_set_fill(p, 0xFFF38BA8);

        heliosview_text_metrics_t dm{};
        heliosview_painter_measure_text(p, digBuf, &dm);
        heliosview_painter_draw_text(p, digBuf, cx - dm.width / 2.0f - 18.0f, cy + 72.0f);

        heliosview_font_desc_t fKm{"Segoe UI", 11.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fKm);
        heliosview_painter_set_fill(p, 0xFF6E738D);
        heliosview_painter_draw_text(p, "KM/H", cx + 22.0f, cy + 78.0f);
    });

    tachoCard->addChild(tachoWidget);
    tabRoot->add(tachoCard);

    // ---- Right Card: Dual-Channel Digital Oscilloscope ----
    auto oscCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto oscWidget = CustomWidget::create();
    oscWidget->setSize(470, 520);

    oscWidget->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        heliosview_font_desc_t fontH{"Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontH);
        heliosview_painter_set_fill(p, 0xFFA6E3A1);
        heliosview_painter_draw_text(p, "Dual-Channel Live CRT Oscilloscope (60FPS)", 18.0f, 16.0f);

        float scrX = 20.0f;
        float scrY = 48.0f;
        float scrW = 430.0f;
        float scrH = 340.0f;

        // CRT Screen Frame
        heliosview_painter_set_fill(p, 0xFF0D1117);
        heliosview_painter_set_stroke(p, 0xFF238636, 1.5f);
        heliosview_painter_draw_round_rect(p, scrX, scrY, scrW, scrH, 10.0f);

        // Screen Grid (Phosphor Grid)
        heliosview_painter_save(p);
        heliosview_painter_set_clip_rect(p, scrX, scrY, scrW, scrH);

        // Grid lines
        heliosview_painter_set_stroke(p, 0x30238636, 1.0f);
        for (float x = scrX; x < scrX + scrW; x += 35.0f) {
            heliosview_painter_draw_line(p, x, scrY, x, scrY + scrH);
        }
        for (float y = scrY; y < scrY + scrH; y += 35.0f) {
            heliosview_painter_draw_line(p, scrX, y, scrX + scrW, y);
        }

        // Center crosshairs
        float midX = scrX + scrW / 2.0f;
        float midY = scrY + scrH / 2.0f;
        heliosview_painter_set_stroke(p, 0x60238636, 1.5f);
        heliosview_painter_draw_line(p, scrX, midY, scrX + scrW, midY);
        heliosview_painter_draw_line(p, midX, scrY, midX, scrY + scrH);

        // Channel 1: High-Frequency Sine Carrier (Emerald Green Glow)
        const int samples = 140;
        std::vector<float> ch1Pts;
        ch1Pts.reserve(samples * 2);
        for (int i = 0; i < samples; ++i) {
            float x = scrX + (float)i / (samples - 1) * scrW;
            float t = (float)i * 0.08f + g_state.timeSec * 8.0f;
            float y = midY - 30.0f + std::sin(t) * 45.0f;
            ch1Pts.push_back(x);
            ch1Pts.push_back(y);
        }
        heliosview_painter_set_stroke(p, 0x40A6E3A1, 5.0f); // Bloom glow
        heliosview_painter_draw_polyline(p, ch1Pts.data(), samples, 0);
        heliosview_painter_set_stroke(p, 0xFFA6E3A1, 1.8f); // Sharp core
        heliosview_painter_draw_polyline(p, ch1Pts.data(), samples, 0);

        // Channel 2: Modulated Low-Frequency Signal (Cyan Glow)
        std::vector<float> ch2Pts;
        ch2Pts.reserve(samples * 2);
        for (int i = 0; i < samples; ++i) {
            float x = scrX + (float)i / (samples - 1) * scrW;
            float t = (float)i * 0.04f + g_state.timeSec * 4.0f;
            float env = std::sin(t * 0.5f);
            float y = midY + 40.0f + std::sin(t * 2.5f) * env * 55.0f;
            ch2Pts.push_back(x);
            ch2Pts.push_back(y);
        }
        heliosview_painter_set_stroke(p, 0x4089DCEB, 5.0f); // Bloom glow
        heliosview_painter_draw_polyline(p, ch2Pts.data(), samples, 0);
        heliosview_painter_set_stroke(p, 0xFF89DCEB, 1.8f); // Sharp core
        heliosview_painter_draw_polyline(p, ch2Pts.data(), samples, 0);

        heliosview_painter_restore(p);

        // Channel Status Badges
        float badgeY = scrY + scrH + 18.0f;
        auto drawChBadge = [p, badgeY](float bx, const char* name, const char* spec, uint32_t col) {
            heliosview_painter_set_fill(p, 0xFF181926);
            heliosview_painter_set_stroke(p, col, 1.0f);
            heliosview_painter_draw_round_rect(p, bx, badgeY, 195.0f, 75.0f, 8.0f);

            heliosview_font_desc_t fB{"Segoe UI", 12.0f, HELIOSVIEW_FONT_BOLD};
            heliosview_painter_set_font(p, &fB);
            heliosview_painter_set_fill(p, col);
            heliosview_painter_draw_text(p, name, bx + 14.0f, badgeY + 12.0f);

            heliosview_font_desc_t fS{"Segoe UI", 10.0f, 0};
            heliosview_painter_set_font(p, &fS);
            heliosview_painter_set_fill(p, 0xFFA6ADC8);
            heliosview_painter_draw_text(p, spec, bx + 14.0f, badgeY + 36.0f);
            heliosview_painter_draw_text(p, "Probe: 1X | Coupling: DC", bx + 14.0f, badgeY + 52.0f);
        };

        drawChBadge(scrX + 10.0f, "CH 1: 50.0 mV/div", "Sine wave @ 2.45 kHz", 0xFFA6E3A1);
        drawChBadge(scrX + 225.0f, "CH 2: 100 mV/div", "AM Modulated @ 680 Hz", 0xFF89DCEB);
    });

    oscCard->addChild(oscWidget);
    tabRoot->add(oscCard);

    return tabRoot;
}

} // namespace

// ----------------------------------------------------------------------------
// Main Application Context & Window Host Wiring
// ----------------------------------------------------------------------------
int main() {
    std::cout << "===========================================================\n";
    std::cout << " HeliosView 2D DirectDraw & UI Component Suite Showcase\n";
    std::cout << "===========================================================\n";

    heliosview_window_t* win = heliosview_window_create(1020, 800, "HeliosView 2D DirectDraw & Retained UI Suite");
    if (!win) return 1;

    // DirectDraw UIHost with Blend2D Engine
    heliosview_host_t* host = heliosview_host_create_ui(win, 0, 0, 1020, 800, HELIOSVIEW_ENGINE_BLEND2D);
    if (!host) return 1;

    // Root UI Tree (VStack)
    auto rootStack = VStack::create(16, 24);

    // Header Title & Engine Badge
    auto headerRow = HStack::create(16, 0);

    auto titleLbl = Label::create("HeliosView UI Gallery");
    titleLbl->setFontSize(22.0f);
    titleLbl->setColor(0xFFCAD3F5);
    headerRow->add(titleLbl);

    auto engineBadge = CustomWidget::create();
    engineBadge->setSize(260, 32);
    engineBadge->setPaint([](CustomWidget* w, heliosview_painter_t* p) {
        heliosview_painter_set_fill(p, 0xFF181926);
        heliosview_painter_set_stroke(p, 0xFF8AADF4, 1.0f);
        heliosview_painter_draw_round_rect(p, 0, 0, 260.0f, 32.0f, 6.0f);

        heliosview_font_desc_t font{"Segoe UI", 11.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &font);
        heliosview_painter_set_fill(p, 0xFF8AADF4);
        heliosview_painter_draw_text(p, "ENGINE: BLEND2D (JIT X86_64) | 60 FPS", 16.0f, 8.0f);
    });
    headerRow->add(engineBadge);
    rootStack->add(headerRow);

    // Segmented Tab Selector
    std::vector<std::string> tabNames = {
        "Controls & Forms",
        "Charts & Analytics",
        "Vector & Paths",
        "Gauges & Waves"
    };
    auto tabBar = SegmentedControl::create(tabNames, 0);
    tabBar->setSize(964, 40);
    rootStack->add(tabBar);

    // Tab Views
    auto tab0 = CreateTabControls();
    auto tab1 = CreateTabCharts();
    auto tab2 = CreateTabVectors();
    auto tab3 = CreateTabGauges();

    tab1->setVisible(false);
    tab2->setVisible(false);
    tab3->setVisible(false);

    rootStack->add(tab0);
    rootStack->add(tab1);
    rootStack->add(tab2);
    rootStack->add(tab3);

    // Tab Switching Logic
    tabBar->onChange([tab0, tab1, tab2, tab3, host](int idx) {
        g_state.activeTab = idx;
        tab0->setVisible(idx == 0);
        tab1->setVisible(idx == 1);
        tab2->setVisible(idx == 2);
        tab3->setVisible(idx == 3);
        heliosview_host_ui_request_repaint(host);
    });

    // Attach root to Host
    heliosview_host_ui_set_root(host, rootStack->handle());
    heliosview_window_show(win);

    std::cout << "[UI] Gallery initialized with 4 interactive tabs.\n";

    // Main 60 FPS Animation & Event Loop
    struct LoopCtx {
        heliosview_host_t* host;
        std::shared_ptr<VStack> root;
    } loopCtx{host, rootStack};

    heliosview_run([](void* udata) -> int {
        auto* ctx = static_cast<LoopCtx*>(udata);
        g_state.timeSec += 0.016f;
        g_state.gearAngle += 0.035f;

        // Continuous 60 FPS repaint for animations & live gauges
        heliosview_host_ui_request_repaint(ctx->host);

        heliosview_event_t ev;
        while (heliosview_poll(&ev)) {
            if (ev.type == HELIOSVIEW_EVENT_WINDOW_RESIZE) {
                heliosview_host_set_bounds(ctx->host, 0, 0, ev.width, ev.height);
            } else if (ev.type == HELIOSVIEW_EVENT_WINDOW_CLOSE) {
                heliosview_quit();
                return 1;
            }
        }
        return 0;
    }, &loopCtx);

    // Teardown
    rootStack.reset();
    tab0.reset();
    tab1.reset();
    tab2.reset();
    tab3.reset();

    if (host) heliosview_host_destroy(host);
    if (win) heliosview_window_destroy(win);

    return 0;
}
