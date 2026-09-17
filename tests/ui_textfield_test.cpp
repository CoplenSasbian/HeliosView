// TextField buffer integrity with mixed ASCII / multi-byte text.
//
// The reported symptom is byte loss, not code point loss, when inserting in the middle
// -- which is what a byte-oriented edit does to a UTF-8 string. This drives the editing
// primitives directly and prints the value with its size after every step, so a step
// that truncates or splits a sequence is visible immediately.
//
// Usage: HeliosView_UiTextFieldTest

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>
#include <HeliosViewCore/UIHost.h>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

using namespace HeliosView::UI;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

/* Is this a well-formed UTF-8 string? Byte loss shows up here first. */
static bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= s.size()) return false;
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

static void show(const char* step, const std::string& value) {
    std::printf("      %-26s \"%s\"  (%zu bytes, utf8=%s)\n", step, value.c_str(), value.size(),
                valid_utf8(value) ? "yes" : "NO");
    std::fflush(stdout);
}

int main() {
    helios::App app;
    auto window = std::make_unique<helios::Window>(420, 200, "textfield check");
    helios::UIHost host = window->createUIHost(0, 0, 420, 200, HELIOSVIEW_ENGINE_BLEND2D);

    auto root = VStack::create(8, 12);
    auto field = TextField::create(380, 34);
    field->setSelectAllOnFocus(false);
    root->add(field);
    host.setRootWidget(root);
    field->focusWidget();

    const char* kCjk = "\xE4\xB8\xAD\xE6\x96\x87"; /* 中文 */

    int frames = 0;
    app.frameCallback = [&] {
        ++frames;
        if (frames == 3) {
            /* --- build "ab" then insert Chinese in the middle --- */
            heliosview_host_ui_dispatch_text(host.handle(), "ab");
            show("after ab", field->text());

            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_LEFT, 0, 1);
            heliosview_host_ui_dispatch_text(host.handle(), kCjk);
            show("insert 中文 mid", field->text());
            check(field->text() == "a" "\xE4\xB8\xAD\xE6\x96\x87" "b", "Chinese inserted mid-string");
            check(valid_utf8(field->text()), "value stays well-formed UTF-8");
        } else if (frames == 6) {
            /* --- insert ASCII in the middle of the Chinese run --- */
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_HOME, 0, 1);
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_RIGHT, 0, 1); /* after 'a' */
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_RIGHT, 0, 1); /* after 中 */
            heliosview_host_ui_dispatch_text(host.handle(), "X");
            show("insert X between 中/文", field->text());
            check(field->text() == "a" "\xE4\xB8\xAD" "X" "\xE6\x96\x87" "b", "ASCII inserted between two CJK chars");
            check(valid_utf8(field->text()), "value stays well-formed UTF-8");
        } else if (frames == 9) {
            /* --- backspace inside the run, one code point at a time --- */
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_BACKSPACE, 0, 1);
            show("backspace", field->text());
            check(field->text() == "a" "\xE4\xB8\xAD" "\xE6\x96\x87" "b", "backspace removed the inserted X");
            check(valid_utf8(field->text()), "value stays well-formed UTF-8");
        } else if (frames == 12) {
            /* --- paste in the middle --- */
            helios::clipboardSetText("\xE6\xB5\x8B\xE8\xAF\x95" "TEST"); /* 测试TEST */
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_HOME, 0, 1);
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_RIGHT, 0, 1);
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_V, HELIOSVIEW_MOD_CTRL, 1);
            show("ctrl+V mid", field->text());
            check(field->text() == "a" "\xE6\xB5\x8B\xE8\xAF\x95" "TEST" "\xE4\xB8\xAD" "\xE6\x96\x87" "b",
                  "paste landed at the caret");
            check(valid_utf8(field->text()), "value stays well-formed UTF-8");
        } else if (frames == 15) {
            /* --- select all, replace with a shorter string --- */
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_A, HELIOSVIEW_MOD_CTRL, 1);
            heliosview_host_ui_dispatch_text(host.handle(), kCjk);
            show("replace selection", field->text());
            check(field->text() == kCjk, "typing over a selection replaces it");
        } else if (frames == 18) {
            /* --- a multi-line paste into a single-line field --- */
            helios::clipboardSetText("first\r\nsecond");
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_A, HELIOSVIEW_MOD_CTRL, 1);
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_V, HELIOSVIEW_MOD_CTRL, 1);
            show("paste CRLF text", field->text());
            check(field->text() == "first", "a CRLF paste keeps only the first line, no stray CR");
            check(field->text().find('\r') == std::string::npos, "no carriage return in the value");
        } else if (frames == 21) {
            /* --- many alternating insertions: the growing-then-shrinking case --- */
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_HOME, 0, 1);
            for (int i = 0; i < 6; ++i) {
                heliosview_host_ui_dispatch_text(host.handle(), "\xE4\xB8\xAD"); /* 中 */
                heliosview_host_ui_dispatch_text(host.handle(), "z");
            }
            show("interleaved inserts", field->text());
            check(valid_utf8(field->text()), "value stays well-formed UTF-8 after interleaving");
        } else if (frames == 24) {
            window->close();
        }
        host.requestRepaint();
    };

    window->closeRequested.connect([&] { window->close(); });
    window->show();
    app.exec();

    host.clearRoot();
    field.reset();
    root.reset();
    window->close();
    window.reset();

    std::printf("=== %s (%d failures) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
