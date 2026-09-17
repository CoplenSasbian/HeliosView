// Which half of Chinese input is broken? Runs the real message loop with a focused
// TextField and posts WM_CHAR with a Chinese character at the host child window -- the
// message a keyboard or an IME commit eventually delivers. If the field ends up with
// the character, the key/text half is fine and the missing piece is the IME
// composition itself; if it is empty, the host's WM_CHAR handling is at fault.
//
// Usage: HeliosView_UiWmCharTest

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>
#include <HeliosViewCore/UIHost.h>

#include <windows.h>

#include <cstdio>
#include <memory>

using namespace HeliosView::UI;

int main() {
    helios::App app;
    auto window = std::make_unique<helios::Window>(420, 220, "wm_char check");
    helios::UIHost host = window->createUIHost(0, 0, 420, 220, HELIOSVIEW_ENGINE_BLEND2D);

    auto root = VStack::create(8, 12);
    auto field = TextField::create(380, 34);
    root->add(field);
    host.setRootWidget(root);
    field->focusWidget();

    HWND hostHwnd = static_cast<HWND>(heliosview_host_native_handle(host.handle()));
    std::printf("host hwnd %p, focused=%d\n", (void*)hostHwnd, field->hasFocus() ? 1 : 0);
    std::fflush(stdout);

    int frames = 0;
    app.frameCallback = [&] {
        ++frames;
        if (frames == 3) {
            // WM_CHAR straight at the host, exactly where a typed key or an IME commit
            // lands. 4e2d 6587 = 中文.
            SendMessageW(hostHwnd, WM_CHAR, (WPARAM)L'\u4e2d', 0);
            SendMessageW(hostHwnd, WM_CHAR, (WPARAM)L'\u6587', 0);
            std::printf("posted WM_CHAR 中文 -> field is now \"%s\" (%zu bytes)\n",
                        field->text().c_str(), field->text().size());
            std::fflush(stdout);
        } else if (frames == 6) {
            // And an ASCII one, for contrast
            SendMessageW(hostHwnd, WM_CHAR, (WPARAM)L'A', 0);
            std::printf("posted WM_CHAR A -> field is now \"%s\" (%zu bytes)\n",
                        field->text().c_str(), field->text().size());
            std::fflush(stdout);
        } else if (frames == 9) {
            /* The same character typed twice in a row must insert twice. This is the
             * case a "drop a repeated character" guard silently eats, which turns fast
             * typing into text that gets shorter the more you type. */
            SendMessageW(hostHwnd, WM_CHAR, (WPARAM)L'\u4e2d', 0); // 中
            SendMessageW(hostHwnd, WM_CHAR, (WPARAM)L'\u4e2d', 0); // 中 again
            std::printf("posted WM_CHAR 中中 -> \"%s\" (%zu bytes)\n",
                        field->text().c_str(), field->text().size());
            std::fflush(stdout);
        } else if (frames == 12) {
            /* The other half: one character an IME reports through BOTH of its channels
             * must insert once, not twice. */
            SendMessageW(hostHwnd, WM_IME_CHAR, (WPARAM)L'\u6587', 0); // 文 via the IME
            SendMessageW(hostHwnd, WM_IME_CHAR, (WPARAM)L'\u6587', 0); // 文 again: a second char
            std::printf("posted WM_IME_CHAR 文文 -> \"%s\" (%zu bytes)\n",
                        field->text().c_str(), field->text().size());
            std::fflush(stdout);
        } else if (frames == 15) {
            window->close();
        }
        host.requestRepaint();
    };

    window->closeRequested.connect([&] { window->close(); });
    window->show();
    const int code = app.exec();

    /* 中文A + 中中 + 文文 */
    const std::string expected = "\xE4\xB8\xAD\xE6\x96\x87" "A"
                                 "\xE4\xB8\xAD\xE4\xB8\xAD"
                                 "\xE6\x96\x87\xE6\x96\x87";
    const bool ok = field->text() == expected;
    std::printf("%s  WM_CHAR/WM_IME_CHAR delivered 中文A中中文文 (got \"%s\", %zu bytes)\n",
                ok ? "ok  " : "FAIL", field->text().c_str(), field->text().size());
    const bool well_formed = field->text().size() == expected.size();
    std::printf("%s  repeated characters were not dropped (no shrinking)\n",
                well_formed ? "ok  " : "FAIL");
    const bool all_ok = ok && well_formed;

    host.clearRoot();
    field.reset();
    root.reset();
    window->close();
    window.reset();

    std::printf("=== %s ===\n", all_ok ? "PASS" : "FAIL");
    return all_ok && code == 0 ? 0 : 1;
}
