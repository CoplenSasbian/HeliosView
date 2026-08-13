cmake_minimum_required(VERSION 4.3)

project(HeliosView
        VERSION 0.1.0
        DESCRIPTION "HeliosView - a C++ viewer library"
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
#   - http-parser: node.js's HTTP/1.x parser (single C file).
#   - nlohmann/json (single header, exposed below as nlohmann_json::nlohmann_json).
#
# WebView2 SDK: downloaded from NuGet at configure time (a .nupkg is just a
# zip of headers + the WebView2Loader loader library/dll), so consumers do not
# need it from vcpkg.
#
# TLS for the async HTTP client (https://) uses Windows SChannel (SSPI), which
# is part of the OS: no third-party TLS library, no vcpkg, and no OpenSSL.
# Server certificates are validated against the Windows system store.

# nlohmann/json is header-only; expose it directly instead of running its build.
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include)

# stdexec: P2300 senders/receivers reference implementation (header-only),
# pinned to commit b783aac (March 2024) — vendored, not downloaded.
add_library(stdexec INTERFACE)
target_include_directories(stdexec INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include)
if(MSVC)
    # stdexec requires MSVC's conforming __cplusplus / preprocessor
    target_compile_options(stdexec INTERFACE /Zc:__cplusplus /Zc:preprocessor)
endif()

# http-parser: node.js's HTTP/1.x parser (single C file), used by the async HTTP
# client to parse responses. Vendored locally (no download).
add_library(http_parser STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/http-parser/http_parser.c)
target_include_directories(http_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/http-parser)

# heliosview_link_platform_system_libs(): links each backend's OS-specific
# system libraries (Win32 SChannel/Winsock, etc.) through one macro, so moving to
# another platform only means adding a branch there.
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

option(HELIOSVIEW_BUILD_EXAMPLES "Build the example executables" ON)
if(HELIOSVIEW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
