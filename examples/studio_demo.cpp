// HeliosView Comprehensive Studio Showcase Demo
// Combines:
//   1. Pure Window shell (1280x780, adaptive split-view)
//   2. Left Host: UIHost with full retained UI system
//      - Tier 1: Pure C ABI Circle Gauge
//      - Tier 2: C++ OOP CustomCard
//      - Tier 3: In-line Lambda Live Waveform
//      - Engine switching (Blend2D / GDI+ / D2D)
//      - Interactive buttons with animated states & counters
//   3. Right Host: WebViewHost (Chromium / WebView2)
//      - Rich HTML5 dashboard with CSS cards
//      - Two-way bridge communication (Native -> Web, Web -> Native)

#include <HeliosView/heliosview.h>
#include <HeliosViewCore/UI/Widget.h>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace HeliosView::UI;

// ================= Global Application State =================
struct StudioApp {
    heliosview_window_t* window = nullptr;
    heliosview_host_t* ui_host = nullptr;
    heliosview_host_t* web_host = nullptr;

    // Simulation metrics
    float wave_phase = 0.0f;
    float gauge_val = 0.72f;
    int click_count = 0;
    bool is_simulating = true;
    heliosview_canvas_engine_t current_engine = HELIOSVIEW_ENGINE_BLEND2D;

    // References to UI widgets for updates
    std::shared_ptr<CustomWidget> wave_widget;
    std::shared_ptr<Label> status_label;
    std::shared_ptr<Button> sim_btn;
};

static StudioApp g_app;

// ================= Tier 1: Pure C Custom Dial/Gauge =================
struct DialData {
    float* p_val = nullptr;
    bool is_hovered = false;
};

static void DialPaint(heliosview_ui_widget_t* w, heliosview_painter_t* p, void* udata) {
    auto* data = static_cast<DialData*>(udata);
    int width = 0, height = 0;
    heliosview_ui_widget_get_bounds(w, nullptr, nullptr, &width, &height);

    float cx = width / 2.0f;
    float cy = height / 2.0f;
    float r = std::min(width, height) / 2.0f - 8.0f;

    // Background track
    heliosview_painter_set_fill(p, 0);
    heliosview_painter_set_stroke(p, 0xFF313244, 8.0f);
    heliosview_painter_draw_ellipse(p, cx - r, cy - r, r * 2, r * 2);

    // Active progress arc
    float val = data->p_val ? *data->p_val : 0.5f;
    uint32_t arc_color = data->is_hovered ? 0xFFF5BDE6 : 0xFF8AADF4;
    heliosview_painter_set_stroke(p, arc_color, 8.0f);
    heliosview_painter_draw_arc(p, cx - r, cy - r, r * 2, r * 2, -90.0f, val * 360.0f);

    // Center percentage text
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", (int)(val * 100.0f));
    heliosview_font_desc_t font{"Segoe UI", 15.0f, HELIOSVIEW_FONT_BOLD};
    heliosview_painter_set_font(p, &font);
    heliosview_painter_set_fill(p, 0xFFCAD3F5);

    heliosview_text_metrics_t m{};
    heliosview_painter_measure_text(p, buf, &m);
    heliosview_painter_draw_text(p, buf, cx - m.width / 2.0f, cy - m.height / 2.0f);
}

static int DialEvent(heliosview_ui_widget_t* w, const heliosview_host_mouse_event_t* e, void* udata) {
    auto* data = static_cast<DialData*>(udata);
    if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
        if (!data->is_hovered) {
            data->is_hovered = true;
            heliosview_ui_widget_request_repaint(w);
        }
        return 1;
    } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
        if (data->is_hovered) {
            data->is_hovered = false;
            heliosview_ui_widget_request_repaint(w);
        }
        return 1;
    } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN) {
        if (data->p_val) {
            *data->p_val += 0.08f;
            if (*data->p_val > 1.0f) *data->p_val = 0.05f;
            heliosview_ui_widget_request_repaint(w);

            // Sync to WebView
            heliosview_webview_t* wv = heliosview_host_get_webview(g_app.web_host);
            if (wv) {
                char js[128];
                std::snprintf(js, sizeof(js), "updateGauge(%d);", (int)(*data->p_val * 100));
                heliosview_webview_eval(wv, js);
            }
        }
        return 1;
    }
    return 0;
}

static void DialDestroy(void* udata) {
    delete static_cast<DialData*>(udata);
}

std::shared_ptr<Widget> CreatePureCDial(float* bind_val) {
    auto* data = new DialData();
    data->p_val = bind_val;

    heliosview_ui_widget_desc_t desc{};
    desc.paint = DialPaint;
    desc.event = DialEvent;
    desc.destroy = DialDestroy;

    auto* raw = heliosview_ui_widget_create(&desc, data);
    heliosview_ui_widget_set_bounds(raw, 0, 0, 95, 95);
    return std::make_shared<Widget>(raw);
}

// ================= Tier 2: C++ OOP Subclass Card =================
class MonitorCard : public Widget {
public:
    static std::shared_ptr<MonitorCard> create(std::string title, std::string desc) {
        auto card = std::make_shared<MonitorCard>();
        card->m_title = std::move(title);
        card->m_desc = std::move(desc);
        card->setSize(420, 90);
        return card;
    }

    void onPaint(heliosview_painter_t* p) override {
        int w = 0, h = 0;
        heliosview_ui_widget_get_bounds(m_handle, nullptr, nullptr, &w, &h);

        // Glassmorphism card background
        heliosview_painter_set_fill(p, m_isHovered ? 0xFF24273A : 0xFF1E1E2E);
        heliosview_painter_set_stroke(p, m_isHovered ? 0xFF8AADF4 : 0xFF363A4F, 1.2f);
        heliosview_painter_draw_round_rect(p, 0, 0, (float)w, (float)h, 10.0f);

        // Header text
        heliosview_font_desc_t fontT{"Segoe UI", 15.0f, HELIOSVIEW_FONT_BOLD};
        heliosview_painter_set_font(p, &fontT);
        heliosview_painter_set_fill(p, 0xFFF5BDE6);
        heliosview_painter_draw_text(p, m_title.c_str(), 16.0f, 16.0f);

        // Subtitle text
        heliosview_font_desc_t fontD{"Segoe UI", 12.0f, 0};
        heliosview_painter_set_font(p, &fontD);
        heliosview_painter_set_fill(p, 0xFFA5ADCB);
        heliosview_painter_draw_text(p, m_desc.c_str(), 16.0f, 42.0f);
    }

    bool onMouseEvent(const heliosview_host_mouse_event_t* e) override {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            if (!m_isHovered) { m_isHovered = true; requestRepaint(); }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            if (m_isHovered) { m_isHovered = false; requestRepaint(); }
            return true;
        }
        return false;
    }

private:
    std::string m_title;
    std::string m_desc;
    bool m_isHovered = false;
};

// ================= JS Bridge Callback (Web -> Native) =================
// Called when the user clicks buttons inside the WebView HTML page
static void OnWebMessage(heliosview_webview_t* wv, uint64_t call_id, const char*, const char* args_json, void*) {
    std::cout << "[Native Received from Web]: " << (args_json ? args_json : "") << std::endl;

    g_app.is_simulating = !g_app.is_simulating;
    if (g_app.sim_btn) {
        g_app.sim_btn->setLabel(g_app.is_simulating ? "Pause Simulator" : "Resume Simulator");
    }
    if (g_app.status_label) {
        g_app.status_label->setText(g_app.is_simulating ? "Engine: Running (Blend2D @ 60 FPS)" : "Engine: Paused");
    }
    heliosview_webview_resolve(wv, call_id, "true");
}

// ================= Native Layout Assembly =================
static std::shared_ptr<Widget> BuildNativeUI() {
    auto root = VStack::create(14, 20);

    // 1. Header & Title
    auto titleLabel = Label::create("HeliosView Studio Control");
    titleLabel->setFontSize(20.0f);
    titleLabel->setColor(0xFFCAD3F5);
    root->add(titleLabel);

    g_app.status_label = Label::create("Engine: Running (Blend2D @ 60 FPS)");
    g_app.status_label->setFontSize(13.0f);
    g_app.status_label->setColor(0xFFA6DA95);
    root->add(g_app.status_label);

    // 2. OOP Card
    auto card = MonitorCard::create(
        "Tier 2: System Telemetry Card",
        "Hardware-isolated viewport with retained sub-tree dispatching"
    );
    root->add(card);

    // 3. Middle Section: Dial (Pure C) + Live Waveform (Tier 3 Lambda)
    auto midRow = HStack::create(15, 0);

    // Tier 1 Dial
    auto dial = CreatePureCDial(&g_app.gauge_val);
    midRow->add(dial);

    // Tier 3 Live Waveform
    g_app.wave_widget = CustomWidget::create();
    g_app.wave_widget->setSize(310, 95);
    g_app.wave_widget->setPaint([](CustomWidget* self, heliosview_painter_t* p) {
        int w = 0, h = 0;
        heliosview_ui_widget_get_bounds(self->handle(), nullptr, nullptr, &w, &h);

        // Frame
        heliosview_painter_set_fill(p, 0xFF181926);
        heliosview_painter_set_stroke(p, 0xFF363A4F, 1.0f);
        heliosview_painter_draw_round_rect(p, 0, 0, (float)w, (float)h, 8.0f);

        // Grid lines
        heliosview_painter_set_stroke(p, 0xFF24273A, 1.0f);
        heliosview_painter_draw_line(p, 10, h / 2.0f, w - 10, h / 2.0f);

        // Sine waveform
        heliosview_painter_set_stroke(p, 0xFF8AADF4, 2.2f);
        float prev_x = 10, prev_y = h / 2.0f;
        for (float x = 10; x < w - 10; x += 4) {
            float y = h / 2.0f + std::sin((x * 0.06f) + g_app.wave_phase) * 28.0f;
            heliosview_painter_draw_line(p, prev_x, prev_y, x, y);
            prev_x = x;
            prev_y = y;
        }

        // Label
        heliosview_font_desc_t font{"Segoe UI", 10.0f, 0};
        heliosview_painter_set_font(p, &font);
        heliosview_painter_set_fill(p, 0xFF8087A2);
        heliosview_painter_draw_text(p, "LIVE SIGNAL CH-01", 14.0f, 8.0f);
    });

    g_app.wave_widget->setMouse([](CustomWidget*, const heliosview_host_mouse_event_t* e) {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            g_app.wave_phase += 0.25f;
            g_app.wave_widget->requestRepaint();
            return true;
        }
        return false;
    });

    midRow->add(g_app.wave_widget);
    root->add(midRow);

    // 4. Control Buttons (Row 1: Play/Pause Simulator, Trigger Events)
    auto btnRow1 = HStack::create(12, 0);

    g_app.sim_btn = Button::create("Pause Simulator");
    g_app.sim_btn->setSize(160, 40);
    g_app.sim_btn->onClick([]() {
        g_app.is_simulating = !g_app.is_simulating;
        g_app.sim_btn->setLabel(g_app.is_simulating ? "Pause Simulator" : "Resume Simulator");
        g_app.status_label->setText(g_app.is_simulating ? "Engine: Running (Blend2D @ 60 FPS)" : "Engine: Paused");
    });
    btnRow1->add(g_app.sim_btn);

    auto sendBtn = Button::create("Post Packet to Web");
    sendBtn->setSize(170, 40);
    sendBtn->onClick([]() {
        g_app.click_count++;
        heliosview_webview_t* wv = heliosview_host_get_webview(g_app.web_host);
        if (wv) {
            char js[128];
            std::snprintf(js, sizeof(js), "addLog('Native button clicked (Total: %d)');", g_app.click_count);
            heliosview_webview_eval(wv, js);
        }
    });
    btnRow1->add(sendBtn);

    root->add(btnRow1);

    return root;
}

// ================= Embedded HTML Page for WebView =================
static const char* kDashboardHtml = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>HeliosView WebView Dashboard</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background: #11111b;
            color: #cdd6f4;
            padding: 24px;
        }
        h2 { color: #f5c2e7; margin-bottom: 8px; }
        p.subtitle { color: #a6adc8; font-size: 13px; margin-bottom: 20px; }
        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 16px;
            margin-bottom: 20px;
        }
        .card {
            background: #1e1e2e;
            border: 1px solid #313244;
            border-radius: 12px;
            padding: 16px;
        }
        .metric-title { font-size: 12px; color: #9399b2; text-transform: uppercase; }
        .metric-value { font-size: 28px; font-weight: bold; color: #89b4fa; margin-top: 6px; }
        .progress-bar {
            width: 100%; height: 8px; background: #313244; border-radius: 4px; overflow: hidden; margin-top: 10px;
        }
        .progress-fill {
            height: 100%; width: 72%; background: linear-gradient(90deg, #89b4fa, #f5c2e7); transition: width 0.2s;
        }
        .console {
            background: #181825;
            border: 1px solid #313244;
            border-radius: 10px;
            padding: 12px;
            height: 180px;
            overflow-y: auto;
            font-family: Consolas, monospace;
            font-size: 12px;
            color: #a6e3a1;
        }
        .btn-web {
            background: #cba6f7;
            color: #11111b;
            border: none;
            padding: 10px 18px;
            border-radius: 8px;
            font-weight: bold;
            cursor: pointer;
            margin-top: 16px;
        }
        .btn-web:hover { background: #f5c2e7; }
    </style>
</head>
<body>
    <h2>Chromium Content Viewport</h2>
    <p class="subtitle">Hardware-isolated WebView2 host synchronized with native C++ pipeline</p>

    <div class="grid">
        <div class="card">
            <div class="metric-title">Native Dial Value</div>
            <div id="gaugeVal" class="metric-value">72%</div>
            <div class="progress-bar"><div id="gaugeBar" class="progress-fill"></div></div>
        </div>
        <div class="card">
            <div class="metric-title">IPC Status</div>
            <div class="metric-value" style="color:#a6e3a1;">CONNECTED</div>
            <div style="font-size:12px; color:#6c7086; margin-top:6px;">Zero-copy host separation</div>
        </div>
    </div>

    <div class="card">
        <div class="metric-title" style="margin-bottom:8px;">Live Telemetry Event Log</div>
        <div id="consoleLog" class="console">
            [System] WebView viewport initialized successfully.<br>
            [System] Listening to native UI events...<br>
        </div>
        <button class="btn-web" onclick="triggerNativeSim()">Trigger Native Simulator Toggle (Web -> Native)</button>
    </div>

    <script>
        function updateGauge(percent) {
            document.getElementById('gaugeVal').innerText = percent + '%';
            document.getElementById('gaugeBar').style.width = percent + '%';
            addLog('Native Dial changed -> ' + percent + '%');
        }

        function addLog(text) {
            const el = document.getElementById('consoleLog');
            el.innerHTML += '[Packet] ' + text + '<br>';
            el.scrollTop = el.scrollHeight;
        }

        function triggerNativeSim() {
            if (window.helios) {
                window.helios.call('toggle_sim', 'trigger');
            }
            addLog('Web invoked: toggle_sim via JS bridge');
        }
    </script>
</body>
</html>
)html";

// ================= Frame & Event Loop =================
static void UpdateLayout(int width, int height) {
    int split_x = 460;
    if (width < 600) split_x = width / 2;

    heliosview_host_set_bounds(g_app.ui_host, 0, 0, split_x, height);
    heliosview_host_set_bounds(g_app.web_host, split_x, 0, width - split_x, height);
}

static int FrameCallback(void*) {
    // 60 FPS continuous animation simulation
    if (g_app.is_simulating && g_app.wave_widget) {
        g_app.wave_phase += 0.08f;
        g_app.wave_widget->requestRepaint();
    }

    heliosview_event_t ev;
    while (heliosview_poll(&ev)) {
        if (ev.type == HELIOSVIEW_EVENT_WINDOW_RESIZE) {
            UpdateLayout(ev.width, ev.height);
        } else if (ev.type == HELIOSVIEW_EVENT_WINDOW_CLOSE) {
            heliosview_window_destroy(g_app.window);
            g_app.window = nullptr;
            heliosview_quit();
        }
    }
    return 0;
}

int main() {
    // 1. Create pure window shell (1280 x 780)
    g_app.window = heliosview_window_create(1280, 780, "HeliosView Studio - Hybrid Native UI & Web Platform");
    if (!g_app.window) {
        std::cerr << "Failed to create window!\n";
        return 1;
    }

    int split_x = 460;

    // 2. Left Host: UIHost (Retained native UI)
    g_app.ui_host = heliosview_host_create_ui(g_app.window, 0, 0, split_x, 780, HELIOSVIEW_ENGINE_BLEND2D);
    auto uiTree = BuildNativeUI();
    heliosview_host_ui_set_root(g_app.ui_host, uiTree->handle());

    // 3. Right Host: WebViewHost (Chromium / WebView2)
    g_app.web_host = heliosview_host_create_webview(g_app.window, split_x, 0, 1280 - split_x, 780);
    heliosview_webview_t* wv = heliosview_host_get_webview(g_app.web_host);
    if (wv) {
        heliosview_webview_navigate_html(wv, kDashboardHtml);
        // Bind native handler for JS -> Native communication
        heliosview_webview_bind(wv, "toggle_sim", OnWebMessage, nullptr, nullptr);
    }

    heliosview_window_show(g_app.window);

    // 4. Run loop
    heliosview_run(FrameCallback, nullptr);

    if (g_app.ui_host) heliosview_host_destroy(g_app.ui_host);
    if (g_app.web_host) heliosview_host_destroy(g_app.web_host);
    if (g_app.window) heliosview_window_destroy(g_app.window);

    return 0;
}
