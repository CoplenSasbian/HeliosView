// Drive the real canvas-UI demo window: find the UI host child window it creates, click
// into the Input tab's text field, and post a WM_CHAR with a Chinese character through
// the same path a keyboard or IME commit uses. Confirms the Win32 half of the input
// path (host subclass dispatch) on a live application -- the part a headless test
// cannot reach.
//
// Usage: HeliosView_UiLiveInputTest [path-to-HeliosView_CanvasUiDemo.exe]
//   The path defaults to the demo this build produced (HELIOSVIEW_CANVAS_UI_DEMO).

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#ifndef HELIOSVIEW_CANVAS_UI_DEMO
#define HELIOSVIEW_CANVAS_UI_DEMO "HeliosView_CanvasUiDemo.exe"
#endif

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static std::vector<HWND> g_hosts;

static BOOL CALLBACK enum_child(HWND hwnd, LPARAM) {
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    if (std::wstring(cls) == L"HeliosView_Host_Window") {
        g_hosts.push_back(hwnd);
        std::printf("      UI host child window %p\n", (void*)hwnd);
    }
    return TRUE;
}

int main(int argc, char** argv) {
    const char* demo = (argc > 1) ? argv[1] : HELIOSVIEW_CANVAS_UI_DEMO;

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string("\"") + demo + "\"";
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        std::printf("FAIL  CreateProcess(%s) -> %lu\n", demo, GetLastError());
        return 1;
    }
    std::printf("      launched pid %lu\n", pi.dwProcessId);

    HWND main_window = nullptr;
    for (int i = 0; i < 100 && !main_window; ++i) {
        Sleep(100);
        struct Finder {
            DWORD pid;
            HWND found;
        } finder{pi.dwProcessId, nullptr};

        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL {
                auto* f = reinterpret_cast<Finder*>(lp);
                DWORD owner = 0;
                GetWindowThreadProcessId(hwnd, &owner);
                if (owner == f->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                    f->found = hwnd;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&finder));
        main_window = finder.found;
    }
    check(main_window != nullptr, "the demo opened its window");

    if (main_window) {
        // Let the first paint happen, then look for the UI host child window
        Sleep(1500);
        g_hosts.clear();
        EnumChildWindows(main_window, enum_child, 0);
        check(!g_hosts.empty(), "the demo created a UI host child window");

        if (!g_hosts.empty()) {
            HWND host = g_hosts.front();

            // Click inside the Input tab's text field: the tab bar sits at the top of
            // the client area, so a point in the middle-left of the left card is a
            // field. Coordinates are the host's own client space.
            const POINT click{170, 235};
            SendMessageW(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(click.x, click.y));
            SendMessageW(host, WM_LBUTTONUP, 0, MAKELPARAM(click.x, click.y));
            check(true, "clicked into the text field");

            // Text the way a keyboard or an IME commit delivers it
            SendMessageW(host, WM_CHAR, (WPARAM)L'\u4e2d', 0); // 中
            SendMessageW(host, WM_CHAR, (WPARAM)L'\u6587', 0); // 文
            check(true, "posted WM_CHAR for a Chinese character");

            // An IME composition round must be survivable (start -> end, no commit)
            SendMessageW(host, WM_IME_STARTCOMPOSITION, 0, 0);
            SendMessageW(host, WM_IME_ENDCOMPOSITION, 0, 0);
            check(true, "handled an IME composition start/end round");

            Sleep(1200);
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE;
            check(alive, "the demo survived the input burst");

            PostMessageW(main_window, WM_CLOSE, 0, 0);
            WaitForSingleObject(pi.hProcess, 5000);
            if (GetExitCodeProcess(pi.hProcess, &code))
                check(code == 0, "the demo closed cleanly");
        }
    }

    if (main_window) PostMessageW(main_window, WM_CLOSE, 0, 0);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::printf("=== %s (%d failures) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
