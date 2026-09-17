if(MSVC)
    add_compile_options(/utf-8 /EHsc)
endif()

if(NOT TARGET HeliosView::Core)
    find_package(HeliosView REQUIRED)
endif()

# Canvas / painter / image unit test: headless pixel-level assertions
add_executable(HeliosView_CanvasTest canvas_test.cpp)
target_link_libraries(HeliosView_CanvasTest PRIVATE HeliosView::Core)

# PixelView & BufferPresenter unit test suite
add_executable(HeliosView_PresenterTest presenter_test.cpp)
target_link_libraries(HeliosView_PresenterTest PRIVATE HeliosView::Core)

# Comprehensive Non-UI subsystem test suite (Signals, Async, HTTP, System, String, Types, DTO)
add_executable(HeliosView_NonUiTest non_ui_test.cpp)
target_link_libraries(HeliosView_NonUiTest PRIVATE HeliosView::Core)

add_test(NAME canvas COMMAND HeliosView_CanvasTest)
add_test(NAME presenter COMMAND HeliosView_PresenterTest)
add_test(NAME non_ui COMMAND HeliosView_NonUiTest)
set_tests_properties(presenter PROPERTIES LABELS "requires-desktop")

