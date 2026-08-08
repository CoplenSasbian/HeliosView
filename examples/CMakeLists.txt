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
