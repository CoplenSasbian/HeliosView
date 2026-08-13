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
# OpenSSL, nlohmann/json and the WebView2 SDK come from a vcpkg install tree
# (classic mode, pre-installed with `vcpkg install ...`). stdexec and
# http-parser are vendored locally (third_party/): stdexec is pinned to the
# commit HeliosView.Core was written against (newer releases removed the
# concept tags this code uses, and vcpkg only ships the new API), and
# http-parser's vcpkg port fetches from codeload.github.com, which is
# unreachable on some networks. No FetchContent: nothing is downloaded at
# configure time. Configure with:
#
#   vcpkg install openssl nlohmann-json webview2
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#
find_package(nlohmann_json CONFIG REQUIRED)

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
