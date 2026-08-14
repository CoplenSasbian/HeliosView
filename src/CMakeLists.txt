add_library(HeliosView SHARED
    heliosview.cpp
    heliosview_http.cpp   # async HTTP client core (platform-neutral: URL/wire/parser/state machine)
)

# Platform backends: each platform adds its implementation files under
# src/<platform>/. The cross-platform API lives in include/HeliosView/heliosview.h;
# porting to another platform only requires rewriting these files (plus the
# TLS backend the HTTP core drives through hv_tls_ops).
if(WIN32)
    target_sources(HeliosView PRIVATE
        win32/heliosview_win32.cpp        # windows / message loop / native message translation / WebView2
        win32/heliosview_io_win32.cpp     # IOCP thread pool + async TCP/file I/O
        win32/heliosview_tls_win32.cpp    # SChannel (SSPI) TLS backend for the async HTTP client
    )
    # This backend's OS system libraries (Winsock, SChannel/SSPI, CertStore,
    # user32/shell/COM) come through the platform macro so the "which libraries a
    # platform needs" lives in one place (cmake/PlatformLibs.cmake). It links:
    #   ws2_32 crypt32 secur32 user32 shell32 ole32
    heliosview_link_platform_system_libs(HeliosView)

    # Third-party, non-OS deps: http-parser (vendored) is the only compiled
    # dependency the DLL needs. The WebView2 bridge envelope parsing is a
    # hand-rolled lightweight scanner (see heliosview_win32.cpp), so the C
    # library has no nlohmann dependency -- nlohmann is only needed by the C++
    # Core headers (HeliosView::Core, for Http.h / WebViewJson.h).
    target_link_libraries(HeliosView PRIVATE
        http_parser
    )

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
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
