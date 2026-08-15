cmake_minimum_required(VERSION 4.3)

project(HeliosView
        VERSION 1.0.0
        DESCRIPTION "HeliosView - a C++ WebView windowing library"
        LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DLLs and demo exes land in the same directory, so no manual PATH setup at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------- Third-party dependencies ----------
#
# Git submodules (third_party/, fetched with `git submodule update --init
# --recursive`, header-only, nothing to compile):
#   - stdexec:     pinned to the commit HeliosView.Core was written against
#                  (newer releases removed the concept tags this code uses).
#   - nlohmann/json (single header, exposed below as nlohmann_json::nlohmann_json).
#
# WebView2 SDK: downloaded from NuGet at configure time (a .nupkg is just a
# zip of headers + the WebView2Loader loader library/dll), so consumers do not
# need it from vcpkg.
#
# Everything else comes from the OS: windowing, dialogs, taskbar, toasts
# (WinRT via the Windows SDK), DWM backdrop and TLS-free by design. No vcpkg.

# nlohmann/json is header-only; expose it directly instead of running its build.
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include>
    $<INSTALL_INTERFACE:include>)

# stdexec: P2300 senders/receivers reference implementation (header-only),
# pinned to commit b783aac (March 2024) — vendored, not downloaded.
add_library(stdexec INTERFACE)
target_include_directories(stdexec INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    # stdexec requires MSVC's conforming __cplusplus / preprocessor
    target_compile_options(stdexec INTERFACE /Zc:__cplusplus /Zc:preprocessor)
endif()

# heliosview_link_platform_system_libs(): links each backend's OS-specific
# system libraries (Win32 user32/shell32/ole32/dwmapi/runtimeobject/propsys,
# etc.) through one macro, so moving to another platform only means adding a
# branch there.
include(cmake/PlatformLibs.cmake)

if(WIN32)
    # WebView2 SDK: download the Microsoft.Web.WebView2 NuGet package at
    # configure time and extract the native headers + WebView2Loader lib/dll.
    set(HELIOSVIEW_WEBVIEW2_VERSION "1.0.4129.50")
    set(HELIOSVIEW_WEBVIEW2_ARCHIVE "${CMAKE_BINARY_DIR}/webview2.nupkg")
    set(HELIOSVIEW_WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2-sdk")
    if(NOT EXISTS "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${HELIOSVIEW_WEBVIEW2_VERSION}"
            "${HELIOSVIEW_WEBVIEW2_ARCHIVE}"
            STATUS _wv2_status)
        list(GET _wv2_status 0 _wv2_code)
        if(NOT _wv2_code EQUAL 0)
            list(GET _wv2_status 1 _wv2_err)
            message(FATAL_ERROR "Failed to download WebView2 SDK: ${_wv2_err}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${HELIOSVIEW_WEBVIEW2_ARCHIVE}" DESTINATION "${HELIOSVIEW_WEBVIEW2_ROOT}")
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(HELIOSVIEW_WEBVIEW2_ARCH x64)
    else()
        set(HELIOSVIEW_WEBVIEW2_ARCH x86)
    endif()
    add_library(WebView2::SDK INTERFACE IMPORTED)
    set_target_properties(WebView2::SDK PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/include"
        INTERFACE_LINK_LIBRARIES "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/${HELIOSVIEW_WEBVIEW2_ARCH}/WebView2Loader.dll.lib"
    )
    set(HELIOSVIEW_WEBVIEW2_LOADER_DLL
        "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/${HELIOSVIEW_WEBVIEW2_ARCH}/WebView2Loader.dll")
endif()

add_subdirectory(src)

# HeliosView.Core: header-only C++ wrapper (INTERFACE library; propagates only
# include paths and link dependencies). Consumers just target_link_libraries(
# ... HeliosView::Core) to get the C++ headers, C API header paths, and the
# HeliosView.lib import library.
add_library(HeliosView.Core INTERFACE)
add_library(HeliosView::Core ALIAS HeliosView.Core)
target_include_directories(HeliosView.Core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# stdexec + nlohmann propagate via INTERFACE: targets linking HeliosView::Core
# (e.g. the examples) automatically get their include paths and the C++23 requirement
target_link_libraries(HeliosView.Core INTERFACE HeliosView stdexec nlohmann_json::nlohmann_json)
# exported to consumers: any target linking HeliosView::Core is compiled as C++23
target_compile_features(HeliosView.Core INTERFACE cxx_std_23)

option(HELIOSVIEW_BUILD_EXAMPLES "Build the example executables" ON)
if(HELIOSVIEW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------- Install / packaging ----------
#
# Consumers (including FFI wrappers in other languages) link the built DLL/import
# lib and include the C header directly; C++ consumers use the exported
# HeliosView::Core INTERFACE target.
include(GNUInstallDirs)

install(TARGETS HeliosView
        EXPORT HeliosViewTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# HeliosView.Core (INTERFACE) and its vendored header-only deps are exported too,
# so C++ consumers get a working HeliosView::Core from the installed config.
# stdexec/nlohmann keep build-tree include paths (the source is vendored under
# third_party/); this is a self-hosting install.
install(TARGETS HeliosView.Core stdexec nlohmann_json EXPORT HeliosViewTargets)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/HeliosView
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/HeliosViewCore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Vendored header-only deps of HeliosView.Core (stdexec, nlohmann/json):
# install them into the same include tree so exported HeliosView::Core resolves.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "._clangd_helper_file.cpp" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(WIN32)
    # WebView2Loader.dll must sit next to the app exe at runtime.
    install(FILES "${HELIOSVIEW_WEBVIEW2_LOADER_DLL}"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT HeliosViewTargets
        FILE HeliosViewTargets.cmake
        NAMESPACE HeliosView::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeliosView)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/HeliosViewConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeliosView)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeliosView)
