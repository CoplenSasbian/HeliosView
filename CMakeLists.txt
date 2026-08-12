cmake_minimum_required(VERSION 4.3)

project(HeliosView
        VERSION 0.1.0
        DESCRIPTION "HeliosView - a C++ viewer library"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DLLs and demo exes land in the same directory, so no manual PATH setup at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------- Third-party dependencies (fetched via git) ----------

include(FetchContent)

# stdexec: reference implementation of P2300 senders/receivers (header-only).
# HeliosView.Core depends on it; it propagates to consumers (exes) via
# INTERFACE dependencies. Pinned to a main-branch commit for reproducible builds.
set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # don't build its own examples when vendored as a dependency
FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG b783aac907718fa0b92e99123883341142670d3e
    SYSTEM
)
# stdexec pulls rapids-cmake via a raw URL zip download, which bypasses git
# proxies and is very slow in CN networks. Pre-declare it as a git clone
# (honoring git's http.proxy / https.proxy) so stdexec's own URL declaration
# is ignored (CMake keeps the first declaration; stdexec's RAPIDS.cmake then
# sees the content already populated).
FetchContent_Declare(
    rapids-cmake
    GIT_REPOSITORY https://github.com/rapidsai/rapids-cmake.git
    GIT_TAG branch-24.02
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(stdexec)

# nlohmann/json: header-only JSON library used for the WebView bridge's
# auto-parameter-binding sugar (WebViewJson.h). Same idea as stdexec: fetched
# from the source, propagated to consumers via HeliosView::Core INTERFACE deps.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE) # don't build its own tests when vendored
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

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
target_link_libraries(HeliosView.Core INTERFACE HeliosView STDEXEC::stdexec nlohmann_json::nlohmann_json)

option(HELIOSVIEW_BUILD_EXAMPLES "Build the example executables" ON)
if(HELIOSVIEW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
