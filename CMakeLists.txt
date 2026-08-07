cmake_minimum_required(VERSION 4.3)

project(HeliosView
        VERSION 0.1.0
        DESCRIPTION "HeliosView - a C++ viewer library"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DLL 与演示 exe 输出到同一目录，运行时无需手动把 DLL 加到 PATH
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(src)

# HeliosView.Core：header-only C++ 封装层（INTERFACE 库，只传播头文件路径与链接依赖）。
# 使用者只需 target_link_libraries(... HeliosView::Core)，
# 即可获得 C++ 头文件、C API 头文件路径以及 HeliosView.lib 导入库。
add_library(HeliosView.Core INTERFACE)
add_library(HeliosView::Core ALIAS HeliosView.Core)
target_include_directories(HeliosView.Core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(HeliosView.Core INTERFACE HeliosView)

option(HELIOSVIEW_BUILD_EXAMPLES "Build the example executables" ON)
if(HELIOSVIEW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
