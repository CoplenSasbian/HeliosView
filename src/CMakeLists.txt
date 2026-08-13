add_library(HeliosView SHARED
    heliosview.cpp
)

# Platform backends: each platform adds its implementation files under
# src/<platform>/. The cross-platform API lives in include/HeliosView/heliosview.h;
# porting to another platform only requires rewriting these files.
if(WIN32)
    target_sources(HeliosView PRIVATE
        win32/heliosview_win32.cpp        # windows / message loop / native message translation / WebView2
        win32/heliosview_io_win32.cpp     # IOCP thread pool + async TCP/file I/O
        win32/heliosview_http_win32.cpp   # async HTTP client (OpenSSL + http-parser over async socket I/O)
    )
    # OpenSSL / nlohmann-json: from vcpkg (manifest mode); http-parser: vendored
    # (see the top-level CMakeLists.txt).
    find_package(OpenSSL REQUIRED)
    target_link_libraries(HeliosView PRIVATE
        ws2_32 crypt32 OpenSSL::SSL OpenSSL::Crypto
        http_parser nlohmann_json::nlohmann_json
    )

    # WebView2 SDK: from the vcpkg `webview2` port (headers via the toolchain's
    # global include; the loader lib/dll from the installed tree).
    target_link_libraries(HeliosView PRIVATE
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/WebView2Loader.dll.lib"
    )
    # WebView2Loader.dll must sit next to the exe (the static loader loads it at runtime)
    add_custom_command(TARGET HeliosView POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/WebView2Loader.dll"
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
