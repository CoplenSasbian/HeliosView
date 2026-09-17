// Reproduce the reported failure on the real application path: helios::Window +
// helios::UIHost + App::exec (the message loop), with a TextField as the root. After
// the loop starts it types ASCII and Chinese through the host's own dispatch entry
// points -- the same functions WM_CHAR calls -- so a crash lands here with the last
// printed step naming it.
//
// Usage: HeliosView_UiExecInputTest

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>
#include <HeliosViewCore/UIHost.h>

#include <cstdio>
#include <memory>

using namespace HeliosView::UI;

static int g_failures = 0;

static void step(const char* what) {
    std::printf("... %s\n", what);
    std::fflush(stdout);
}

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

int main() {
    helios::App app;

    step("create window");
    auto window = std::make_unique<helios::Window>(520, 300, "exec input check");

    step("create UI host");
    helios::UIHost host = window->createUIHost(0, 0, 520, 300, HELIOSVIEW_ENGINE_BLEND2D);
    check(host.valid(), "host is valid");

    step("build the tree and set it as the root");
    auto root = VStack::create(10, 16);
    auto label = Label::create("Field:");
    auto field = TextField::create(400, 34);
    field->setPlaceholder("type english / 中文");
    field->onChange([](const std::string& v) {
        std::printf("      onChange: \"%s\" (%zu bytes)\n", v.c_str(), v.size());
        std::fflush(stdout);
    });
    root->add(label);
    root->add(field);
    host.setRootWidget(root);

    step("focus the field");
    field->focusWidget();
    check(field->hasFocus(), "field has focus through the C++ path");

    // The whole point: run the real message loop, and drive input from inside it, in
    // the same order a user would (typed key -> paint -> next key).
    step("install frame logic and show the window");
    int frames = 0;
    app.frameCallback = [&] {
        ++frames;
        if (frames == 3) {
            heliosview_host_ui_dispatch_text(host.handle(), "a");
        } else if (frames == 6) {
            heliosview_host_ui_dispatch_text(host.handle(), "b");
        } else if (frames == 9) {
            heliosview_host_ui_dispatch_text(host.handle(), "\xE4\xB8\xAD\xE6\x96\x87"); // 中文
        } else if (frames == 12) {
            heliosview_host_ui_dispatch_composition(host.handle(), "zhong");
        } else if (frames == 15) {
            heliosview_host_ui_dispatch_composition(host.handle(), "");
        } else if (frames == 18) {
            heliosview_host_ui_dispatch_key(host.handle(), HELIOSVIEW_KEY_BACKSPACE, 0, 1);
        } else if (frames == 21) {
            std::printf("      frame %d: value is \"%s\"\n", frames, field->text().c_str());
            std::fflush(stdout);
            window->close();
        }
        host.requestRepaint();
    };

    window->closeRequested.connect([&] { window->close(); });
    window->show();

    step("running the message loop");
    const int code = app.exec();
    step("message loop returned");

    check(field->text() == "a" "b" "\xE4\xB8\xAD", "final value is 'ab中'");
    check(code == 0, "loop exited cleanly");

    host.clearRoot();
    field.reset();
    label.reset();
    root.reset();
    window->close();
    window.reset();

    std::printf("=== %s (%d failures) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

