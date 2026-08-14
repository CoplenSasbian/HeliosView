add_library(HeliosView SHARED
    heliosview.cpp   # platform-independent core: version, allocator, events, string codecs
)

# Platform backends: each platform adds its implementation files under
# src/<platform>/. The cross-platform API lives in include/HeliosView/heliosview.h;
# porting to another platform only requires rewriting these files.
if(WIN32)
    target_sources(HeliosView PRIVATE
        win32/heliosview_win32.cpp      # windows / message loop / native message translation / WebView2 / dialogs
        win32/heliosview_notify_win32.cpp # OS toast notifications (WinRT via the Windows SDK)
    )
    # This backend's OS system libraries (user32, shell32, ole32, dwmapi,
    # runtimeobject, propsys) come through the platform macro so the "which
    # libraries a platform needs" lives in one place (cmake/PlatformLibs.cmake).
    heliosview_link_platform_system_libs(HeliosView)

    # WebView2 SDK: header-only interface target populated at configure time
    # (see the top-level CMakeLists.txt), linking the WebView2Loader import lib.
    target_link_libraries(HeliosView PRIVATE WebView2::SDK)
    # WebView2Loader.dll must sit next to the exe (the static loader loads it at runtime)
    add_custom_command(TARGET HeliosView POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HELIOSVIEW_WEBVIEW2_LOADER_DLL}"
            $<TARGET_FILE_DIR:HeliosView>
    )
endif()

add_library(HeliosView::HeliosView ALIAS HeliosView)

target_include_directories(HeliosView PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)

# HELIOSVIEW_EXPORTS is defined only when building the library itself;
# HeliosViewExport.h then expands to __declspec(dllexport) (Windows) or
# visibility("default") (GCC/Clang)
target_compile_definitions(HeliosView PRIVATE
    HELIOSVIEW_EXPORTS
    HELIOSVIEW_VERSION_STR="${PROJECT_VERSION}"
)

# Symbols hidden by default on GCC/Clang; only HELIOSVIEW_API-marked ones are exported
set_target_properties(HeliosView PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_compile_options(HeliosView PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
