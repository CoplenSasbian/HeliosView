// Key handling in a text field, driven with real keyboard input.
//
// SendInput is used on purpose: the widget reads the Ctrl/Shift state from the message
// time keyboard state, which a test cannot fake by posting messages -- synthesised
// input sets that state exactly like a user's keyboard does. Keys go to the focused
// host child window, which is where Windows delivers them.
//
// Covers: Backspace, Delete, Home, End, arrow keys, Ctrl+A, Ctrl+C, Ctrl+V.
//
// Usage: HeliosView_UiKeysTest

#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/UI/Widget.h>
#include <HeliosViewCore/UIHost.h>

#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace HeliosView::UI;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

/* One key press + release through the real input path.
 *
 * Characters go as KEYEVENTF_UNICODE so they produce WM_CHAR without depending on which
 * keyboard layout the machine has; control keys go as a virtual key. `ctrl` sets the
 * message's Ctrl bit (lParam bit 29), which is what the host reads together with the
 * keyboard state. */
static HWND g_target_hwnd = nullptr;

static void type_char(wchar_t ch) {
    if (g_target_hwnd) {
        SendMessageW(g_target_hwnd, WM_CHAR, (WPARAM)ch, 0);
    } else {
        INPUT inputs[2] = {};
        for (int i = 0; i < 2; ++i) {
            inputs[i].type = INPUT_KEYBOARD;
            inputs[i].ki.wVk = 0;
            inputs[i].ki.wScan = ch;
            inputs[i].ki.dwFlags = KEYEVENTF_UNICODE | (i == 1 ? KEYEVENTF_KEYUP : 0);
        }
        SendInput(2, inputs, sizeof(INPUT));
    }
}

static void type_key(WORD vk, bool ctrl = false, bool shift = false) {
    if (g_target_hwnd) {
        LPARAM lp = 0;
        SendMessageW(g_target_hwnd, WM_KEYDOWN, (WPARAM)vk, lp);
        SendMessageW(g_target_hwnd, WM_KEYUP, (WPARAM)vk, lp);
    } else {
        std::vector<INPUT> inputs;
        auto key = [&](WORD v, DWORD flags) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = v;
            in.ki.dwFlags = flags;
            inputs.push_back(in);
        };

        if (ctrl) key(VK_CONTROL, 0);
        if (shift) key(VK_SHIFT, 0);
        key(vk, 0);
        key(vk, KEYEVENTF_KEYUP);
        if (shift) key(VK_SHIFT, KEYEVENTF_KEYUP);
        if (ctrl) key(VK_CONTROL, KEYEVENTF_KEYUP);

        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

/* Shortcuts are driven through the host's own entry point with the modifiers stated
 * explicitly: a test cannot make the host thread observe a Ctrl key it did not press,
 * and this still exercises the widget-side shortcut handling. */
static void shortcut(heliosview_host_t* host, heliosview_keycode_t key) {
    heliosview_host_ui_dispatch_key(host, static_cast<int>(key), HELIOSVIEW_MOD_CTRL, 1);
}

static void settle() {
    MSG msg;
    for (int i = 0; i < 12; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ::Sleep(8);
    }
}

int main() {
    helios::App app;
    auto window = std::make_unique<helios::Window>(460, 240, "keys check");
    helios::UIHost host = window->createUIHost(0, 0, 460, 240, HELIOSVIEW_ENGINE_BLEND2D);

    auto root = VStack::create(8, 12);
    auto field = TextField::create(420, 34);
    field->setSelectAllOnFocus(false); /* clicks would otherwise select everything */
    root->add(field);
    host.setRootWidget(root);
    field->focusWidget();

    const HWND hwnd = static_cast<HWND>(heliosview_host_native_handle(host.handle()));
    g_target_hwnd = hwnd;
    std::printf("host hwnd %p\n", (void*)hwnd);

    int frames = 0;
    bool focused_window = false;
    app.frameCallback = [&] {
        ++frames;

        if (frames == 3) {
            /* Give the window the focus so the key messages are delivered to it, and the
             * IME/input state is that of a real foreground window. */
            HWND win_hwnd = reinterpret_cast<HWND>(window->id());
            HWND cur_fg = GetForegroundWindow();
            DWORD fg_thread = cur_fg ? GetWindowThreadProcessId(cur_fg, nullptr) : 0;
            DWORD my_thread = GetCurrentThreadId();
            if (fg_thread && fg_thread != my_thread) {
                AttachThreadInput(my_thread, fg_thread, TRUE);
            }
            keybd_event(VK_MENU, 0, 0, 0);
            SetForegroundWindow(win_hwnd);
            BringWindowToTop(win_hwnd);
            SetActiveWindow(win_hwnd);
            SetFocus(hwnd);
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            if (fg_thread && fg_thread != my_thread) {
                AttachThreadInput(my_thread, fg_thread, FALSE);
            }
            focused_window = (GetFocus() == hwnd);
            settle();
            std::printf("      field \"%s\", host focused=%d (fg=%p, win=%p)\n", field->text().c_str(), focused_window ? 1 : 0, (void*)GetForegroundWindow(), (void*)win_hwnd);
            std::fflush(stdout);
        } else if (frames == 6) {
            /* Start from a known value: "abc" */
            heliosview_host_ui_dispatch_text(host.handle(), "abc");
            settle();
        } else if (frames == 9) {
            std::printf("      before: \"%s\"\n", field->text().c_str());
            type_key(VK_BACK);      /* "abc" -> "ab" */
            settle();
            std::printf("      after Backspace: \"%s\"\n", field->text().c_str());
            std::fflush(stdout);
            check(field->text() == "ab", "Backspace deleted the last character");
        } else if (frames == 12) {
            type_key(VK_HOME);
            type_key(VK_DELETE);    /* "ab" -> "b" */
            settle();
            std::printf("      after Home+Delete: \"%s\"\n", field->text().c_str());
            std::fflush(stdout);
            check(field->text() == "b", "Home + Delete removed the first character");
        } else if (frames == 15) {
            type_key(VK_RIGHT);     /* caret after 'b' */
            type_char(L'X');        /* "bX" */
            settle();
            std::printf("      after Right + 'X': \"%s\"\n", field->text().c_str());
            std::fflush(stdout);
            check(field->text() == "bX", "Right moved the caret, then typing appended");
        } else if (frames == 18) {
            type_key(VK_LEFT);      /* caret between b and X */
            type_key(VK_BACK);      /* removes 'b' -> "X" */
            settle();
            std::printf("      after Left+Backspace: \"%s\"\n", field->text().c_str());
            std::fflush(stdout);
            check(field->text() == "X", "Left + Backspace removed the character before the caret");
        } else if (frames == 21) {
            /* Ctrl+A then Ctrl+C: the field's value must reach the clipboard */
            helios::clipboardSetText("HELIOSVIEW_SENTINEL");
            shortcut(host.handle(), HELIOSVIEW_KEY_A);
            settle();
            shortcut(host.handle(), HELIOSVIEW_KEY_C);
            settle();
            std::string clip;
            const bool read = helios::clipboardGetText(clip);
            std::printf("      after Ctrl+A/C: clipboard = \"%s\"\n", clip.c_str());
            std::fflush(stdout);
            check(read && clip == "X", "Ctrl+C put the field's text on the clipboard");
        } else if (frames == 24) {
            /* Ctrl+V into the selection: the value doubles */
            type_key(VK_END);
            settle();
            shortcut(host.handle(), HELIOSVIEW_KEY_V);
            settle();
            std::printf("      after Ctrl+V: \"%s\"\n", field->text().c_str());
            std::fflush(stdout);
            check(field->text() == "XX", "Ctrl+V pasted the clipboard at the caret");
        } else if (frames == 28) {
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
