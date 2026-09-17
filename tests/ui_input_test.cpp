// Standalone check for the retained-UI input path: focus, committed text (what an IME
// commit and a typed key both become), key-driven editing, and the composition preview.
// Written against the C++ wrappers, so the whole application-facing stack is exercised.
//
// Usage: HeliosView_UiInputTest

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>

#include <cstdio>
#include <string>

using namespace HeliosView::UI;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

int main() {
    helios::Window window(640, 420, "input check");
    helios::UIHost host = window.createUIHost(0, 0, 640, 420, HELIOSVIEW_ENGINE_BLEND2D);
    if (!host.valid()) {
        std::printf("FAIL  host create\n");
        return 2;
    }
    heliosview_host_t* h = host.handle();

    auto field = TextField::create(320, 34);
    host.setRootWidget(field);

    // The host repaints through a paint cycle; focusing exercises the C++ -> C focus path
    field->focusWidget();
    check(heliosview_host_ui_get_focus(h) == field->handle(), "focus lands on the field");
    check(field->hasFocus(), "widget sees itself as focused");

    // --- typed text and IME commits both arrive as UTF-8 text -------------------
    check(heliosview_host_ui_dispatch_text(h, "A") == 1, "dispatch_text consumes a key");
    check(field->text() == "A", "typed text is inserted");

    // "中文" = E4 B8 AD E6 96 87: an IME commit looks exactly like this
    check(heliosview_host_ui_dispatch_text(h, "\xE4\xB8\xAD\xE6\x96\x87") == 1,
          "dispatch_text consumes an IME commit");
    check(field->text() == "A\xE4\xB8\xAD\xE6\x96\x87", "Chinese text is inserted intact");

    // --- an unhandled host is a no-op ------------------------------------------
    check(heliosview_host_ui_dispatch_text(nullptr, "x") == 0, "NULL host is rejected");

    // --- key-driven editing ----------------------------------------------------
    check(heliosview_host_ui_dispatch_key(h, HELIOSVIEW_KEY_BACKSPACE, 0, 1) == 1,
          "backspace is consumed");
    // One backspace removes the whole "文" (3 bytes), never a partial sequence
    check(field->text() == "A\xE4\xB8\xAD", "backspace deletes a whole code point");

    check(heliosview_host_ui_dispatch_key(h, HELIOSVIEW_KEY_HOME, 0, 1) == 1, "home is consumed");
    check(heliosview_host_ui_dispatch_key(h, HELIOSVIEW_KEY_DELETE, 0, 1) == 1,
          "delete is consumed");
    check(field->text() == "\xE4\xB8\xAD", "delete removes at the caret");

    // --- IME composition preview ----------------------------------------------
    check(heliosview_host_ui_dispatch_composition(h, "zhong") == 1, "composition is consumed");
    check(field->text() == "\xE4\xB8\xAD", "composition does not commit into the value");
    check(heliosview_host_ui_dispatch_composition(h, "") == 1, "composition clear is consumed");

    // --- the widget reports its caret for the candidate window -----------------
    // (a plain number: only that the call path runs without a host crash)
    heliosview_ui_widget_report_ime_caret(field->handle(), 12, 8, 18.0f);
    check(true, "caret report runs");

    // --- a second tree takes focus -------------------------------------------
    auto other = TextField::create(200, 30);
    host.setRootWidget(other); // replaces the root; the host now knows this tree
    other->focusWidget();
    check(heliosview_host_ui_get_focus(h) == other->handle(), "focus moves to the new tree");
    check(heliosview_host_ui_dispatch_text(h, "x") == 1, "the new field receives text");
    // --- PlatformInputContext & TextInputClient abstraction verification ---
    HeliosView::PlatformInputContext* ctx = host.inputContext();
    check(ctx != nullptr, "host provides PlatformInputContext");
    if (ctx) {
        check(ctx->activeClient() == other->textInputClient(), "activeClient matches focused field");
        check(other->textInputClient() != nullptr, "field exposes TextInputClient");

        // Test caret box query in host space
        helios::Rect r = other->caretHostRect();
        check(r.height > 0, "caretHostRect returns valid height");

        // Test deleteSurroundingText via client interface
        other->textInputClient()->insertText("123");
        check(other->text() == "x123", "client insertText succeeds");
        other->textInputClient()->deleteSurroundingText(2, 0);
        check(other->text() == "x1", "client deleteSurroundingText succeeds");

        // Test composition preview & confirmation via client
        other->textInputClient()->setComposition("test", 4);
        check(other->text() == "x1", "client composition does not mutate value");
        other->textInputClient()->confirmComposition();
        check(other->text() == "x1test", "client confirmComposition commits text");
    }

    host.clearRoot();
    if (ctx) {
        check(ctx->activeClient() == nullptr, "clearing root detaches active client");
    }
    other.reset();
    field.reset();
    window.close();

    heliosview_host_t* hostHandle = h;
    (void)hostHandle;

    std::printf("=== %s (%d failures) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
