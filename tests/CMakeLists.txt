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

# Retained-UI input path: focus, committed text (typed keys and IME commits alike),
# key-driven editing and the composition preview, driven through the C ABI the way an
# application does.
add_executable(HeliosView_UiInputTest ui_input_test.cpp)
target_link_libraries(HeliosView_UiInputTest PRIVATE HeliosView::Core)
add_test(NAME ui_input COMMAND HeliosView_UiInputTest)
set_tests_properties(ui_input PROPERTIES LABELS "requires-desktop")

# The same path on a live window: launches the canvas UI demo, clicks into a text field
# and posts a Chinese WM_CHAR plus an IME composition round to the real host child
# window, which is the part a headless test cannot reach.
add_executable(HeliosView_UiLiveInputTest ui_live_input_test.cpp)
target_link_libraries(HeliosView_UiLiveInputTest PRIVATE HeliosView::Core)
target_compile_definitions(HeliosView_UiLiveInputTest
    PRIVATE HELIOSVIEW_CANVAS_UI_DEMO="$<TARGET_FILE:HeliosView_CanvasUiDemo>")
add_dependencies(HeliosView_UiLiveInputTest HeliosView_CanvasUiDemo)
add_test(NAME ui_live_input COMMAND HeliosView_UiLiveInputTest)
set_tests_properties(ui_live_input PROPERTIES LABELS "requires-desktop")

