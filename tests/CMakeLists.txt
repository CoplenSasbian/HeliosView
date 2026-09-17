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

# Input driven from inside the real message loop (App::exec), which is where a widget
# that measured text outside its paint call used to crash: a painter is only valid for
# the paint it belongs to, and typing happens between paints.
add_executable(HeliosView_UiExecInputTest ui_exec_input_test.cpp)
target_link_libraries(HeliosView_UiExecInputTest PRIVATE HeliosView::Core)
add_test(NAME ui_exec_input COMMAND HeliosView_UiExecInputTest)
set_tests_properties(ui_exec_input PROPERTIES LABELS "requires-desktop")

# WM_CHAR straight at a host child window: the message a keyboard or an IME commit
# delivers. Proves the text half of Chinese input independently of any IME.
add_executable(HeliosView_UiWmCharTest ui_wmchar_test.cpp)
target_link_libraries(HeliosView_UiWmCharTest PRIVATE HeliosView::Core)
add_test(NAME ui_wmchar COMMAND HeliosView_UiWmCharTest)
set_tests_properties(ui_wmchar PROPERTIES LABELS "requires-desktop")

# The IME half: attaches an IME context to a host window, starts a composition and
# writes a pre-edit string into that context. Windows then delivers WM_IME_COMPOSITION
# and the host has to read the string back (ImmGetCompositionStringW) and hand UTF-8 to
# the focused widget. Deterministic, unlike waiting for a user's IME to activate.
add_executable(HeliosView_UiImeComposeTest ui_ime_compose_test.cpp)
target_link_libraries(HeliosView_UiImeComposeTest PRIVATE HeliosView::Core)
add_test(NAME ui_ime_compose COMMAND HeliosView_UiImeComposeTest)
set_tests_properties(ui_ime_compose PROPERTIES LABELS "requires-desktop")

# Editing keys with real keyboard input (SendInput): Backspace, Delete, Home, End,
# arrows and the Ctrl+A/C/V clipboard shortcuts. The widget reads the Ctrl/Shift state
# from the message-time keyboard state, which only real input can set.
add_executable(HeliosView_UiKeysTest ui_keys_test.cpp)
target_link_libraries(HeliosView_UiKeysTest PRIVATE HeliosView::Core)
add_test(NAME ui_keys COMMAND HeliosView_UiKeysTest)
set_tests_properties(ui_keys PROPERTIES LABELS "requires-desktop")

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


# TextField buffer integrity with mixed ASCII / multi-byte text: byte loss when editing
# in the middle of a string is what a byte-oriented edit does to UTF-8.
add_executable(HeliosView_UiTextFieldTest ui_textfield_test.cpp)
target_link_libraries(HeliosView_UiTextFieldTest PRIVATE HeliosView::Core)
add_test(NAME ui_textfield COMMAND HeliosView_UiTextFieldTest)
set_tests_properties(ui_textfield PROPERTIES LABELS "requires-desktop")
