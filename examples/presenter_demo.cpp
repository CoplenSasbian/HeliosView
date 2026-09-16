/**
 * HeliosView Presenter Full-Pipeline Live Demo
 *
 * Demonstrates the decoupled, zero-dependency bit-presentation architecture:
 *
 *     Canvas (Blend2D / GDI+)
 *         |
 *         v (pixelView() - non-owning slice, 0 copy, 0 window dependency)
 *     PixelView
 *         |
 *         v (present() - Win32 subclass, SetDIBitsToDevice, no-flicker double-buffer)
 *     BufferPresenter
 *         |
 *         v (HWND client area)
 *     Window
 *
 * Interactive Features:
 *   - Direct on-screen interactive buttons (Play/Pause, Theme, Engine, Counter)
 *   - Smooth, non-blocking animation & window dragging/resizing
 *   - Low-latency mouse tracking with hover & press effects
 *   - Keyboard shortcuts: Space, C, E, Esc
 */

#include <HeliosViewCore/HeliosView.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>

namespace {

struct Button {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    std::string text;
    bool isHovered = false;
    bool isPressed = false;
    std::function<void()> onClick;

    bool hitTest(float px, float py) const {
        return px >= x && px <= (x + w) && py >= y && py <= (y + h);
    }
};

struct Ripple {
    float x = 0;
    float y = 0;
    float radius = 0;
    float maxRadius = 160.0f;
    float alpha = 1.0f;
    bool active = false;
};

class PresenterDemoApp {
public:
    PresenterDemoApp()
        : m_winWidth(1000)
        , m_winHeight(640)
        , m_currentEngine(helios::PaintEngine::Builtin)
        , m_window(m_winWidth, m_winHeight, "HeliosView - Interactive BufferPresenter Live Demo")
        , m_presenter(m_window)
        , m_canvas(m_winWidth, m_winHeight, helios::PixelFormat::Bgra8Premul, m_currentEngine)
    {
        // 1. Presenter callback for window resize
        m_presenter.onResize([this](int w, int h) {
            onResize(w, h);
        });

        // 2. Window event signals
        m_window.mouseMoved.connect([this](int32_t x, int32_t y) {
            m_mouseX = static_cast<float>(x);
            m_mouseY = static_cast<float>(y);
            updateButtonHover(m_mouseX, m_mouseY);
        });

        m_window.mouseButtonPressed.connect([this](int32_t x, int32_t y, helios::MouseButton btn) {
            if (btn == helios::MouseButton::Left) {
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                m_isMouseDown = true;
                bool hitBtn = false;
                for (auto& b : m_buttons) {
                    if (b.hitTest(fx, fy)) {
                        b.isPressed = true;
                        hitBtn = true;
                        break;
                    }
                }
                if (!hitBtn) {
                    spawnRipple(fx, fy);
                }
            }
        });

        m_window.mouseButtonReleased.connect([this](int32_t x, int32_t y, helios::MouseButton btn) {
            if (btn == helios::MouseButton::Left) {
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                m_isMouseDown = false;
                for (auto& b : m_buttons) {
                    if (b.isPressed) {
                        b.isPressed = false;
                        if (b.hitTest(fx, fy) && b.onClick) {
                            b.onClick();
                        }
                    }
                }
            }
        });

        m_window.keyPressed.connect([this](helios::KeyCode key) {
            onKeyPressed(key);
        });

        m_window.closeRequested.connect([this]() {
            stop();
            m_window.close();
        });

        initButtons();

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_fpsTimer = std::chrono::steady_clock::now();

        // 3. Animation timer (~60fps, 16ms interval)
        m_timerId = helios::interval(std::chrono::milliseconds(16), &PresenterDemoApp::timerCallback, this);

        printBanner();
    }

    ~PresenterDemoApp() {
        stop();
    }

    void run() {
        m_window.center();
        m_window.show();

        // Ensure focus
        HWND hwnd = reinterpret_cast<HWND>(m_window.id());
        if (hwnd) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }

        renderFrame();
    }

private:
    void initButtons() {
        m_buttons.clear();

        // 1. Pause / Play
        m_buttons.push_back({0, 0, 130, 36, "Pause", false, false, [this]() {
            m_paused = !m_paused;
            m_buttons[0].text = m_paused ? "Play" : "Pause";
            std::printf("[Click] Animation state -> %s\n", m_paused ? "PAUSED" : "PLAYING");
        }});

        // 2. Theme
        m_buttons.push_back({0, 0, 140, 36, "Theme Color", false, false, [this]() {
            m_themeIndex = (m_themeIndex + 1) % 4;
            std::printf("[Click] Switched Theme -> #%d\n", m_themeIndex + 1);
        }});

        // 3. Engine switch
        m_buttons.push_back({0, 0, 160, 36, "Engine: Blend2D", false, false, [this]() {
            toggleEngine();
        }});

        // 4. Counter test
        m_buttons.push_back({0, 0, 140, 36, "Clicks: 0", false, false, [this]() {
            m_clickCounter++;
            m_buttons[3].text = "Clicks: " + std::to_string(m_clickCounter);
            std::printf("[Click] Interactive Counter = %d\n", m_clickCounter);
        }});

        // 5. Exit
        m_buttons.push_back({0, 0, 90, 36, "Quit", false, false, [this]() {
            stop();
            m_window.close();
        }});

        layoutButtons(m_winWidth, m_winHeight);
    }

    void layoutButtons(int w, int /*h*/) {
        const float startX = 30.0f;
        const float startY = 85.0f;
        float curX = startX;
        const float gap = 12.0f;

        for (auto& b : m_buttons) {
            b.x = curX;
            b.y = startY;
            curX += b.w + gap;
        }
        (void)w;
    }

    void updateButtonHover(float px, float py) {
        for (auto& b : m_buttons) {
            b.isHovered = b.hitTest(px, py);
        }
    }

    void stop() {
        if (m_timerId != 0) {
            helios::cancelTimer(m_timerId);
            m_timerId = 0;
        }
    }

    static void timerCallback(uint32_t /*timer_id*/, void* userdata) {
        auto* self = static_cast<PresenterDemoApp*>(userdata);
        if (self) {
            self->onTimerTick();
        }
    }

    void onTimerTick() {
        // Prevent re-entrancy and message loop starvation
        if (m_isRendering) return;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;

        if (!m_paused) {
            m_angle += 1.8f * dt;
            m_pulse += 2.5f * dt;
            updateRipples(dt);
        }

        m_frameCount++;
        float elapsedFps = std::chrono::duration<float>(now - m_fpsTimer).count();
        if (elapsedFps >= 0.5f) {
            m_currentFps = static_cast<float>(m_frameCount) / elapsedFps;
            m_frameCount = 0;
            m_fpsTimer = now;
        }

        renderFrame();
    }

    void updateRipples(float dt) {
        for (auto& r : m_ripples) {
            if (r.active) {
                r.radius += 240.0f * dt;
                r.alpha = 1.0f - (r.radius / r.maxRadius);
                if (r.radius >= r.maxRadius || r.alpha <= 0.0f) {
                    r.active = false;
                }
            }
        }
    }

    void spawnRipple(float x, float y) {
        for (auto& r : m_ripples) {
            if (!r.active) {
                r.x = x;
                r.y = y;
                r.radius = 5.0f;
                r.alpha = 1.0f;
                r.active = true;
                break;
            }
        }
    }

    void onResize(int w, int h) {
        if (w <= 0 || h <= 0) return;
        m_winWidth = w;
        m_winHeight = h;

        layoutButtons(w, h);
        m_canvas.resize(w, h);
        renderFrame();
    }

    void onKeyPressed(helios::KeyCode key) {
        switch (key) {
        case helios::KeyCode::Escape:
            stop();
            m_window.close();
            break;

        case helios::KeyCode::Space:
            m_paused = !m_paused;
            m_buttons[0].text = m_paused ? "Play" : "Pause";
            std::printf("[Key] Animation %s\n", m_paused ? "Paused" : "Resumed");
            break;

        case helios::KeyCode::C:
            m_themeIndex = (m_themeIndex + 1) % 4;
            std::printf("[Key] Switched accent theme: %d\n", m_themeIndex + 1);
            break;

        case helios::KeyCode::E:
            toggleEngine();
            break;

        default:
            break;
        }
    }

    void toggleEngine() {
        if (m_currentEngine == helios::PaintEngine::Builtin) {
            m_currentEngine = helios::PaintEngine::Native;
            m_buttons[2].text = "Engine: GDI+";
        } else {
            m_currentEngine = helios::PaintEngine::Builtin;
            m_buttons[2].text = "Engine: Blend2D";
        }
        m_canvas = helios::Canvas(m_winWidth, m_winHeight, helios::PixelFormat::Bgra8Premul, m_currentEngine);
        std::printf("[Engine] Switched Canvas engine -> %s (valid=%d)\n",
                    m_currentEngine == helios::PaintEngine::Builtin ? "Builtin (Blend2D)" : "Native (GDI+)",
                    m_canvas.valid() ? 1 : 0);
        renderFrame();
    }

    uint32_t getAccentColor(uint8_t alpha = 255) const {
        static const uint32_t kThemes[4] = {
            0x003A86FF, // Electric Blue
            0x0010B981, // Emerald Green
            0x00F43F5E, // Crimson Rose
            0x00F59E0B  // Warm Amber
        };
        uint32_t rgb = kThemes[m_themeIndex];
        return (static_cast<uint32_t>(alpha) << 24) | rgb;
    }

    void renderFrame() {
        if (!m_canvas.valid() || m_isRendering) return;

        m_isRendering = true;
        auto t0 = std::chrono::steady_clock::now();

        const int w = m_canvas.width();
        const int h = m_canvas.height();

        // 1. Draw into Canvas
        {
            helios::Painter p(m_canvas);

            // Clean modern dark background
            p.clear(0xFF0F1117);

            // Top banner bar
            p.setFill(0x181A23);
            p.fillRect(0, 0, static_cast<float>(w), 75.0f, 0xFF141924);

            // Mouse glow spotlight
            if (m_mouseX > 0 && m_mouseY > 0) {
                p.setFill(getAccentColor(24));
                p.setStroke(0, 0);
                p.drawEllipse(m_mouseX, m_mouseY, 130.0f, 130.0f);
            }

            // Interactive ripples
            for (const auto& r : m_ripples) {
                if (r.active) {
                    uint8_t a = static_cast<uint8_t>(std::clamp(r.alpha * 255.0f, 0.0f, 255.0f));
                    p.setStroke(getAccentColor(a), 2.0f);
                    p.setFill(0);
                    p.drawEllipse(r.x, r.y, r.radius, r.radius);
                }
            }

            // UI Header
            p.setFont({"Segoe UI", 20.0f, helios::FontFlag::Bold});
            p.setFill(0xFFF1F5F9);
            p.drawText("HeliosView BufferPresenter Live Demo", 30, 20);

            p.setFont({"Segoe UI", 12.0f});
            p.setFill(0xFF94A3B8);
            p.drawText("Zero-Copy PixelView -> Direct Window DC Blit | Highly responsive 60 FPS", 30, 48);

            // Render interactive buttons
            p.setFont({"Segoe UI", 13.0f, helios::FontFlag::Bold});
            for (const auto& b : m_buttons) {
                uint32_t bg = 0xFF1E2638;
                uint32_t border = 0xFF334155;
                uint32_t textCol = 0xFFE2E8F0;

                if (b.isPressed) {
                    bg = getAccentColor(120);
                    border = getAccentColor(255);
                    textCol = 0xFFFFFFFF;
                } else if (b.isHovered) {
                    bg = getAccentColor(45);
                    border = getAccentColor(200);
                    textCol = 0xFFFFFFFF;
                }

                p.setFill(bg);
                p.setStroke(border, 1.5f);
                p.drawRoundRect(b.x, b.y, b.w, b.h, 6.0f);

                p.setFill(textCol);
                p.drawText(b.text, b.x + 14.0f, b.y + 10.0f);
            }

            // Center card
            const float cardW = std::min(520.0f, w - 80.0f);
            const float cardH = std::min(320.0f, h - 210.0f);
            const float cardX = (w - cardW) * 0.5f;
            const float cardY = 145.0f + (h - 145.0f - 60.0f - cardH) * 0.5f;

            if (cardW > 120 && cardH > 100) {
                p.setFill(getAccentColor(14));
                p.setStroke(0, 0);
                p.drawRoundRect(cardX - 4, cardY - 4, cardW + 8, cardH + 8, 16.0f);

                p.setFill(0xF0151A24);
                p.setStroke(getAccentColor(100), 1.5f);
                p.drawRoundRect(cardX, cardY, cardW, cardH, 12.0f);

                const float cx = cardX + cardW * 0.5f;
                const float cy = cardY + cardH * 0.5f;

                p.save();
                p.translate(cx, cy);
                p.rotate(m_angle);

                const int petals = 8;
                for (int i = 0; i < petals; ++i) {
                    float a = static_cast<float>(i) * (3.14159265f * 2.0f / petals);
                    float px = std::cos(a) * 48.0f;
                    float py = std::sin(a) * 48.0f;
                    float r = 22.0f + 5.0f * std::sin(m_pulse + i);

                    p.setFill(getAccentColor(80));
                    p.setStroke(getAccentColor(220), 1.5f);
                    p.drawEllipse(px, py, r, r);
                }

                p.setFill(0xFFFFFFFF);
                p.setStroke(getAccentColor(255), 3.0f);
                p.drawEllipse(0, 0, 16.0f, 16.0f);

                p.restore();
            }

            // Bottom dashboard bar
            char infoBuffer[256];
            std::snprintf(infoBuffer, sizeof(infoBuffer),
                          "Engine: %-8s | Res: %dx%d | FPS: %4.1f | Frame: %.2f ms | Clicks: %d | Theme: #%d",
                          m_currentEngine == helios::PaintEngine::Builtin ? "Blend2D" : "GDI+",
                          w, h, m_currentFps, m_lastCostMs, m_clickCounter, m_themeIndex + 1);

            p.setFill(0xCC0B0E14);
            p.setStroke(0xFF242E40, 1.0f);
            p.drawRoundRect(24, h - 46, w - 48, 32, 6.0f);

            p.setFont({"Consolas", 13.0f});
            p.setFill(0xFF38BDF8);
            p.drawText(infoBuffer, 36, h - 37);
        }

        auto t1 = std::chrono::steady_clock::now();
        m_lastCostMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

        // 2. Direct blit to window DC
        m_presenter.present(m_canvas);

        m_isRendering = false;
    }

    void printBanner() const {
        std::printf("===============================================================\n");
        std::printf("  HeliosView BufferPresenter Live Interactive Demo\n");
        std::printf("===============================================================\n");
        std::printf("  * Default Engine: %s (valid=%d)\n",
                    m_currentEngine == helios::PaintEngine::Builtin ? "Builtin (Blend2D)" : "Native (GDI+)",
                    m_canvas.valid() ? 1 : 0);
        std::printf("  * Direct DC presentation: Active (Non-Blocking)\n");
        std::printf("  * Interactive Buttons: Pause, Theme, Engine, Counter\n");
        std::printf("  * Shortcuts: Space, C, E, Esc\n");
        std::printf("===============================================================\n");
    }

    // Member declaration order strictly matches constructor initialization order:
    int m_winWidth = 1000;
    int m_winHeight = 640;
    helios::PaintEngine m_currentEngine = helios::PaintEngine::Builtin;

    helios::Window m_window;
    helios::BufferPresenter m_presenter;
    helios::Canvas m_canvas;

    uint32_t m_timerId = 0;
    bool m_paused = false;
    bool m_isRendering = false;
    int m_themeIndex = 0;
    int m_clickCounter = 0;
    bool m_isMouseDown = false;

    float m_angle = 0.0f;
    float m_pulse = 0.0f;
    float m_mouseX = -100.0f;
    float m_mouseY = -100.0f;

    float m_currentFps = 60.0f;
    float m_lastCostMs = 0.0f;
    int m_frameCount = 0;

    std::chrono::steady_clock::time_point m_lastFrameTime;
    std::chrono::steady_clock::time_point m_fpsTimer;

    std::vector<Button> m_buttons;
    std::vector<Ripple> m_ripples{12};
};

} // namespace

int main()
{
    helios::App app;
    PresenterDemoApp demo;
    demo.run();
    return app.exec();
}
