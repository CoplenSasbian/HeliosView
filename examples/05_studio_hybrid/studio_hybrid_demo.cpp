// ============================================================================
// HeliosView Example 05: Studio Flagship Dual-Viewport Hybrid Platform
// ============================================================================
// Demonstrates HeliosView's killer architecture:
//   1. Single Window Host Shell (adaptive split-view layout)
//   2. Left Viewport: helios::UIHost (Retained 2D DirectDraw canvas via Blend2D)
//      - 60FPS continuous animated waveform
//      - Interactive circular gauge
//      - OOP telemetry status cards
//   3. Right Viewport: WebViewHost (Hardware-isolated Chromium / WebView2)
//      - Modern CSS3 / HTML5 dashboard
//   4. Zero-Copy Cross-Viewport Synchronization:
//      - Native Gauge Click -> Automatically updates WebView HTML metric bar
//      - Web Button Click -> Invokes C++ JS Bridge, toggling Native simulation state
//
// Written against the C++ wrappers: helios::Window / helios::App own the shell and
// the message loop, helios::UIHost the viewports, HeliosView::UI the widget tree,
// and helios::Painter every drawing call.
// ============================================================================

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

using namespace HeliosView::UI;

// ----------------------------------------------------------------------------
// Global Hybrid Application State
// ----------------------------------------------------------------------------
struct StudioApp {
    std::unique_ptr<helios::Window> window;   // native shell + message loop owner
    helios::UIHost ui_host;                   // left viewport: retained widget tree
    helios::UIHost web_host;                  // right viewport: embedded WebView2

    float wave_phase = 0.0f;
    float gauge_val = 0.72f;
    int click_count = 0;
    bool is_simulating = true;

    std::shared_ptr<CustomWidget> wave_widget;
    std::shared_ptr<Label> status_label;
    std::shared_ptr<Button> sim_btn;
};

static StudioApp g_app;

// ----------------------------------------------------------------------------
// Native DirectDraw Widgets (Left Viewport)
// ----------------------------------------------------------------------------

// Interactive circular gauge: a Widget subclass that draws through the C++ painter
// wrapper and exposes the bound value to the rest of the studio.
class DialWidget : public Widget {
public:
    DialWidget() = default;

    static std::shared_ptr<DialWidget> create(float* boundValue) {
        auto dial = std::make_shared<DialWidget>();
        dial->m_value = boundValue;
        dial->setSize(95, 95);
        return dial;
    }

    void onPaint(helios::Painter& p) override {
        const helios::Rect box = bounds();
        const float cx = (float)box.width / 2.0f;
        const float cy = (float)box.height / 2.0f;
        const float r = std::min(box.width, box.height) / 2.0f - 8.0f;

        // Track
        p.setFill(0);
        p.setStroke(0xFF313244, 8.0f);
        p.drawEllipse(cx - r, cy - r, r * 2, r * 2);

        // Active arc
        const float val = m_value ? *m_value : 0.5f;
        p.setStroke(m_isHovered ? 0xFFF5BDE6 : 0xFF8AADF4, 8.0f);
        p.drawArc(cx - r, cy - r, r * 2, r * 2, -90.0f, val * 360.0f);

        // Percentage
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", (int)(val * 100.0f));
        p.setFont({ "Segoe UI", 15.0f, helios::FontFlag::Bold });
        p.setFill(0xFFCAD3F5);

        helios::TextMetrics m{};
        p.measureText(buf, m);
        p.drawText(buf, cx - m.width / 2.0f, cy - m.height / 2.0f);
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
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_DOWN) {
            if (m_value) {
                *m_value += 0.08f;
                if (*m_value > 1.0f) *m_value = 0.05f;
                requestRepaint();

                // Synchronize with the WebView2 viewport (native -> web)
                heliosview_webview_t* wv = heliosview_host_get_webview(g_app.web_host.handle());
                if (wv) {
                    char js[128];
                    std::snprintf(js, sizeof(js), "updateGauge(%d);", (int)(*m_value * 100));
                    heliosview_webview_eval(wv, js);
                }
            }
            return true;
        }
        return false;
    }

private:
    float* m_value = nullptr;
    bool m_isHovered = false;
};

class MonitorCard : public Widget {
public:
    static std::shared_ptr<MonitorCard> create(std::string title, std::string desc) {
        auto card = std::make_shared<MonitorCard>();
        card->m_title = std::move(title);
        card->m_desc = std::move(desc);
        card->setSize(420, 90);
        return card;
    }

    void onPaint(helios::Painter& p) override {
        const helios::Rect box = bounds();
        const float w = (float)box.width;
        const float h = (float)box.height;

        p.setFill(m_isHovered ? 0xFF24273A : 0xFF1E1E2E);
        p.setStroke(m_isHovered ? 0xFF8AADF4 : 0xFF363A4F, 1.2f);
        p.drawRoundRect(0, 0, w, h, 10.0f);

        p.setFont({ "Segoe UI", 15.0f, helios::FontFlag::Bold });
        p.setFill(0xFFF5BDE6);
        p.drawText(m_title, 16.0f, 16.0f);

        p.setFont({ "Segoe UI", 12.0f, helios::FontFlag::None });
        p.setFill(0xFFA5ADCB);
        p.drawText(m_desc, 16.0f, 42.0f);
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

// ----------------------------------------------------------------------------
// JS Bridge Message Handler (Web -> Native)
// ----------------------------------------------------------------------------
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

static std::shared_ptr<Widget> BuildNativeUI() {
    auto root = VStack::create(14, 20);

    auto titleLabel = Label::create("HeliosView Studio Control");
    titleLabel->setFontSize(20.0f);
    titleLabel->setColor(0xFFCAD3F5);
    root->add(titleLabel);

    g_app.status_label = Label::create("Engine: Running (Blend2D @ 60 FPS)");
    g_app.status_label->setFontSize(13.0f);
    g_app.status_label->setColor(0xFFA6DA95);
    root->add(g_app.status_label);

    auto card = MonitorCard::create(
        "DirectDraw UIHost Telemetry",
        "Hardware-isolated viewport with retained sub-tree dispatching"
    );
    root->add(card);

    auto midRow = HStack::create(15, 0);

    auto dial = DialWidget::create(&g_app.gauge_val);
    midRow->add(dial);

    g_app.wave_widget = CustomWidget::create();
    g_app.wave_widget->setSize(310, 95);
    g_app.wave_widget->setPaint([](CustomWidget* self, helios::Painter& p) {
        const helios::Rect r = self->bounds();
        const float w = (float)r.width;
        const float h = (float)r.height;

        p.setFill(0xFF181926);
        p.setStroke(0xFF363A4F, 1.0f);
        p.drawRoundRect(0, 0, w, h, 8.0f);

        p.setStroke(0xFF24273A, 1.0f);
        p.drawLine(10, h / 2.0f, w - 10, h / 2.0f);

        p.setStroke(0xFF8AADF4, 2.2f);
        float prev_x = 10, prev_y = h / 2.0f;
        for (float x = 10; x < w - 10; x += 4) {
            float y = h / 2.0f + std::sin((x * 0.06f) + g_app.wave_phase) * 28.0f;
            p.drawLine(prev_x, prev_y, x, y);
            prev_x = x;
            prev_y = y;
        }

        p.setFont({ "Segoe UI", 10.0f, helios::FontFlag::None });
        p.setFill(0xFF8087A2);
        p.drawText("LIVE SIGNAL CH-01", 14.0f, 8.0f);
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
        heliosview_webview_t* wv = heliosview_host_get_webview(g_app.web_host.handle());
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

// ----------------------------------------------------------------------------
// Embedded HTML Dashboard for Right Viewport
// ----------------------------------------------------------------------------
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
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 20px; }
        .card { background: #1e1e2e; border: 1px solid #313244; border-radius: 12px; padding: 16px; }
        .metric-title { font-size: 12px; color: #9399b2; text-transform: uppercase; }
        .metric-value { font-size: 28px; font-weight: bold; color: #89b4fa; margin-top: 6px; }
        .progress-bar { width: 100%; height: 8px; background: #313244; border-radius: 4px; overflow: hidden; margin-top: 10px; }
        .progress-fill { height: 100%; width: 72%; background: linear-gradient(90deg, #89b4fa, #f5c2e7); transition: width 0.2s; }
        .console {
            background: #181825; border: 1px solid #313244; border-radius: 10px; padding: 12px;
            height: 180px; overflow-y: auto; font-family: Consolas, monospace; font-size: 12px; color: #a6e3a1;
        }
        .btn-web {
            background: #cba6f7; color: #11111b; border: none; padding: 10px 18px; border-radius: 8px;
            font-weight: bold; cursor: pointer; margin-top: 16px;
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
        <button class="btn-web" onclick="triggerNativeSim()">Toggle Native Simulator (Web -> Native)</button>
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

// ----------------------------------------------------------------------------
// Layout & Event Management
// ----------------------------------------------------------------------------
static void UpdateLayout(int width, int height) {
    int split_x = 460;
    if (width < 600) split_x = width / 2;

    g_app.ui_host.setBounds(0, 0, split_x, height);
    g_app.web_host.setBounds(split_x, 0, width - split_x, height);
}

int main() {
    // 1. The application object owns the message loop (helios::App::exec)
    helios::App app;

    // 2. Create the main window shell (1280 x 780)
    g_app.window = std::make_unique<helios::Window>(1280, 780, "HeliosView Studio - Hybrid Native UI & Web Platform");
    helios::Window& window = *g_app.window;

    int split_x = 460;

    // 3. Left Viewport: UIHost (Retained native UI), attached to the window
    g_app.ui_host = window.createUIHost(0, 0, split_x, 780, HELIOSVIEW_ENGINE_BLEND2D);
    auto uiTree = BuildNativeUI();
    g_app.ui_host.setRootWidget(uiTree);

    // 4. Right Viewport: WebViewHost (Chromium / WebView2), owned by its UIHost
    g_app.web_host = helios::UIHost::createWebView(window, split_x, 0, 1280 - split_x, 780);
    heliosview_webview_t* wv = heliosview_host_get_webview(g_app.web_host.handle());
    if (wv) {
        heliosview_webview_navigate_html(wv, kDashboardHtml);
        heliosview_webview_bind(wv, "toggle_sim", OnWebMessage, nullptr, nullptr);
    }

    // 5. Native event wiring: the window dispatches into its signals, the App into
    // its frame callback -- no manual event pump.
    window.closeRequested.connect([&window] { window.close(); });
    window.resized.connect([](int32_t w, int32_t h) { UpdateLayout(w, h); });

    app.frameCallback = [] {
        if (g_app.is_simulating && g_app.wave_widget) {
            g_app.wave_phase += 0.08f;
            g_app.wave_widget->requestRepaint();
        }
    };

    window.show();

    // 6. Run loop
    const int exitCode = app.exec();

    // 7. Structured teardown in strict hierarchical order:
    // First, release retained UI widget references and the tree the host holds
    g_app.wave_widget.reset();
    g_app.status_label.reset();
    g_app.sim_btn.reset();
    uiTree.reset();
    g_app.ui_host.clearRoot();

    // Second, destroy the web host this object owns, then the window (which destroys
    // the UI host it owns)
    g_app.web_host.close();
    window.close();
    g_app.window.reset();

    return exitCode;
}
