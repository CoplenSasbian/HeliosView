# The demo sources contain UTF-8 string literals (Chinese labels in the WebView
# pages). MSVC defaults to the system ANSI codepage, which would corrupt them;
# force UTF-8 for the demo TUs.
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(HeliosViewDemo main.cpp)
target_link_libraries(HeliosViewDemo PRIVATE HeliosView::Core )

add_executable(HeliosViewWindowDemo window_demo.cpp)
target_link_libraries(HeliosViewWindowDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewAppDemo app_demo.cpp)
target_link_libraries(HeliosViewAppDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewSystemDemo system_demo.cpp)
target_link_libraries(HeliosViewSystemDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWebViewDemo webview_demo.cpp)
target_link_libraries(HeliosViewWebViewDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWebViewEventsDemo webview_events_demo.cpp)
target_link_libraries(HeliosViewWebViewEventsDemo PRIVATE HeliosView::Core)

# Pure-C consumer: proves the C API header compiles as C99 and links cleanly.
add_executable(HeliosViewCDemo c_demo.c)
target_link_libraries(HeliosViewCDemo PRIVATE HeliosView)
