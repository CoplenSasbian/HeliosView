// HeliosView Demo -- Host Viewport Architecture (UI Subclass + WebView Subclass)
// Demonstrates:
//   1. Window is purely a window shell (no canvas/webview baggage)
//   2. Left side: UI Host (self-drawn native Canvas viewport with custom button & mouse hover)
//   3. Right side: WebView Host (embedded webview viewport with live HTML)
//   4. Clean split-view layout with seamless resizing

#include <HeliosView/heliosview.h>
#include <cstdio>
#include <cmath>

struct AppState {
    heliosview_window_t* window = nullptr;
    heliosview_host_t* ui_host = nullptr;
    heliosview_host_t* web_host = nullptr;

    // UI Host state
    int click_count = 0;
    bool is_hovered = false;
    bool is_pressed = false;
};

// UI Host Paint Callback
static void OnUiPaint(heliosview_host_t* host, heliosview_painter_t* painter, void* userdata) {
    auto* app = static_cast<AppState*>(userdata);

    int w = 0, h = 0;
    heliosview_host_get_bounds(host, nullptr, nullptr, &w, &h);

    // 1. Background
    heliosview_painter_clear(painter, 0xFF1E1E2E); // Dark theme

    // 2. Title Text
    heliosview_font_desc_t font_title{ "Segoe UI", 18.0f, HELIOSVIEW_FONT_BOLD };
    heliosview_painter_set_font(painter, &font_title);
    heliosview_painter_set_fill(painter, 0xFFCAD3F5);
    heliosview_painter_draw_text(painter, "Native UI Host (Canvas)", 20.0f, 30.0f);

    // 3. Subtitle / Description
    heliosview_font_desc_t font_desc{ "Segoe UI", 12.0f, 0 };
    heliosview_painter_set_font(painter, &font_desc);
    heliosview_painter_set_fill(painter, 0xFFA5ADCB);
    heliosview_painter_draw_text(painter, "Independent viewport inside host window", 20.0f, 65.0f);

    // 4. Interactive Button
    float btn_x = 20.0f, btn_y = 110.0f, btn_w = 260.0f, btn_h = 50.0f;
    uint32_t btn_color = 0xFF8AADF4; // Blue
    if (app->is_pressed) {
        btn_color = 0xFF7DC4E4;
    } else if (app->is_hovered) {
        btn_color = 0xFF91D7E3;
    }

    heliosview_painter_set_fill(painter, btn_color);
    heliosview_painter_set_stroke(painter, 0, 0);
    heliosview_painter_draw_round_rect(painter, btn_x, btn_y, btn_w, btn_h, 8.0f);

    // Button label
    heliosview_font_desc_t font_btn{ "Segoe UI", 14.0f, HELIOSVIEW_FONT_BOLD };
    heliosview_painter_set_font(painter, &font_btn);
    heliosview_painter_set_fill(painter, 0xFF181926);

    char label_buf[64];
    std::snprintf(label_buf, sizeof(label_buf), "Clicked: %d times", app->click_count);
    heliosview_painter_draw_text(painter, label_buf, btn_x + 50.0f, btn_y + 16.0f);

    // 5. Border on right edge to separate viewports
    heliosview_painter_set_stroke(painter, 0xFF363A4F, 2.0f);
    heliosview_painter_draw_line(painter, (float)w - 1.0f, 0, (float)w - 1.0f, (float)h);
}

// UI Host Mouse Event Callback
static void OnUiMouse(heliosview_host_t* host, const heliosview_host_mouse_event_t* evt, void* userdata) {
    auto* app = static_cast<AppState*>(userdata);
    float btn_x = 20.0f, btn_y = 110.0f, btn_w = 260.0f, btn_h = 50.0f;

    bool inside_btn = (evt->x >= btn_x && evt->x <= btn_x + btn_w &&
                       evt->y >= btn_y && evt->y <= btn_y + btn_h);

    bool need_repaint = false;

    if (evt->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
        if (inside_btn != app->is_hovered) {
            app->is_hovered = inside_btn;
            need_repaint = true;
        }
    } else if (evt->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
        if (app->is_hovered) {
            app->is_hovered = false;
            app->is_pressed = false;
            need_repaint = true;
        }
    } else if (evt->action == HELIOSVIEW_HOST_MOUSE_DOWN && evt->button == 1) {
        if (inside_btn) {
            app->is_pressed = true;
            need_repaint = true;
        }
    } else if (evt->action == HELIOSVIEW_HOST_MOUSE_UP && evt->button == 1) {
        if (app->is_pressed && inside_btn) {
            app->click_count++;
            need_repaint = true;

            // When native button is clicked, update the WebView page dynamically!
            heliosview_webview_t* wv = heliosview_host_get_webview(app->web_host);
            if (wv) {
                char js_code[128];
                std::snprintf(js_code, sizeof(js_code),
                              "document.getElementById('count').innerText = '%d';", app->click_count);
                heliosview_webview_eval(wv, js_code);
            }
        }
        app->is_pressed = false;
    }

    if (need_repaint) {
        heliosview_host_ui_request_repaint(host);
    }
}

static void update_layout(AppState* app, int width, int height) {
    int split = 320;
    if (width < 400) split = width / 2;

    heliosview_host_set_bounds(app->ui_host, 0, 0, split, height);
    heliosview_host_set_bounds(app->web_host, split, 0, width - split, height);
}

static int frame(void* userdata) {
    auto* app = static_cast<AppState*>(userdata);
    heliosview_event_t ev;
    while (heliosview_poll(&ev)) {
        switch (ev.type) {
        case HELIOSVIEW_EVENT_WINDOW_RESIZE:
            update_layout(app, ev.width, ev.height);
            break;
        case HELIOSVIEW_EVENT_WINDOW_CLOSE:
            heliosview_window_destroy(app->window);
            app->window = nullptr;
            heliosview_quit();
            break;
        default:
            break;
        }
    }
    return 0;
}

int main() {
    AppState app;

    // 1. Create pure window shell (1000 x 650)
    app.window = heliosview_window_create(1000, 650, "HeliosView - Host Architecture Demo (UI + WebView)");
    if (!app.window) {
        std::fprintf(stderr, "Failed to create window!\n");
        return 1;
    }

    int split_x = 320;

    // 2. Left side: UI Host (width: split_x)
    app.ui_host = heliosview_host_create_ui(
        app.window, 0, 0, split_x, 650, HELIOSVIEW_ENGINE_BLEND2D
    );
    heliosview_host_ui_set_paint_callback(app.ui_host, OnUiPaint, &app);
    heliosview_host_ui_set_mouse_callback(app.ui_host, OnUiMouse, &app);

    // 3. Right side: WebView Host (width: 1000 - split_x)
    app.web_host = heliosview_host_create_webview(
        app.window, split_x, 0, 1000 - split_x, 650
    );

    // Load initial HTML in webview
    heliosview_webview_t* wv = heliosview_host_get_webview(app.web_host);
    if (wv) {
        const char* html = R"html(
            <!DOCTYPE html>
            <html>
            <head>
                <style>
                    body {
                        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
                        background: #181926;
                        color: #cad3f5;
                        padding: 30px;
                    }
                    h1 { color: #f5bde6; }
                    .card {
                        background: #24273a;
                        border-radius: 12px;
                        padding: 20px;
                        margin-top: 20px;
                        border: 1px solid #363a4f;
                    }
                    .badge {
                        background: #8aadf4;
                        color: #181926;
                        font-weight: bold;
                        padding: 4px 10px;
                        border-radius: 6px;
                        font-size: 18px;
                    }
                </style>
            </head>
            <body>
                <h1>Embedded WebView Host</h1>
                <p>This side is running Chromium (WebView2) inside its own isolated Host viewport.</p>
                <div class="card">
                    <h3>Synchronized State from Native UI:</h3>
                    <p>Native Button Clicked: <span id="count" class="badge">0</span></p>
                </div>
            </body>
            </html>
        )html";
        heliosview_webview_navigate_html(wv, html);
    }

    heliosview_window_show(app.window);

    // 4. Run message loop
    heliosview_run(frame, &app);

    if (app.ui_host) heliosview_host_destroy(app.ui_host);
    if (app.web_host) heliosview_host_destroy(app.web_host);
    if (app.window) heliosview_window_destroy(app.window);

    return 0;
}
