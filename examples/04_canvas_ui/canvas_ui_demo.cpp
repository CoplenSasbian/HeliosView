// ============================================================================
// HeliosView Example 04: Canvas DirectDraw & Retained UI Component Gallery
// ============================================================================
// A comprehensive showcase of HeliosView's 2D vector drawing engine & modern
// retained-mode UI component system, written entirely against the C++ wrappers:
//   helios::Window / helios::App       - native window shell & message loop
//   helios::UIHost                     - the DirectDraw child viewport
//   HeliosView::UI::*                  - retained widget tree (Card, Slider, ...)
//   helios::Painter                    - every drawing call (no C painter calls)
//
//   Tab 0: OS Shell              - Window shell, lifecycle, dialogs, tray, clipboard
//   Tab 1: Controls & Forms      - Buttons, Sliders, Switches, Checkboxes, Progress, Badges
//   Tab 2: Charts & Analytics    - Real-time Area Line Chart with Crosshair, Bar Chart, Donut Gauges
//   Tab 3: Vector & Paths        - Even-Odd Stars, Bezier Curves, Rotating Interlocking Gears
//   Tab 4: Gauges & Oscilloscope - Semicircular Analog Tachometer, Dual-Channel 60FPS Waveform
// ============================================================================

#include <HeliosViewCore/Dialogs.h>
#include <HeliosViewCore/HeliosView.h>
#include <HeliosViewCore/Menu.h>
#include <HeliosViewCore/Notification.h>
#include <HeliosViewCore/Tray.h>
#include <HeliosViewCore/UI/Widget.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace HeliosView::UI;

namespace {

// The showcase window (owned by main; the widget callbacks only borrow it)
helios::Window* s_win = nullptr;
std::unique_ptr<helios::Menu> s_contextMenu;
std::unique_ptr<helios::Tray> s_tray;
std::unique_ptr<helios::Menu> s_trayMenu;
std::vector<std::shared_ptr<helios::Window>> s_subWindows;
std::string s_lastPickedPath;
int s_subWindowCount = 0;

constexpr float kPi = 3.14159265358979323846f;

// ----------------------------------------------------------------------------
// Shared Global State for Interactive Showcase
// ----------------------------------------------------------------------------
struct AppState {
    int activeTab = 0;
    int clickCounter = 0;
    bool jitEnabled = true;
    bool antialiasEnabled = true;
    bool vsyncEnabled = true;
    bool deepDarkEnabled = true;
    bool highDpiEnabled = false;

    float volume = 74.0f;
    float brightness = 85.0f;
    float animSpeed = 1.0f;

    // Animation & Waveform State
    float timeSec = 0.0f;
    float gearAngle = 0.0f;
    float tachometerVal = 88.0f;

    // Line Chart Crosshair
    int chartHoverX = -1;
    int chartHoverY = -1;
    bool chartHovered = false;

    // Bar Chart Hover
    int barHoveredIndex = -1;
};

static AppState g_state;

// ----------------------------------------------------------------------------
// Helpers for Vector Drawing
// ----------------------------------------------------------------------------
static void DrawStar(helios::Painter& p, float cx, float cy, float outerR, float innerR, int points, uint32_t fill, uint32_t stroke) {
    std::vector<float> pts;
    pts.reserve(points * 4);
    for (int i = 0; i < points * 2; ++i) {
        float r = (i % 2 == 0) ? outerR : innerR;
        float angle = -kPi / 2.0f + i * (kPi / points);
        pts.push_back(cx + std::cos(angle) * r);
        pts.push_back(cy + std::sin(angle) * r);
    }
    p.setFill(fill);
    p.setStroke(stroke, 1.5f);
    p.drawPolygon(pts);
}

static void DrawGear(helios::Painter& p, float cx, float cy, float radius, int teeth, float angleRad, uint32_t color) {
    p.save();
    p.translate(cx, cy);
    p.rotate(angleRad);

    std::vector<float> pts;
    float dAngle = (2.0f * kPi) / teeth;
    float toothH = radius * 0.18f;

    for (int i = 0; i < teeth; ++i) {
        float a0 = i * dAngle;
        float a1 = a0 + dAngle * 0.25f;
        float a2 = a0 + dAngle * 0.50f;
        float a3 = a0 + dAngle * 0.75f;

        pts.push_back(std::cos(a0) * radius);
        pts.push_back(std::sin(a0) * radius);

        pts.push_back(std::cos(a1) * (radius + toothH));
        pts.push_back(std::sin(a1) * (radius + toothH));

        pts.push_back(std::cos(a2) * (radius + toothH));
        pts.push_back(std::sin(a2) * (radius + toothH));

        pts.push_back(std::cos(a3) * radius);
        pts.push_back(std::sin(a3) * radius);
    }

    p.setFill(color);
    p.setStroke(0xFF181926, 1.5f);
    p.drawPolygon(pts);

    // Center hole
    p.setFill(0xFF1E1E2E);
    p.setStroke(0xFF45475A, 1.5f);
    p.drawEllipse(-radius * 0.35f, -radius * 0.35f, radius * 0.70f, radius * 0.70f);

    p.restore();
}

// ----------------------------------------------------------------------------
// Tab 0: Native Window Shell & OS Integration View (Comprehensive Master Showcase)
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabShellIntegration() {
    auto tabRoot = HStack::create(16, 0);

    // Shared status feedback label
    auto statusLbl = Label::create("Status: Ready. Click any native action below.");
    statusLbl->setFontSize(12.0f)->setColor(0xFFA6E3A1);

    auto updateStatus = [statusLbl](const std::string& msg) {
        statusLbl->setText(std::format("[{}] {}", "OK", msg));
    };

    // ---- Left Card: Window Shell, Multi-Window & Lifecycle ----
    auto leftCard = Card::create(475, 540, 0xFF1E1E2E, 0xFF313244);
    auto leftStack = VStack::create(12, 16);

    auto titleL = Label::create("Window Shell & Lifecycle Controls");
    titleL->setFontSize(15.0f);
    titleL->setColor(0xFF8AADF4);
    leftStack->add(titleL);

    // Opacity Slider
    auto opacLbl = Label::create("Window Opacity: 100%");
    opacLbl->setFontSize(12.5f)->setColor(0xFFCAD3F5);
    leftStack->add(opacLbl);

    auto opacSlider = Slider::create(30.0f, 100.0f, 100.0f);
    opacSlider->setSize(440, 26);
    opacSlider->onChange([opacLbl, updateStatus](float val) {
        opacLbl->setText(std::format("Window Opacity: {:.0f}%", val));
        if (s_win) {
            s_win->setOpacity(val / 100.0f);
            updateStatus(std::format("Window opacity set to {:.0f}%", val));
        }
    });
    leftStack->add(opacSlider);

    // State Toggles
    auto swRow1 = HStack::create(10, 0);
    auto swTop = Switch::create(false);
    swTop->onToggle([updateStatus](bool on) {
        if (s_win) {
            s_win->setTopmost(on);
            updateStatus(on ? "Topmost enabled (Always on Top)" : "Topmost disabled");
        }
    });
    auto swTopLbl = Label::create("Always on Top (Topmost Window)");
    swTopLbl->setFontSize(12.5f)->setColor(0xFFCDD6F4);
    swRow1->add(swTop)->add(swTopLbl);
    leftStack->add(swRow1);

    auto swRow2 = HStack::create(10, 0);
    auto swFs = Switch::create(false);
    swFs->onToggle([updateStatus](bool on) {
        if (s_win) {
            s_win->setFullscreen(on);
            updateStatus(on ? "Fullscreen mode active" : "Fullscreen mode exited");
        }
    });
    auto swFsLbl = Label::create("Fullscreen Mode (Kiosk / Immersive)");
    swFsLbl->setFontSize(12.5f)->setColor(0xFFCDD6F4);
    swRow2->add(swFs)->add(swFsLbl);
    leftStack->add(swRow2);

    auto swRow3 = HStack::create(10, 0);
    auto swDark = Switch::create(true);
    swDark->onToggle([updateStatus](bool on) {
        if (s_win) {
            s_win->setDarkMode(on);
            updateStatus(on ? "Native dark frame active" : "Native dark frame disabled");
        }
    });
    auto swDarkLbl = Label::create("Native Dark Mode Frame");
    swDarkLbl->setFontSize(12.5f)->setColor(0xFFCDD6F4);
    swRow3->add(swDark)->add(swDarkLbl);
    leftStack->add(swRow3);

    // Window action buttons
    auto btnRow1 = HStack::create(8, 0);
    auto maxBtn = Button::create("Maximize/Restore");
    maxBtn->setSize(130, 32);
    maxBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->toggleMaximize();
            updateStatus("Toggled Maximize/Restore");
        }
    });
    auto minBtn = Button::create("Minimize");
    minBtn->setSize(85, 32);
    minBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->minimize();
            updateStatus("Window minimized");
        }
    });
    auto centerBtn = Button::create("Center Window");
    centerBtn->setSize(110, 32);
    centerBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->center();
            updateStatus("Window centered on primary monitor");
        }
    });
    btnRow1->add(maxBtn)->add(minBtn)->add(centerBtn);
    leftStack->add(btnRow1);

    auto btnRow2 = HStack::create(8, 0);
    auto flashBtn = Button::create("Flash Taskbar");
    flashBtn->setSize(105, 32);
    flashBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->flash();
            updateStatus("Taskbar flash triggered");
        }
    });
    auto flashUntilBtn = Button::create("Flash Until Focus");
    flashUntilBtn->setSize(125, 32);
    flashUntilBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->flashUntilFocus();
            updateStatus("Taskbar flash until focus active");
        }
    });
    btnRow2->add(flashBtn)->add(flashUntilBtn);
    leftStack->add(btnRow2);

    // Multi-Window & Lifecycle
    auto subWinHeader = Label::create("Multi-Window & Application Lifecycle");
    subWinHeader->setFontSize(13.0f)->setColor(0xFFCBA6F7);
    leftStack->add(subWinHeader);

    auto btnRow3 = HStack::create(8, 0);
    auto spawnWinBtn = Button::create("Spawn Sub-Window");
    spawnWinBtn->setSize(135, 32);
    spawnWinBtn->onClick([updateStatus]() {
        int n = ++s_subWindowCount;
        auto sub = std::make_shared<helios::Window>(420, 300, std::format("Canvas Sub-Window #{}", n).c_str());
        sub->show();

        auto* rawSub = sub.get();
        rawSub->closeRequested.connect([rawSub, updateStatus, n]() {
            updateStatus(std::format("Sub-Window #{} closed", n));
            rawSub->close();
            std::erase_if(s_subWindows, [rawSub](const std::shared_ptr<helios::Window>& item) {
                return item.get() == rawSub;
            });
        });
        rawSub->keyPressed.connect([updateStatus, n](helios::KeyCode k) {
            updateStatus(std::format("Key in Sub-Window #{}: code {}", n, static_cast<int>(k)));
        });

        s_subWindows.push_back(std::move(sub));
        updateStatus(std::format("Spawned Sub-Window #{} (Active: {})", n, s_subWindows.size()));
    });

    auto closeWinsBtn = Button::create("Close Sub-Windows");
    closeWinsBtn->setSize(135, 32);
    closeWinsBtn->onClick([updateStatus]() {
        size_t cnt = s_subWindows.size();
        s_subWindows.clear();
        updateStatus(std::format("Closed all {} sub-windows", cnt));
    });

    auto quitBtn = Button::create("Quit App");
    quitBtn->setSize(80, 32);
    quitBtn->onClick([]() {
        if (auto* app = helios::App::instance()) app->quit();
    });
    btnRow3->add(spawnWinBtn)->add(closeWinsBtn)->add(quitBtn);
    leftStack->add(btnRow3);

    leftCard->addChild(leftStack);
    tabRoot->add(leftCard);

    // ---- Right Card: Native OS Integration, Dialogs & System Queries ----
    auto rightCard = Card::create(475, 540, 0xFF1E1E2E, 0xFF313244);
    auto rightStack = VStack::create(12, 16);

    auto titleR = Label::create("Native OS Integration, Dialogs & System");
    titleR->setFontSize(15.0f);
    titleR->setColor(0xFFA6E3A1);
    rightStack->add(titleR);

    // Linked Taskbar Progress
    auto tbLbl = Label::create("Linked Taskbar Progress: 50%");
    tbLbl->setFontSize(12.5f)->setColor(0xFFCAD3F5);
    rightStack->add(tbLbl);

    auto tbSlider = Slider::create(0.0f, 100.0f, 50.0f);
    tbSlider->setSize(440, 26);
    auto tbBar = ProgressBar::create(0.50f);
    tbBar->setSize(440, 6);
    tbBar->setColor(0xFFA6E3A1);

    tbSlider->onChange([tbLbl, tbBar, updateStatus](float val) {
        tbLbl->setText(std::format("Linked Taskbar Progress: {:.0f}%", val));
        tbBar->setProgress(val / 100.0f);
        if (s_win) {
            s_win->setProgressState(helios::ProgressState::Normal);
            s_win->setProgress(static_cast<uint32_t>(val), 100);
            updateStatus(std::format("Taskbar progress set to {:.0f}%", val));
        }
    });
    rightStack->add(tbSlider);
    rightStack->add(tbBar);

    auto tbStateRow = HStack::create(8, 0);
    auto tbPauseBtn = Button::create("Paused (Yellow)");
    tbPauseBtn->setSize(105, 28);
    tbPauseBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->setProgressState(helios::ProgressState::Paused);
            updateStatus("Taskbar progress state: PAUSED (Yellow)");
        }
    });
    auto tbErrBtn = Button::create("Error (Red)");
    tbErrBtn->setSize(90, 28);
    tbErrBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->setProgressState(helios::ProgressState::Error);
            updateStatus("Taskbar progress state: ERROR (Red)");
        }
    });
    auto tbClrBtn = Button::create("Clear Taskbar");
    tbClrBtn->setSize(95, 28);
    tbClrBtn->onClick([updateStatus]() {
        if (s_win) {
            s_win->clearProgress();
            updateStatus("Taskbar progress cleared");
        }
    });
    tbStateRow->add(tbPauseBtn)->add(tbErrBtn)->add(tbClrBtn);
    rightStack->add(tbStateRow);

    // Native Dialogs Row 1
    auto dlgRow1 = HStack::create(8, 0);
    auto fileBtn = Button::create("Open File(s)");
    fileBtn->setSize(100, 32);
    fileBtn->onClick([updateStatus]() {
        auto files = helios::openFiles(s_win ? s_win->nativeHandle() : nullptr, "Select File (Canvas UI)");
        if (!files.empty()) {
            s_lastPickedPath = files.front();
            updateStatus(std::format("File picked: {}", s_lastPickedPath));
        } else {
            updateStatus("File selection cancelled");
        }
    });
    auto folderBtn = Button::create("Pick Folder");
    folderBtn->setSize(95, 32);
    folderBtn->onClick([updateStatus]() {
        std::string folder;
        if (helios::selectFolder(s_win ? s_win->nativeHandle() : nullptr, "Select Folder", folder)) {
            s_lastPickedPath = folder;
            updateStatus(std::format("Folder picked: {}", s_lastPickedPath));
        } else {
            updateStatus("Folder selection cancelled");
        }
    });
    auto saveBtn = Button::create("Save File");
    saveBtn->setSize(90, 32);
    saveBtn->onClick([updateStatus]() {
        std::string path;
        const std::vector<helios::FileFilter> filters = {{"Text Files", "txt"}, {"All Files", "*.*"}};
        if (helios::saveFile(s_win ? s_win->nativeHandle() : nullptr, "Save File (Canvas UI)", filters, "sample.txt", path)) {
            s_lastPickedPath = path;
            updateStatus(std::format("Save target: {}", s_lastPickedPath));
        } else {
            updateStatus("Save file cancelled");
        }
    });
    auto revealBtn = Button::create("Reveal in Explorer");
    revealBtn->setSize(125, 32);
    revealBtn->onClick([updateStatus]() {
        if (s_lastPickedPath.empty()) {
            updateStatus("Please pick a file or folder first!");
        } else {
            helios::showInFolder(s_lastPickedPath);
            updateStatus(std::format("Revealed in Explorer: {}", s_lastPickedPath));
        }
    });
    dlgRow1->add(fileBtn)->add(folderBtn)->add(saveBtn)->add(revealBtn);
    rightStack->add(dlgRow1);

    // System Messaging & Queries Row 2
    auto dlgRow2 = HStack::create(8, 0);
    auto msgBoxBtn = Button::create("Modal MsgBox");
    msgBoxBtn->setSize(100, 32);
    msgBoxBtn->onClick([updateStatus]() {
        auto res = helios::messageBox(s_win ? s_win->nativeHandle() : nullptr,
                                      helios::MessageBoxType::Question, helios::MessageBoxButtons::YesNo,
                                      "HeliosView Canvas UI", "Do you confirm this action?");
        updateStatus(std::format("MsgBox result: {}", (res == helios::MessageBoxResult::Yes) ? "YES" : "NO"));
    });

    auto toastBtn = Button::create("Action Toast");
    toastBtn->setSize(100, 32);
    toastBtn->onClick([updateStatus]() {
        helios::notificationShow("HeliosView Canvas UI", "Toast notification dispatched from 2D Vector UI!");
        updateStatus("Dispatched Action Center Toast notification");
    });

    auto trayBtn = Button::create("Tray Alert");
    trayBtn->setSize(90, 32);
    trayBtn->onClick([updateStatus]() {
        if (!s_tray) {
            s_tray = std::make_unique<helios::Tray>("HeliosView UI Gallery");
            s_trayMenu = std::make_unique<helios::Menu>();
            auto* itemRestore = s_trayMenu->addItem("Restore Window");
            itemRestore->triggered.connect([] { if (s_win) s_win->show(); });
            s_trayMenu->addSeparator();
            auto* itemQuit = s_trayMenu->addItem("Quit");
            itemQuit->triggered.connect([] { if (auto* app = helios::App::instance()) app->quit(); });
            s_tray->setMenu(s_trayMenu->handle());
        }
        s_tray->notify("HeliosView Gallery", "Tray balloon alert triggered!", helios::NotifyIcon::Info);
        updateStatus("System tray balloon notification posted");
    });

    auto menuBtn = Button::create("Context Menu");
    menuBtn->setSize(105, 32);
    menuBtn->onClick([updateStatus]() {
        if (s_contextMenu && s_win) {
            s_contextMenu->show(s_win->nativeHandle());
            updateStatus("Popped native context menu");
        }
    });
    dlgRow2->add(msgBoxBtn)->add(toastBtn)->add(trayBtn)->add(menuBtn);
    rightStack->add(dlgRow2);

    // Clipboard & Environment Row 3
    auto dlgRow3 = HStack::create(8, 0);
    auto clipSetBtn = Button::create("Copy Clipboard");
    clipSetBtn->setSize(110, 32);
    clipSetBtn->onClick([updateStatus]() {
        helios::clipboardSetText("HeliosView Canvas Retained UI Text Snapshot");
        updateStatus("Copied 'HeliosView Canvas Retained UI Text Snapshot' to clipboard");
    });

    auto clipGetBtn = Button::create("Read Clipboard");
    clipGetBtn->setSize(110, 32);
    clipGetBtn->onClick([updateStatus]() {
        std::string txt;
        if (helios::clipboardGetText(txt)) {
            updateStatus(std::format("Clipboard contains: \"{}\"", txt.substr(0, 40)));
        } else {
            updateStatus("Clipboard is empty or contains no text");
        }
    });

    auto workAreaBtn = Button::create("Query Work Area");
    workAreaBtn->setSize(125, 32);
    workAreaBtn->onClick([updateStatus]() {
        helios::Rect r{};
        helios::primaryWorkArea(r);
        int32_t cx = 0, cy = 0;
        helios::cursorPosition(cx, cy);
        updateStatus(std::format("WorkArea: {}x{} @ ({},{}), Cursor: ({},{})", r.width, r.height, r.x, r.y, cx, cy));
    });
    dlgRow3->add(clipSetBtn)->add(clipGetBtn)->add(workAreaBtn);
    rightStack->add(dlgRow3);

    // Status output display
    rightStack->add(statusLbl);

    rightCard->addChild(rightStack);
    tabRoot->add(rightCard);

    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 1: Controls & Forms View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabControls() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: Command & State Controls ----
    auto leftCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto leftStack = VStack::create(14, 18);

    auto titleL = Label::create("Command & Toggle Controls");
    titleL->setFontSize(16.0f);
    titleL->setColor(0xFF8AADF4);
    leftStack->add(titleL);

    auto btnRow = HStack::create(12, 0);
    auto clickBtn = Button::create("Primary Click (0)");
    clickBtn->setSize(160, 36);
    auto resetBtn = Button::create("Reset Counter");
    resetBtn->setSize(120, 36);

    clickBtn->onClick([clickBtn]() {
        g_state.clickCounter++;
        clickBtn->setLabel(std::format("Primary Click ({})", g_state.clickCounter));
    });
    resetBtn->onClick([clickBtn]() {
        g_state.clickCounter = 0;
        clickBtn->setLabel("Primary Click (0)");
    });
    btnRow->add(clickBtn);
    btnRow->add(resetBtn);
    leftStack->add(btnRow);

    auto sep1 = CustomWidget::create();
    sep1->setSize(434, 1);
    sep1->setPaint([](CustomWidget*, helios::Painter& p) {
        p.setFill(0xFF313244);
        p.drawRect(0, 0, 434, 1);
    });
    leftStack->add(sep1);

    // Switches
    auto swRow1 = HStack::create(12, 0);
    auto sw1 = Switch::create(g_state.jitEnabled);
    sw1->onToggle([](bool on) { g_state.jitEnabled = on; });
    auto sw1Lbl = Label::create("Hardware JIT Code Generation (Blend2D)");
    sw1Lbl->setFontSize(13.0f)->setColor(0xFFCDD6F4);
    swRow1->add(sw1)->add(sw1Lbl);
    leftStack->add(swRow1);

    auto swRow2 = HStack::create(12, 0);
    auto sw2 = Switch::create(g_state.antialiasEnabled);
    sw2->onToggle([](bool on) { g_state.antialiasEnabled = on; });
    auto sw2Lbl = Label::create("Analytic Subpixel Anti-Aliasing");
    sw2Lbl->setFontSize(13.0f)->setColor(0xFFCDD6F4);
    swRow2->add(sw2)->add(sw2Lbl);
    leftStack->add(swRow2);

    // Checkboxes
    auto cb1 = Checkbox::create("Enable VSync (Lock 60 FPS)", g_state.vsyncEnabled);
    cb1->onToggle([](bool on) { g_state.vsyncEnabled = on; });
    leftStack->add(cb1);

    auto cb2 = Checkbox::create("Deep Dark High-Contrast Canvas Mode", g_state.deepDarkEnabled);
    cb2->onToggle([](bool on) { g_state.deepDarkEnabled = on; });
    leftStack->add(cb2);

    auto cb3 = Checkbox::create("High-DPI Subpixel Font Kerning", g_state.highDpiEnabled);
    cb3->onToggle([](bool on) { g_state.highDpiEnabled = on; });
    leftStack->add(cb3);

    leftCard->addChild(leftStack);
    tabRoot->add(leftCard);

    // ---- Right Card: Sliders, Progress & Badges ----
    auto rightCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto rightStack = VStack::create(14, 18);

    auto titleR = Label::create("Sliders, Indicators & Badges");
    titleR->setFontSize(16.0f);
    titleR->setColor(0xFFA6E3A1);
    rightStack->add(titleR);

    // Volume Slider & Label
    auto volLbl = Label::create(std::format("Master Volume Output: {:.0f}%", g_state.volume));
    volLbl->setFontSize(13.0f)->setColor(0xFFCAD3F5);
    rightStack->add(volLbl);

    auto volSlider = Slider::create(0.0f, 100.0f, g_state.volume);
    volSlider->setSize(434, 28);

    // Progress Bar connected to Slider
    auto volBar = ProgressBar::create(g_state.volume / 100.0f);
    volBar->setSize(434, 8);
    volBar->setColor(0xFF8AADF4);

    volSlider->onChange([volLbl, volBar](float val) {
        g_state.volume = val;
        volLbl->setText(std::format("Master Volume Output: {:.0f}%", val));
        volBar->setProgress(val / 100.0f);
    });
    rightStack->add(volSlider);
    rightStack->add(volBar);

    // Brightness Slider
    auto brLbl = Label::create(std::format("Backlight Panel Brightness: {:.0f}%", g_state.brightness));
    brLbl->setFontSize(13.0f)->setColor(0xFFCAD3F5);
    rightStack->add(brLbl);

    auto brSlider = Slider::create(0.0f, 100.0f, g_state.brightness);
    brSlider->setSize(434, 28);
    brSlider->onChange([brLbl](float val) {
        g_state.brightness = val;
        brLbl->setText(std::format("Backlight Panel Brightness: {:.0f}%", val));
    });
    rightStack->add(brSlider);

    // Striped Animated Progress Bar
    auto animBarLbl = Label::create("Live Animated Streaming Buffer (60FPS Striped)");
    animBarLbl->setFontSize(13.0f)->setColor(0xFFBAC2DE);
    rightStack->add(animBarLbl);

    auto stripedBar = CustomWidget::create();
    stripedBar->setSize(434, 14);
    stripedBar->setPaint([](CustomWidget* w, helios::Painter& p) {
        const helios::Rect r = w->bounds();
        const float width = (float)r.width;
        const float height = (float)r.height;

        // Track
        p.setFill(0xFF313244);
        p.drawRoundRect(0, 0, width, height, height / 2.0f);

        // Active animated stripes
        float fillW = width * 0.78f;
        p.save();
        p.setClipRect(0, 0, fillW, height);

        p.setFill(0xFFCBA6F7);
        p.drawRoundRect(0, 0, fillW, height, height / 2.0f);

        float stripeW = 16.0f;
        float offset = std::fmod(g_state.timeSec * 35.0f, stripeW * 2.0f);
        p.setFill(0xFFB4BEFE);

        for (float sx = -stripeW * 2.0f + offset; sx < fillW + stripeW; sx += stripeW * 2.0f) {
            const float pts[] = {
                sx, height,
                sx + stripeW, height,
                sx + stripeW + 8.0f, 0.0f,
                sx + 8.0f, 0.0f
            };
            p.drawPolygon(pts, 4);
        }
        p.restore();
    });
    rightStack->add(stripedBar);

    // Badges Row
    auto badgeRow = HStack::create(12, 0);
    auto makeBadge = [](const char* text, uint32_t bg, uint32_t fg) {
        auto badge = CustomWidget::create();
        badge->setSize(130, 32);
        badge->setPaint([text, bg, fg](CustomWidget* w, helios::Painter& p) {
            const helios::Rect r = w->bounds();
            const float bw = (float)r.width;
            const float bh = (float)r.height;
            p.setFill(bg);
            p.setStroke(fg, 1.0f);
            p.drawRoundRect(0, 0, bw, bh, 6.0f);

            p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::Bold });
            p.setFill(fg);
            helios::TextMetrics m{};
            p.measureText(text, m);
            p.drawText(text, (bw - m.width) / 2.0f, (bh - m.height) / 2.0f);
        });
        return badge;
    };

    badgeRow->add(makeBadge("[STATUS: READY]", 0xFF1E2E24, 0xFFA6E3A1));
    badgeRow->add(makeBadge("[AVX2 / JIT]", 0xFF1E243A, 0xFF8AADF4));
    badgeRow->add(makeBadge("[SANDBOXED]", 0xFF2A1E34, 0xFFCBA6F7));
    rightStack->add(badgeRow);

    rightCard->addChild(rightStack);
    tabRoot->add(rightCard);

    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 1: Charts & Analytics View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabCharts() {
    auto tabRoot = VStack::create(16, 0);

    // Top: Real-Time Area Line Chart with Interactive Crosshair & Tooltip
    auto lineChartCard = Card::create(956, 260, 0xFF1E1E2E, 0xFF313244);
    auto lineChart = CustomWidget::create();
    lineChart->setSize(956, 260);

    lineChart->setPaint([](CustomWidget* w, helios::Painter& p) {
        const helios::Rect r = w->bounds();
        const float cw = (float)r.width;
        const float ch = (float)r.height;

        // Header Title
        p.setFont({ "Segoe UI", 14.0f, helios::FontFlag::Bold });
        p.setFill(0xFF8AADF4);
        p.drawText("Real-Time Multi-Point Telemetry & Load Monitor (Area Gradient)", 20.0f, 16.0f);

        float left = 50.0f;
        float right = cw - 24.0f;
        float top = 46.0f;
        float bottom = ch - 30.0f;
        float plotW = right - left;
        float plotH = bottom - top;

        // Background Grid & Y-Axis Labels
        p.setFont({ "Segoe UI", 10.0f, helios::FontFlag::None });

        for (int i = 0; i <= 4; ++i) {
            float y = bottom - i * (plotH / 4.0f);
            p.setStroke(0xFF282A3A, 1.0f);
            p.drawLine(left, y, right, y);

            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", i * 25);
            p.setFill(0xFF6E738D);
            p.drawText(buf, 14.0f, y - 6.0f);
        }

        // Generate dynamic waveform data points
        const int numPts = 64;
        std::vector<float> pts;
        pts.reserve((numPts + 2) * 2);

        for (int i = 0; i < numPts; ++i) {
            float px = left + (float)i / (numPts - 1) * plotW;
            float t = g_state.timeSec * 2.0f + (float)i * 0.12f;
            float val = 0.45f + 0.28f * std::sin(t) + 0.12f * std::cos(t * 2.3f + 1.2f);
            val = std::clamp(val, 0.05f, 0.95f);
            float py = bottom - val * plotH;
            pts.push_back(px);
            pts.push_back(py);
        }

        // Area under curve
        std::vector<float> poly = pts;
        poly.push_back(right);
        poly.push_back(bottom);
        poly.push_back(left);
        poly.push_back(bottom);

        p.setFill(0x30A6E3A1);
        p.setStroke(0, 0);
        p.drawPolygon(poly);

        // Curve stroke
        p.setStroke(0xFFA6E3A1, 2.2f);
        p.drawPolyline(pts);

        // Secondary Telemetry Stream (Mauve)
        std::vector<float> pts2;
        pts2.reserve(numPts * 2);
        for (int i = 0; i < numPts; ++i) {
            float px = left + (float)i / (numPts - 1) * plotW;
            float t = g_state.timeSec * 1.5f + (float)i * 0.09f;
            float val = 0.32f + 0.20f * std::sin(t * 1.6f + 2.0f);
            val = std::clamp(val, 0.05f, 0.95f);
            float py = bottom - val * plotH;
            pts2.push_back(px);
            pts2.push_back(py);
        }
        p.setStroke(0xFFCBA6F7, 1.8f);
        p.drawPolyline(pts2);

        // Crosshair & Data Tooltip when hovered
        if (g_state.chartHovered && g_state.chartHoverX >= left && g_state.chartHoverX <= right) {
            float hx = (float)g_state.chartHoverX;
            p.setStroke(0x808AADF4, 1.2f);
            p.drawLine(hx, top, hx, bottom);

            // Interpolated value at crosshair
            float frac = (hx - left) / plotW;
            float t = g_state.timeSec * 2.0f + frac * (numPts - 1) * 0.12f;
            float val = 0.45f + 0.28f * std::sin(t) + 0.12f * std::cos(t * 2.3f + 1.2f);
            val = std::clamp(val, 0.05f, 0.95f);
            float hy = bottom - val * plotH;

            // Highlight point
            p.setFill(0xFFFFFFFF);
            p.setStroke(0xFFA6E3A1, 2.5f);
            p.drawEllipse(hx - 5.0f, hy - 5.0f, 10.0f, 10.0f);

            // Tooltip box
            char tip[64];
            std::snprintf(tip, sizeof(tip), "CH01 Load: %.1f%%", val * 100.0f);
            float tipW = 120.0f, tipH = 26.0f;
            float tipX = (hx + 10.0f + tipW > right) ? (hx - tipW - 10.0f) : (hx + 10.0f);
            float tipY = std::clamp(hy - 30.0f, top, bottom - tipH);

            p.setFill(0xEE181926);
            p.setStroke(0xFF8AADF4, 1.0f);
            p.drawRoundRect(tipX, tipY, tipW, tipH, 4.0f);

            p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::Bold });
            p.setFill(0xFFCDD6F4);
            p.drawText(tip, tipX + 8.0f, tipY + 6.0f);
        }
    });

    lineChart->setMouse([](CustomWidget* w, const heliosview_host_mouse_event_t* e) {
        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE) {
            g_state.chartHovered = true;
            g_state.chartHoverX = e->x;
            g_state.chartHoverY = e->y;
            w->requestRepaint();
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            g_state.chartHovered = false;
            w->requestRepaint();
            return true;
        }
        return false;
    });

    lineChartCard->addChild(lineChart);
    tabRoot->add(lineChartCard);

    // Bottom Row: Bar Chart (Left) + Multi-Donut Gauge (Right)
    auto bottomRow = HStack::create(16, 0);

    // ---- Bar Chart ----
    auto barCard = Card::create(470, 244, 0xFF1E1E2E, 0xFF313244);
    auto barWidget = CustomWidget::create();
    barWidget->setSize(470, 244);

    barWidget->setPaint([](CustomWidget* w, helios::Painter& p) {
        const helios::Rect r = w->bounds();
        const float bw = (float)r.width;
        const float bh = (float)r.height;

        p.setFont({ "Segoe UI", 13.0f, helios::FontFlag::Bold });
        p.setFill(0xFFF5BDE6);
        p.drawText("Weekly Network Throughput (GB)", 18.0f, 14.0f);

        const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        float vals[] = {4.2f, 7.8f, 5.4f, 9.2f, 8.5f, 3.1f, 6.4f};
        float maxVal = 10.0f;

        float startX = 36.0f;
        float startY = bh - 42.0f;
        float chartH = 140.0f;
        float slotW = (bw - startX - 24.0f) / 7.0f;
        float barWidth = 32.0f;

        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::None });

        for (int i = 0; i < 7; ++i) {
            float x = startX + i * slotW + (slotW - barWidth) / 2.0f;
            float h = (vals[i] / maxVal) * chartH;
            float y = startY - h;

            bool hovered = (g_state.barHoveredIndex == i);
            uint32_t fill = hovered ? 0xFF8AADF4 : 0xFF585B70;
            if (i == 3) fill = hovered ? 0xFFF5BDE6 : 0xFFCBA6F7; // Peak highlight

            p.setFill(fill);
            p.setStroke(0, 0);
            p.drawRoundRect(x, y, barWidth, h, 4.0f);

            // Day label
            p.setFill(hovered ? 0xFFFFFFFF : 0xFFA6ADC8);
            p.drawText(days[i], x + 4.0f, startY + 8.0f);

            // Value on top if hovered
            if (hovered) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.1fG", vals[i]);
                p.setFill(0xFFCAD3F5);
                p.drawText(buf, x - 2.0f, y - 16.0f);
            }
        }
    });

    barWidget->setMouse([](CustomWidget* w, const heliosview_host_mouse_event_t* e) {
        float startX = 36.0f;
        float slotW = (470.0f - startX - 24.0f) / 7.0f;
        int idx = (int)((e->x - startX) / slotW);

        if (e->action == HELIOSVIEW_HOST_MOUSE_MOVE && idx >= 0 && idx < 7) {
            if (g_state.barHoveredIndex != idx) {
                g_state.barHoveredIndex = idx;
                w->requestRepaint();
            }
            return true;
        } else if (e->action == HELIOSVIEW_HOST_MOUSE_LEAVE) {
            g_state.barHoveredIndex = -1;
            w->requestRepaint();
            return true;
        }
        return false;
    });

    barCard->addChild(barWidget);
    bottomRow->add(barCard);

    // ---- Multi-Donut Gauge ----
    auto donutCard = Card::create(470, 244, 0xFF1E1E2E, 0xFF313244);
    auto donutWidget = CustomWidget::create();
    donutWidget->setSize(470, 244);

    donutWidget->setPaint([](CustomWidget* w, helios::Painter& p) {
        const float dh = (float)w->bounds().height;

        p.setFont({ "Segoe UI", 13.0f, helios::FontFlag::Bold });
        p.setFill(0xFFA6E3A1);
        p.drawText("System Quota Allocation (Nested Rings)", 18.0f, 14.0f);

        float cx = 130.0f;
        float cy = dh / 2.0f + 10.0f;

        // Outer Ring: Disk 74%
        float r1 = 68.0f;
        p.setFill(0);
        p.setStroke(0xFF313244, 8.0f);
        p.drawEllipse(cx - r1, cy - r1, r1 * 2, r1 * 2);
        p.setStroke(0xFF8AADF4, 8.0f);
        p.drawArc(cx - r1, cy - r1, r1 * 2, r1 * 2, -90.0f, 0.74f * 360.0f);

        // Middle Ring: Memory 58%
        float r2 = 52.0f;
        p.setStroke(0xFF313244, 8.0f);
        p.drawEllipse(cx - r2, cy - r2, r2 * 2, r2 * 2);
        p.setStroke(0xFFCBA6F7, 8.0f);
        p.drawArc(cx - r2, cy - r2, r2 * 2, r2 * 2, -90.0f, 0.58f * 360.0f);

        // Inner Ring: GPU VRAM 82%
        float r3 = 36.0f;
        p.setStroke(0xFF313244, 8.0f);
        p.drawEllipse(cx - r3, cy - r3, r3 * 2, r3 * 2);
        p.setStroke(0xFFA6E3A1, 8.0f);
        p.drawArc(cx - r3, cy - r3, r3 * 2, r3 * 2, -90.0f, 0.82f * 360.0f);

        // Center Score
        p.setFont({ "Segoe UI", 15.0f, helios::FontFlag::Bold });
        p.setFill(0xFFCAD3F5);
        p.drawText("71%", cx - 14.0f, cy - 8.0f);

        // Legend on Right
        float lx = 240.0f;
        float ly = 60.0f;
        auto drawLegend = [&p, &lx, &ly](uint32_t col, const char* name, const char* pct) {
            p.setFill(col);
            p.setStroke(0, 0);
            p.drawRoundRect(lx, ly, 12.0f, 12.0f, 3.0f);

            p.setFont({ "Segoe UI", 12.0f, helios::FontFlag::None });
            p.setFill(0xFFCDD6F4);
            p.drawText(name, lx + 20.0f, ly);

            p.setFont({ "Segoe UI", 12.0f, helios::FontFlag::Bold });
            p.setFill(col);
            p.drawText(pct, lx + 140.0f, ly);
            ly += 32.0f;
        };

        drawLegend(0xFF8AADF4, "NVMe Storage", "74%");
        drawLegend(0xFFCBA6F7, "Unified RAM", "58%");
        drawLegend(0xFFA6E3A1, "Dedicated GPU", "82%");
    });

    donutCard->addChild(donutWidget);
    bottomRow->add(donutCard);

    tabRoot->add(bottomRow);
    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 2: Vector Graphics & Paths View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabVectors() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: Complex Geometric Paths & Winding ----
    auto pathCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto pathWidget = CustomWidget::create();
    pathWidget->setSize(470, 520);

    pathWidget->setPaint([](CustomWidget*, helios::Painter& p) {
        p.setFont({ "Segoe UI", 14.0f, helios::FontFlag::Bold });
        p.setFill(0xFFF9E2AF);
        p.drawText("Complex 2D Paths, Stars & Bezier Curves", 18.0f, 16.0f);

        // 1. Golden 5-Point Star
        DrawStar(p, 110.0f, 110.0f, 60.0f, 26.0f, 5, 0xFFF9E2AF, 0xFFFAB387);

        // 2. 8-Point Cyan Radiant Star
        DrawStar(p, 340.0f, 110.0f, 62.0f, 32.0f, 8, 0xFF89DCEB, 0xFF74C7EC);

        // Labels
        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::None });
        p.setFill(0xFFA6ADC8);
        p.drawText("5-Point Vector Polygon", 44.0f, 185.0f);
        p.drawText("8-Point Radiant Compass", 270.0f, 185.0f);

        // Separator
        p.setFill(0xFF313244);
        p.drawRect(20.0f, 210.0f, 430.0f, 1.0f);

        // 3. Smooth Cubic Bezier Ribbon
        p.setFont({ "Segoe UI", 12.0f, helios::FontFlag::Bold });
        p.setFill(0xFFCBA6F7);
        p.drawText("Dynamic Cubic Bezier Waveform", 20.0f, 226.0f);

        float bx0 = 30.0f, by0 = 340.0f;
        float bx3 = 440.0f, by3 = 340.0f;
        float bx1 = 150.0f, by1 = 250.0f + std::sin(g_state.timeSec * 3.0f) * 60.0f;
        float bx2 = 320.0f, by2 = 430.0f - std::sin(g_state.timeSec * 3.0f) * 60.0f;

        // Draw control lines
        p.setStroke(0xFF45475A, 1.0f);
        p.drawLine(bx0, by0, bx1, by1);
        p.drawLine(bx3, by3, bx2, by2);

        // Control point handles
        p.setFill(0xFFF38BA8);
        p.drawEllipse(bx1 - 4.0f, by1 - 4.0f, 8.0f, 8.0f);
        p.drawEllipse(bx2 - 4.0f, by2 - 4.0f, 8.0f, 8.0f);

        // Approximate cubic bezier curve with polyline
        const int steps = 40;
        std::vector<float> bPts;
        bPts.reserve((steps + 1) * 2);
        for (int i = 0; i <= steps; ++i) {
            float u = (float)i / steps;
            float inv = 1.0f - u;
            float px = inv * inv * inv * bx0 + 3.0f * inv * inv * u * bx1 + 3.0f * inv * u * u * bx2 + u * u * u * bx3;
            float py = inv * inv * inv * by0 + 3.0f * inv * inv * u * by1 + 3.0f * inv * u * u * by2 + u * u * u * by3;
            bPts.push_back(px);
            bPts.push_back(py);
        }
        p.setStroke(0xFFF5C2E7, 3.0f);
        p.drawPolyline(bPts);

        // 4. Concentric Nested Polygons (Hexagon)
        float hx = 235.0f, hy = 445.0f;
        for (int r = 16; r <= 48; r += 16) {
            std::vector<float> hex;
            for (int k = 0; k < 6; ++k) {
                float a = k * (kPi / 3.0f) + g_state.timeSec * 0.5f;
                hex.push_back(hx + std::cos(a) * r);
                hex.push_back(hy + std::sin(a) * r);
            }
            p.setFill(0);
            p.setStroke((r == 48) ? 0xFF8AADF4 : 0xFFCBA6F7, 1.5f);
            p.drawPolygon(hex);
        }
    });

    pathCard->addChild(pathWidget);
    tabRoot->add(pathCard);

    // ---- Right Card: Affine Transforms & Intermeshing Gears ----
    auto gearCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto gearWidget = CustomWidget::create();
    gearWidget->setSize(470, 520);

    gearWidget->setPaint([](CustomWidget*, helios::Painter& p) {
        p.setFont({ "Segoe UI", 14.0f, helios::FontFlag::Bold });
        p.setFill(0xFF8AADF4);
        p.drawText("Mechanical Gear Train (Affine Transforms)", 18.0f, 16.0f);

        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::None });
        p.setFill(0xFFA6ADC8);
        p.drawText("Real-time affine matrix rotation with synchronized gear ratios", 18.0f, 38.0f);

        // Main drive gear (18 teeth, Radius 80)
        float cx1 = 175.0f, cy1 = 200.0f, r1 = 80.0f;
        int t1 = 18;
        DrawGear(p, cx1, cy1, r1, t1, g_state.gearAngle, 0xFF8AADF4);

        // Secondary driven gear (12 teeth, Radius 53.33)
        // Ratio = 18/12 = 1.5x speed in opposite direction
        float r2 = r1 * (12.0f / 18.0f);
        float dist = r1 + r2 + 6.0f;
        float anglePos = kPi * 0.15f;
        float cx2 = cx1 + std::cos(anglePos) * dist;
        float cy2 = cy1 + std::sin(anglePos) * dist;
        DrawGear(p, cx2, cy2, r2, 12, -g_state.gearAngle * 1.5f + 0.2f, 0xFFA6E3A1);

        // Third small pinion gear (9 teeth, Radius 40)
        // Ratio = 18/9 = 2.0x speed
        float r3 = r1 * (9.0f / 18.0f);
        float dist3 = r1 + r3 + 6.0f;
        float anglePos3 = -kPi * 0.55f;
        float cx3 = cx1 + std::cos(anglePos3) * dist3;
        float cy3 = cy1 + std::sin(anglePos3) * dist3;
        DrawGear(p, cx3, cy3, r3, 9, -g_state.gearAngle * 2.0f, 0xFFF5BDE6);

        // Bottom Info Card
        float infoY = 380.0f;
        p.setFill(0xFF181926);
        p.setStroke(0xFF313244, 1.0f);
        p.drawRoundRect(20.0f, infoY, 430.0f, 110.0f, 8.0f);

        p.setFont({ "Segoe UI", 12.0f, helios::FontFlag::Bold });
        p.setFill(0xFFCAD3F5);
        p.drawText("Kinematic Drive Metrics:", 34.0f, infoY + 16.0f);

        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::None });
        p.setFill(0xFFA6ADC8);
        p.drawText("> Primary Drive: 18T @ 1.0x Angular Velocity", 34.0f, infoY + 40.0f);
        p.drawText("> Intermediate Planet: 12T @ -1.5x Meshed Velocity", 34.0f, infoY + 60.0f);
        p.drawText("> High-Speed Pinion: 9T @ -2.0x Overdrive", 34.0f, infoY + 80.0f);
    });

    gearCard->addChild(gearWidget);
    tabRoot->add(gearCard);

    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 3: Gauges & Oscilloscope View
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabGauges() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: Analog Tachometer / Speedometer ----
    auto tachoCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto tachoWidget = CustomWidget::create();
    tachoWidget->setSize(470, 520);

    tachoWidget->setPaint([](CustomWidget*, helios::Painter& p) {
        p.setFont({ "Segoe UI", 14.0f, helios::FontFlag::Bold });
        p.setFill(0xFFF38BA8);
        p.drawText("Analog Precision Speedometer / Tachometer", 18.0f, 16.0f);

        float cx = 235.0f;
        float cy = 250.0f;
        float r = 160.0f;

        // Dial Bezel (Gradient-like concentric rings)
        p.setFill(0xFF181926);
        p.setStroke(0xFF45475A, 3.0f);
        p.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);

        // Color Arc Zones:
        // Semicircular range: 135 deg to 405 deg (270 deg span)
        float startAngle = 135.0f;
        // Green zone (0 - 80 km/h) -> 135 to 270 deg (135 deg span)
        p.setFill(0);
        p.setStroke(0xFFA6E3A1, 6.0f);
        p.drawArc(cx - r + 14.0f, cy - r + 14.0f, (r - 14.0f) * 2.0f, (r - 14.0f) * 2.0f, 135.0f, 135.0f);

        // Amber zone (80 - 120 km/h) -> 270 to 337.5 deg (67.5 deg span)
        p.setStroke(0xFFF9E2AF, 6.0f);
        p.drawArc(cx - r + 14.0f, cy - r + 14.0f, (r - 14.0f) * 2.0f, (r - 14.0f) * 2.0f, 270.0f, 67.5f);

        // Red danger zone (120 - 160 km/h) -> 337.5 to 405 deg (67.5 deg span)
        p.setStroke(0xFFF38BA8, 6.0f);
        p.drawArc(cx - r + 14.0f, cy - r + 14.0f, (r - 14.0f) * 2.0f, (r - 14.0f) * 2.0f, 337.5f, 67.5f);

        // Major and Minor Tick Marks
        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::Bold });

        for (int val = 0; val <= 160; val += 20) {
            float frac = (float)val / 160.0f;
            float deg = startAngle + frac * 270.0f;
            float rad = deg * (kPi / 180.0f);

            float cosA = std::cos(rad);
            float sinA = std::sin(rad);

            float x1 = cx + cosA * (r - 28.0f);
            float y1 = cy + sinA * (r - 28.0f);
            float x2 = cx + cosA * (r - 16.0f);
            float y2 = cy + sinA * (r - 16.0f);

            p.setStroke((val >= 120) ? 0xFFF38BA8 : 0xFFCAD3F5, 2.5f);
            p.drawLine(x1, y1, x2, y2);

            // Number Label
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", val);
            float lx = cx + cosA * (r - 46.0f) - 8.0f;
            float ly = cy + sinA * (r - 46.0f) - 6.0f;
            p.setFill(0xFFA6ADC8);
            p.drawText(buf, lx, ly);
        }

        // Animated Needle with Spring Physics Simulation
        float targetVal = 70.0f + 55.0f * std::sin(g_state.timeSec * 1.8f) + 12.0f * std::cos(g_state.timeSec * 4.2f);
        targetVal = std::clamp(targetVal, 0.0f, 160.0f);
        g_state.tachometerVal += (targetVal - g_state.tachometerVal) * 0.15f;

        float needleFrac = g_state.tachometerVal / 160.0f;
        float needleDeg = startAngle + needleFrac * 270.0f;
        float needleRad = needleDeg * (kPi / 180.0f);

        float nx = cx + std::cos(needleRad) * (r - 26.0f);
        float ny = cy + std::sin(needleRad) * (r - 26.0f);
        float tailX = cx - std::cos(needleRad) * 22.0f;
        float tailY = cy - std::sin(needleRad) * 22.0f;

        p.setStroke(0xFFF38BA8, 3.5f);
        p.drawLine(tailX, tailY, nx, ny);

        // Center Pivot Hub
        p.setFill(0xFF313244);
        p.setStroke(0xFFCAD3F5, 2.0f);
        p.drawEllipse(cx - 14.0f, cy - 14.0f, 28.0f, 28.0f);

        // Digital Readout Display
        p.setFill(0xFF11111B);
        p.setStroke(0xFF313244, 1.0f);
        p.drawRoundRect(cx - 75.0f, cy + 60.0f, 150.0f, 48.0f, 8.0f);

        char digBuf[32];
        std::snprintf(digBuf, sizeof(digBuf), "%.1f", g_state.tachometerVal);
        p.setFont({ "Segoe UI", 20.0f, helios::FontFlag::Bold });
        p.setFill(0xFFF38BA8);

        helios::TextMetrics dm{};
        p.measureText(digBuf, dm);
        p.drawText(digBuf, cx - dm.width / 2.0f - 18.0f, cy + 72.0f);

        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::Bold });
        p.setFill(0xFF6E738D);
        p.drawText("KM/H", cx + 22.0f, cy + 78.0f);
    });

    tachoCard->addChild(tachoWidget);
    tabRoot->add(tachoCard);

    // ---- Right Card: Dual-Channel Digital Oscilloscope ----
    auto oscCard = Card::create(470, 520, 0xFF1E1E2E, 0xFF313244);
    auto oscWidget = CustomWidget::create();
    oscWidget->setSize(470, 520);

    oscWidget->setPaint([](CustomWidget*, helios::Painter& p) {
        p.setFont({ "Segoe UI", 14.0f, helios::FontFlag::Bold });
        p.setFill(0xFFA6E3A1);
        p.drawText("Dual-Channel Live CRT Oscilloscope (60FPS)", 18.0f, 16.0f);

        float scrX = 20.0f;
        float scrY = 48.0f;
        float scrW = 430.0f;
        float scrH = 340.0f;

        // CRT Screen Frame
        p.setFill(0xFF0D1117);
        p.setStroke(0xFF238636, 1.5f);
        p.drawRoundRect(scrX, scrY, scrW, scrH, 10.0f);

        // Screen Grid (Phosphor Grid)
        p.save();
        p.setClipRect(scrX, scrY, scrW, scrH);

        // Grid lines
        p.setStroke(0x30238636, 1.0f);
        for (float x = scrX; x < scrX + scrW; x += 35.0f) {
            p.drawLine(x, scrY, x, scrY + scrH);
        }
        for (float y = scrY; y < scrY + scrH; y += 35.0f) {
            p.drawLine(scrX, y, scrX + scrW, y);
        }

        // Center crosshairs
        float midX = scrX + scrW / 2.0f;
        float midY = scrY + scrH / 2.0f;
        p.setStroke(0x60238636, 1.5f);
        p.drawLine(scrX, midY, scrX + scrW, midY);
        p.drawLine(midX, scrY, midX, scrY + scrH);

        // Channel 1: High-Frequency Sine Carrier (Emerald Green Glow)
        const int samples = 140;
        std::vector<float> ch1Pts;
        ch1Pts.reserve(samples * 2);
        for (int i = 0; i < samples; ++i) {
            float x = scrX + (float)i / (samples - 1) * scrW;
            float t = (float)i * 0.08f + g_state.timeSec * 8.0f;
            float y = midY - 30.0f + std::sin(t) * 45.0f;
            ch1Pts.push_back(x);
            ch1Pts.push_back(y);
        }
        p.setStroke(0x40A6E3A1, 5.0f); // Bloom glow
        p.drawPolyline(ch1Pts);
        p.setStroke(0xFFA6E3A1, 1.8f); // Sharp core
        p.drawPolyline(ch1Pts);

        // Channel 2: Modulated Low-Frequency Signal (Cyan Glow)
        std::vector<float> ch2Pts;
        ch2Pts.reserve(samples * 2);
        for (int i = 0; i < samples; ++i) {
            float x = scrX + (float)i / (samples - 1) * scrW;
            float t = (float)i * 0.04f + g_state.timeSec * 4.0f;
            float env = std::sin(t * 0.5f);
            float y = midY + 40.0f + std::sin(t * 2.5f) * env * 55.0f;
            ch2Pts.push_back(x);
            ch2Pts.push_back(y);
        }
        p.setStroke(0x4089DCEB, 5.0f); // Bloom glow
        p.drawPolyline(ch2Pts);
        p.setStroke(0xFF89DCEB, 1.8f); // Sharp core
        p.drawPolyline(ch2Pts);

        p.restore();

        // Channel Status Badges
        float badgeY = scrY + scrH + 18.0f;
        auto drawChBadge = [&p, badgeY](float bx, const char* name, const char* spec, uint32_t col) {
            p.setFill(0xFF181926);
            p.setStroke(col, 1.0f);
            p.drawRoundRect(bx, badgeY, 195.0f, 75.0f, 8.0f);

            p.setFont({ "Segoe UI", 12.0f, helios::FontFlag::Bold });
            p.setFill(col);
            p.drawText(name, bx + 14.0f, badgeY + 12.0f);

            p.setFont({ "Segoe UI", 10.0f, helios::FontFlag::None });
            p.setFill(0xFFA6ADC8);
            p.drawText(spec, bx + 14.0f, badgeY + 36.0f);
            p.drawText("Probe: 1X | Coupling: DC", bx + 14.0f, badgeY + 52.0f);
        };

        drawChBadge(scrX + 10.0f, "CH 1: 50.0 mV/div", "Sine wave @ 2.45 kHz", 0xFFA6E3A1);
        drawChBadge(scrX + 225.0f, "CH 2: 100 mV/div", "AM Modulated @ 680 Hz", 0xFF89DCEB);
    });

    oscCard->addChild(oscWidget);
    tabRoot->add(oscCard);

    return tabRoot;
}

// ----------------------------------------------------------------------------
// Tab 5: Text Input (C++ TextField, real Chinese IME support)
// ----------------------------------------------------------------------------
static std::shared_ptr<Widget> CreateTabInput() {
    auto tabRoot = HStack::create(16, 0);

    // ---- Left Card: editable fields ----
    auto leftCard = Card::create(520, 520, 0xFF1E1E2E, 0xFF313244);
    auto leftStack = VStack::create(12, 18);

    auto titleL = Label::create("Text Fields (click one, then type)");
    titleL->setFontSize(16.0f)->setColor(0xFF8AADF4);
    leftStack->add(titleL);

    auto hint = Label::create("中文输入：把输入法切到拼音，点进输入框直接打；候选窗会跟着光标走。");
    hint->setFontSize(12.0f)->setColor(0xFFA6ADC8);
    leftStack->add(hint);

    // The live readout the change handlers write into
    auto output = Label::create("> (nothing typed yet)");
    output->setFontSize(12.0f)->setColor(0xFFA6E3A1);

    auto line = [&](const char* prefix, const char* placeholder, const char* initial) {
        auto row = HStack::create(10, 0);
        auto lbl = Label::create(prefix);
        lbl->setFontSize(12.5f)->setColor(0xFFCAD3F5);
        lbl->setSize(96, 30);

        auto field = TextField::create(372, 32);
        field->setPrefix("");
        field->setPlaceholder(placeholder);
        field->setText(initial);
        field->onChange([output, prefix](const std::string& value) {
            output->setText(std::format("{} = \"{}\"  ({} bytes UTF-8)", prefix, value, value.size()));
        });
        field->onSubmit([output](const std::string& value) {
            output->setText(std::format("submitted: \"{}\"", value));
        });

        row->add(lbl)->add(field);
        leftStack->add(row);
        return field;
    };

    line("Name:", "e.g. 张伟 / Alice", "");
    line("City:", "e.g. 北京 / Shanghai", "");
    line("Note:", "type anything, Enter submits", "");

    // Password-ish field: no initial select-all, max length in code points
    auto limitedRow = HStack::create(10, 0);
    auto limitedLbl = Label::create("Max 8:");
    limitedLbl->setFontSize(12.5f)->setColor(0xFFCAD3F5);
    limitedLbl->setSize(96, 30);
    auto limited = TextField::create(372, 32);
    limited->setPlaceholder("at most 8 characters");
    limited->setMaxLength(8);
    limited->setSelectAllOnFocus(false);
    limited->onChange([output](const std::string& v) {
        output->setText(std::format("Max8 = \"{}\" ({} chars max 8)", v, v.size()));
    });
    limitedRow->add(limitedLbl)->add(limited);
    leftStack->add(limitedRow);

    leftStack->add(output);

    auto notes = Label::create("Keys: Left/Right, Home/End, Shift+arrows to select, Backspace/Delete,\nCtrl+A/C/X/V, Enter to submit. IME composition shows underlined in the field.");
    notes->setFontSize(11.5f)->setColor(0xFF6E738D);
    leftStack->add(notes);

    leftCard->addChild(leftStack);
    tabRoot->add(leftCard);

    // ---- Right Card: how the input path is wired ----
    auto rightCard = Card::create(420, 520, 0xFF1E1E2E, 0xFF313244);
    auto rightStack = VStack::create(10, 18);

    auto titleR = Label::create("Input Pipeline");
    titleR->setFontSize(16.0f)->setColor(0xFFA6E3A1);
    rightStack->add(titleR);

    auto pipeline = Label::create(
        "WM_CHAR (IME commit arrives here too)\n"
        "  -> host child window (it has focus)\n"
        "  -> heliosview_host_ui_dispatch_text\n"
        "  -> focused widget's text callback\n"
        "  -> Widget::onTextInput (C++)\n"
        "\n"
        "WM_KEYDOWN\n"
        "  -> main window event queue\n"
        "  -> helios::App -> Window::event\n"
        "  -> heliosview_host_ui_dispatch_key\n"
        "  -> focused widget's key callback\n"
        "\n"
        "WM_IME_COMPOSITION\n"
        "  -> heliosview_host_ui_dispatch_composition\n"
        "  -> Widget::onComposition (drawn underlined)\n"
        "  -> heliosview_ui_widget_report_ime_caret\n"
        "  -> ImmSetCompositionWindow / ImmSetCandidateWindow");
    pipeline->setFontSize(11.5f)->setColor(0xFFCDD6F4);
    rightStack->add(pipeline);

    rightCard->addChild(rightStack);
    tabRoot->add(rightCard);

    return tabRoot;
}

} // namespace

// ----------------------------------------------------------------------------
// Main Application Context & Window Host Wiring
// ----------------------------------------------------------------------------
int main() {
    std::cout << "===========================================================\n";
    std::cout << " HeliosView 2D DirectDraw & UI Component Suite Showcase\n";
    std::cout << "===========================================================\n";

    // The application object owns the message loop (helios::App::exec); frames are
    // driven by App::frameCallback, native events by the Window's signals.
    helios::App app;

    helios::Window win(1020, 800, "HeliosView 2D DirectDraw & Retained UI Suite",
                       helios::WindowStyle::Normal);
    s_win = &win;

    // Right-click context popup menu
    s_contextMenu = std::make_unique<helios::Menu>();
    auto* itemMax = s_contextMenu->addItem("Toggle Maximize");
    itemMax->triggered.connect([] { if (s_win) s_win->toggleMaximize(); });

    auto* itemFs = s_contextMenu->addItem("Toggle Fullscreen");
    itemFs->triggered.connect([] {
        static bool fs = false;
        fs = !fs;
        if (s_win) s_win->setFullscreen(fs);
    });

    s_contextMenu->addSeparator();

    auto* itemPicker = s_contextMenu->addItem("Open File Picker...");
    itemPicker->triggered.connect([] {
        auto files = helios::openFiles(s_win ? s_win->nativeHandle() : nullptr, "Context Menu File Picker");
        if (!files.empty()) std::cout << "[Menu] Selected: " << files.front() << "\n";
    });

    auto* itemToast = s_contextMenu->addItem("Send OS Toast Notification");
    itemToast->triggered.connect([] {
        helios::notificationShow("HeliosView Context Menu", "Triggered from right-click native context menu!");
    });

    s_contextMenu->addSeparator();

    auto* itemExit = s_contextMenu->addItem("Exit Gallery");
    itemExit->triggered.connect([] { if (auto* a = helios::App::instance()) a->quit(); });

    // DirectDraw UI viewport with the Blend2D engine. The host is attached to the
    // window (the window destroys it), and it keeps the widget tree alive.
    helios::UIHost host = win.createUIHost(0, 0, 1020, 800, HELIOSVIEW_ENGINE_BLEND2D);
    if (!host.valid()) return 1;

    // Root UI Tree (VStack)
    auto rootStack = VStack::create(16, 24);

    // Header Title & Engine Badge
    auto headerRow = HStack::create(16, 0);

    auto titleLbl = Label::create("HeliosView UI Gallery");
    titleLbl->setFontSize(22.0f);
    titleLbl->setColor(0xFFCAD3F5);
    headerRow->add(titleLbl);

    auto engineBadge = CustomWidget::create();
    engineBadge->setSize(260, 32);
    engineBadge->setPaint([](CustomWidget*, helios::Painter& p) {
        p.setFill(0xFF181926);
        p.setStroke(0xFF8AADF4, 1.0f);
        p.drawRoundRect(0, 0, 260.0f, 32.0f, 6.0f);

        p.setFont({ "Segoe UI", 11.0f, helios::FontFlag::Bold });
        p.setFill(0xFF8AADF4);
        p.drawText("Engine: BLEND2D (JIT X86_64) | 60 FPS", 16.0f, 8.0f);
    });
    headerRow->add(engineBadge);
    rootStack->add(headerRow);

    // Segmented Tab Selector (6 Tabs)
    std::vector<std::string> tabNames = {
        "OS Shell",
        "Controls",
        "Charts",
        "Vectors",
        "Gauges",
        "Input"
    };
    auto tabBar = SegmentedControl::create(tabNames, 0);
    tabBar->setSize(964, 40);
    rootStack->add(tabBar);

    // Tab Views
    auto tabShell = CreateTabShellIntegration();
    auto tab0 = CreateTabControls();
    auto tab1 = CreateTabCharts();
    auto tab2 = CreateTabVectors();
    auto tab3 = CreateTabGauges();
    auto tab4 = CreateTabInput();

    tab0->setVisible(false);
    tab1->setVisible(false);
    tab2->setVisible(false);
    tab3->setVisible(false);
    tab4->setVisible(false);

    rootStack->add(tabShell);
    rootStack->add(tab0);
    rootStack->add(tab1);
    rootStack->add(tab2);
    rootStack->add(tab3);
    rootStack->add(tab4);

    // Tab Switching Logic
    tabBar->onChange([tabShell, tab0, tab1, tab2, tab3, tab4, &host](int idx) {
        g_state.activeTab = idx;
        tabShell->setVisible(idx == 0);
        tab0->setVisible(idx == 1);
        tab1->setVisible(idx == 2);
        tab2->setVisible(idx == 3);
        tab3->setVisible(idx == 4);
        tab4->setVisible(idx == 5);
        host.requestRepaint();
    });

    // Attach the tree to the host (which now keeps it alive) and show the window
    host.setRootWidget(rootStack);
    win.show();

    // Native event wiring -- no polling loop: the Window dispatches into its signals
    win.closeRequested.connect([&win] { win.close(); });
    win.resized.connect([&host](int32_t w, int32_t h) {
        host.setBounds(0, 0, w, h);
    });
    win.mouseButtonPressed.connect([](int32_t, int32_t, helios::MouseButton button) {
        if (button == helios::MouseButton::Right && s_contextMenu && s_win) {
            s_contextMenu->show(s_win->nativeHandle());
        }
    });

    // Per-frame animation state, then a repaint request for the live widgets
    app.frameCallback = [&host] {
        g_state.timeSec += 0.016f;
        g_state.gearAngle += 0.035f;

        // Continuous 60 FPS repaint for animations & live gauges
        host.requestRepaint();
    };

    std::cout << "[UI] Gallery initialized with 6 interactive tabs (Tab 5: retained-mode text input with IME).\n";
    std::cout << "[UI] Tip: Right-click anywhere in window for Native Context Menu.\n";

    // Main 60 FPS Animation & Event Loop
    const int exitCode = app.exec();

    // Teardown: detach the tree while the host is still alive, then let the window
    // destroy the host it owns.
    host.clearRoot();
    rootStack.reset();
    tabShell.reset();
    tab0.reset();
    tab1.reset();
    tab2.reset();
    tab3.reset();
    tab4.reset();
    s_contextMenu.reset();
    s_tray.reset();
    s_subWindows.clear();
    win.close();
    s_win = nullptr;

    return exitCode;
}
