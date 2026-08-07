add_library(HeliosView SHARED
    heliosview.cpp
)

add_library(HeliosView::HeliosView ALIAS HeliosView)

target_include_directories(HeliosView PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)

# HELIOSVIEW_EXPORTS 只在编译库自身时定义：HeliosViewExport.h 据此
# 展开为 __declspec(dllexport)（Windows）/ visibility("default")（GCC/Clang）
target_compile_definitions(HeliosView PRIVATE
    HELIOSVIEW_EXPORTS
    HELIOSVIEW_VERSION_STR="${PROJECT_VERSION}"
)

# GCC/Clang 下默认隐藏，仅 HELIOSVIEW_API 标记的符号被导出
set_target_properties(HeliosView PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_compile_options(HeliosView PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
