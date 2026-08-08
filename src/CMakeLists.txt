add_library(HeliosView SHARED
    heliosview.cpp
)

# Platform backends: each platform adds its implementation files under
# src/<platform>/. The cross-platform API lives in include/HeliosView/heliosview.h;
# porting to another platform only requires rewriting these files.
if(WIN32)
    target_sources(HeliosView PRIVATE
        win32/heliosview_win32.cpp    # windows / message loop / native message translation / WebView2
        win32/heliosview_io_win32.cpp # IOCP thread pool + async TCP/file I/O
    )
    target_link_libraries(HeliosView PRIVATE ws2_32 nlohmann_json::nlohmann_json)

    # WebView2 SDK: pulled straight from the official NuGet package (same idea
    # as stdexec: fetch from the package source, no vcpkg).
    # Version kept in sync with https://www.nuget.org/packages/Microsoft.Web.WebView2.
    include(FetchContent)
    FetchContent_Declare(
        webview2_sdk
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3800.47
        URL_HASH SHA512=10048ce88c166b7f29a563fcdb9487d71bac5723777cd0a98b5c5a0e71cff344551a0bc27410b0cb0f8482a9ecdec7454a45ad0d2a7ae998fc347ead15598889
    )
    FetchContent_MakeAvailable(webview2_sdk)

    target_include_directories(HeliosView PRIVATE
        ${webview2_sdk_SOURCE_DIR}/build/native/include
    )
    target_link_libraries(HeliosView PRIVATE
        ${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib
    )
    # WebView2Loader.dll must sit next to the exe (the static loader loads it at runtime)
    add_custom_command(TARGET HeliosView POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2Loader.dll
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
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
