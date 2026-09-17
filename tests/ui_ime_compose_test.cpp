// Deterministic check of the IME composition path, without depending on the IME being
// "active" (which only happens for the foreground window of a real user session).
//
// Attaches an IME context to the host window, starts a composition and writes a
// composition string into that context. Windows then delivers WM_IME_COMPOSITION to the
// window, and the host must read the string back through ImmGetCompositionStringW and
// hand UTF-8 to the focused widget -- the same path a user's pinyin keystrokes take.
//
// Usage: HeliosView_UiImeComposeTest

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>
#include <HeliosViewCore/UIHost.h>

#include <windows.h>
#include <imm.h>

#include <cstdio>
#include <memory>
#include <string>

#pragma comment(lib, "imm32.lib")

using namespace HeliosView::UI;

static int g_compositions = 0;
static bool g_preview_seen = false;
static bool g_keys_ignored_during_composition = false;

class LoggingField : public TextField {
public:
    static std::shared_ptr<LoggingField> create(int w, int h) {
        auto f = std::shared_ptr<LoggingField>(new LoggingField());
        f->setSize(w, h);
        return f;
    }

    bool onComposition(std::string_view utf8) override {
        ++g_compositions;
        if (!utf8.empty()) g_preview_seen = true;
        std::printf("      onComposition(\"%s\")\n", std::string(utf8).c_str());
        std::fflush(stdout);
        return TextField::onComposition(utf8);
    }

private:
    LoggingField() = default;
};

int main() {
    helios::App app;
    auto window = std::make_unique<helios::Window>(460, 240, "ime compose check");
    helios::UIHost host = window->createUIHost(0, 0, 460, 240, HELIOSVIEW_ENGINE_BLEND2D);

    auto root = VStack::create(8, 12);
    auto field = LoggingField::create(420, 34);   // commit target
    auto preview = LoggingField::create(420, 34); // composition target
    root->add(field);
    root->add(preview);
    host.setRootWidget(root);
    field->focusWidget();

    HWND hostHwnd = static_cast<HWND>(heliosview_host_native_handle(host.handle()));
    std::printf("host hwnd %p\n", (void*)hostHwnd);
    std::fflush(stdout);

    auto pump = [] {
        MSG msg;
        for (int i = 0; i < 10; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    };

    const bool ime_attached = [] (HWND h) {
        HIMC imc = ImmGetContext(h);
        if (!imc) return false;
        ImmReleaseContext(h, imc);
        return true;
    }(hostHwnd);
    std::printf("%s  the host window has an IME context\n", ime_attached ? "ok  " : "FAIL");
    if (!ime_attached) return 1;

    int frames = 0;
    bool committed_ok = false;
    bool preview_ok = false;
    app.frameCallback = [&] {
        ++frames;
        if (frames == 3) {
            SetFocus(hostHwnd);
            pump();

            // ---- commit first, while the IME context is untouched ----
            // A character on selection: some IMEs deliver it as WM_IME_CHAR, some as
            // WM_CHAR, some as both, and both channels must produce ONE insertion.
            SendMessageW(hostHwnd, WM_IME_CHAR, (WPARAM)L'\u4e2d', 0); // 中
            SendMessageW(hostHwnd, WM_CHAR, (WPARAM)L'\u4e2d', 0);     // the same char again
            SendMessageW(hostHwnd, WM_IME_CHAR, (WPARAM)L'\u6587', 0); // 文
            committed_ok = field->text() == "\xE4\xB8\xAD\xE6\x96\x87";
            std::printf("      commit check: \"%s\" (%zu bytes)\n", field->text().c_str(), field->text().size());
            std::fflush(stdout);
        } else if (frames == 8) {
            // ---- composition preview, on the second field ----
            // Give the focus to the other field so the preview lands there and the
            // committed text above stays verifiable.
            preview->focusWidget();
            pump();
            SendMessageW(hostHwnd, WM_IME_STARTCOMPOSITION, 0, 0);

            HIMC imc = ImmGetContext(hostHwnd);
            if (imc) {
                // "zhong": the pre-edit a pinyin IME shows before a commit
                wchar_t comp[] = L"zhong";
                const BOOL ok = ImmSetCompositionStringW(
                    imc, SCS_SETSTR, comp, static_cast<DWORD>(wcslen(comp) * sizeof(wchar_t)), nullptr, 0);
                std::printf("      ImmSetCompositionString(zhong) -> %s\n", ok ? "ok" : "failed");
                std::fflush(stdout);
                if (ok) {
                    // SCS_SETSTR does not notify the window; an IME sends this after it
                    SendMessageW(hostHwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
                }
                ImmReleaseContext(hostHwnd, imc);
            }
        } else if (frames == 12) {
            /* While an IME composes, the KEYS belong to the IME: Backspace must not reach
             * the widget and edit text the user has not committed yet. (WM_CHAR is
             * committed text by definition and is still expected to insert -- that is
             * how an IME delivers punctuation that commits directly.) */
            const std::string before = preview->text();
            SendMessageW(hostHwnd, WM_KEYDOWN, VK_BACK, 0);
            SendMessageW(hostHwnd, WM_KEYUP, VK_BACK, 0);
            SendMessageW(hostHwnd, WM_KEYDOWN, VK_DELETE, 0);
            SendMessageW(hostHwnd, WM_KEYUP, VK_DELETE, 0);
            g_keys_ignored_during_composition = (preview->text() == before);
            std::printf("      keys during composition: preview \"%s\" (%s)\n",
                        preview->text().c_str(),
                        g_keys_ignored_during_composition ? "untouched" : "CHANGED");
            std::fflush(stdout);
        } else if (frames == 16) {
            // End the composition and drop the preview
            HIMC imc = ImmGetContext(hostHwnd);
            if (imc) {
                ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(hostHwnd, imc);
            }
            SendMessageW(hostHwnd, WM_IME_ENDCOMPOSITION, 0, 0);
            preview_ok = g_preview_seen;
            std::printf("      preview check: composition seen = %d, preview field = \"%s\"\n",
                        g_preview_seen ? 1 : 0, preview->text().c_str());
            std::fflush(stdout);
        } else if (frames == 20) {
            window->close();
        }
        host.requestRepaint();
    };

    window->closeRequested.connect([&] { window->close(); });
    window->show();
    app.exec();

    const std::string expected = "\xE4\xB8\xAD" "\xE6\x96\x87";
    const bool committed = field->text() == expected;

    std::printf("%s  the commit landed exactly once (got \"%s\", %zu bytes)\n",
                committed && committed_ok ? "ok  " : "FAIL", field->text().c_str(), field->text().size());
    std::printf("%s  the composition string travelled IME context -> host -> widget\n",
                preview_ok ? "ok  " : "FAIL");
    std::printf("%s  keys during composition stayed with the IME (value untouched)\n",
                g_keys_ignored_during_composition ? "ok  " : "FAIL");

    host.clearRoot();
    preview.reset();
    field.reset();
    root.reset();
    window->close();
    window.reset();

    const bool ok = committed && preview_ok && g_keys_ignored_during_composition;
    std::printf("=== %s ===\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}


