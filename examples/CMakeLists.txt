# The demos are normally built as part of the HeliosView top-level project
# (add_subdirectory). Built standalone against an installed SDK (e.g. the
# release zip), declare the project here - HeliosView::Core is then the
# imported target from find_package(HeliosView).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 4.3)
    project(HeliosViewExamples CXX C)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# The demo sources contain UTF-8 string literals (Chinese labels in the WebView
# pages). MSVC defaults to the system ANSI codepage, which would corrupt them;
# force UTF-8 for the demo TUs.
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT TARGET HeliosView::Core)
    find_package(HeliosView REQUIRED)
endif()

add_executable(HeliosViewDemo main.cpp)
target_link_libraries(HeliosViewDemo PRIVATE HeliosView::Core )

add_executable(HeliosViewWindowDemo window_demo.cpp)
target_link_libraries(HeliosViewWindowDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewAppDemo app_demo.cpp)
target_link_libraries(HeliosViewAppDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewSystemDemo system_demo.cpp)
target_link_libraries(HeliosViewSystemDemo PRIVATE HeliosView::Core)

# Console demo: Async pool (timers/sockets) + HttpClient (http/https GET, POST).
add_executable(HeliosViewAsyncHttpDemo async_http_demo.cpp)
target_link_libraries(HeliosViewAsyncHttpDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWebViewDemo webview_demo.cpp)
target_link_libraries(HeliosViewWebViewDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWebViewEventsDemo webview_events_demo.cpp)
target_link_libraries(HeliosViewWebViewEventsDemo PRIVATE HeliosView::Core)

# Pure-C consumer: proves the C API header compiles as C99 and links cleanly.
# HeliosView::HeliosView is the namespaced target in both build modes (alias in
# the top-level project, imported target via find_package).
add_executable(HeliosViewCDemo c_demo.c)
target_link_libraries(HeliosViewCDemo PRIVATE HeliosView::HeliosView)
