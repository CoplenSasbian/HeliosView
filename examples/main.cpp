// HeliosView.Core 窗体 feature 示例（信号槽用法）。
// 程序引用 header-only 的 HeliosView.Core，其内部调用 HeliosView.dll 的 C 接口。
// 交互：移动鼠标 / 按键（Esc 关闭窗口）/ 点击窗口右上角 X
#include <HeliosViewCore/HeliosView.h>

#include <cstdio>

int main()
{
    std::printf("HeliosView %s\n", helios::version().c_str());

    helios::App app;
    helios::Window window(800, 600, "HeliosView Demo");
    window.show();

    // 信号槽（Qt 风格）：无需子类化 Window，直接 connect lambda
    window.resized.connect([](int32_t w, int32_t h) {
        std::printf("[win] resize %d x %d\n", w, h);
    });

    window.keyPressed.connect([&window](helios::KeyCode key) {
        std::printf("[win] key down: %d\n", static_cast<int>(key));
        if (key == helios::KeyCode::Escape)
            window.close(); // Esc 关闭窗口（消息循环随之退出）
    });

    window.mouseMoved.connect([](int32_t x, int32_t y) {
        std::printf("[win] mouse move: %d, %d\n", x, y);
    });

    window.mouseButtonPressed.connect([](int32_t x, int32_t y, helios::MouseButton button) {
        std::printf("[win] mouse button %d down at %d, %d\n",
                    static_cast<int>(button), x, y);
    });

    window.closed.connect([] {
        std::printf("[win] close requested\n");
        // 默认行为：Window::event 发射 closed 后自动销毁窗口，
        // App::exec 在最后一个窗口关闭后退出
    });

    return app.exec();
}
