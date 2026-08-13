add_executable(HeliosViewDemo main.cpp)
target_link_libraries(HeliosViewDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewCoroDemo coro_demo.cpp)
target_link_libraries(HeliosViewCoroDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWindowDemo window_demo.cpp)
target_link_libraries(HeliosViewWindowDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewAppDemo app_demo.cpp)
target_link_libraries(HeliosViewAppDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWebViewDemo webview_demo.cpp)
target_link_libraries(HeliosViewWebViewDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewWebViewEventsDemo webview_events_demo.cpp)
target_link_libraries(HeliosViewWebViewEventsDemo PRIVATE HeliosView::Core)

add_executable(HeliosViewHttpDemo http_demo.cpp)
target_link_libraries(HeliosViewHttpDemo PRIVATE HeliosView::Core)
