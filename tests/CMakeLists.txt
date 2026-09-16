if(MSVC)
    add_compile_options(/utf-8)
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
