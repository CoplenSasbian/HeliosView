# =============================================================================
# HeliosView Examples CMakeLists.txt
# =============================================================================
# Structured modular demos for HeliosView:
#   01_console_core/    - Core async tasks, thread pool, HTTP client, signals
#   02_window_shell/    - Native window styles, titlebars, menus, tray, dialogs
#   03_webview_bridge/  - WebView2, HTML5 dashboard, bidirectional RPC bridge
#   04_canvas_ui/       - Retained DirectDraw UI, custom components, layout
#   05_studio_hybrid/   - Flagship dual-viewport (UIHost + WebViewHost) showcase
#   06_c_api/           - Pure C99 ABI tour and FFI interop demo
# =============================================================================

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 4.3)
    project(HeliosViewExamples CXX C)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT TARGET HeliosView::Core)
    find_package(HeliosView REQUIRED)
endif()

# -----------------------------------------------------------------------------
# 01. Console Core & Async Demo
# -----------------------------------------------------------------------------
add_executable(HeliosView_ConsoleCoreDemo 01_console_core/console_core_demo.cpp)
target_link_libraries(HeliosView_ConsoleCoreDemo PRIVATE HeliosView::Core)

# -----------------------------------------------------------------------------
# 02. Native Window Shell Demo
# -----------------------------------------------------------------------------
add_executable(HeliosView_WindowShellDemo 02_window_shell/window_shell_demo.cpp)
target_link_libraries(HeliosView_WindowShellDemo PRIVATE HeliosView::Core)

# -----------------------------------------------------------------------------
# 03. Modern WebView & RPC Bridge Demo
# -----------------------------------------------------------------------------
add_executable(HeliosView_WebViewBridgeDemo 03_webview_bridge/webview_bridge_demo.cpp)
target_link_libraries(HeliosView_WebViewBridgeDemo PRIVATE HeliosView::Core)

# -----------------------------------------------------------------------------
# 04. Retained Canvas UI Demo
# -----------------------------------------------------------------------------
add_executable(HeliosView_CanvasUiDemo 04_canvas_ui/canvas_ui_demo.cpp)
target_link_libraries(HeliosView_CanvasUiDemo PRIVATE HeliosView::Core)

# -----------------------------------------------------------------------------
# 05. Studio Hybrid Dual-Viewport Showcase Demo
# -----------------------------------------------------------------------------
add_executable(HeliosView_StudioHybridDemo 05_studio_hybrid/studio_hybrid_demo.cpp)
target_link_libraries(HeliosView_StudioHybridDemo PRIVATE HeliosView::Core)

# -----------------------------------------------------------------------------
# 06. Pure C99 ABI Showcase Demo
# -----------------------------------------------------------------------------
add_executable(HeliosView_CApiDemo 06_c_api/c_api_demo.c)
target_link_libraries(HeliosView_CApiDemo PRIVATE HeliosView::HeliosView)
