#ifndef HELIOSVIEW_HELIOSVIEW_EXPORT_H
#define HELIOSVIEW_HELIOSVIEW_EXPORT_H

/**
 * HeliosView.dll 的 C 接口导出宏（纯 C 头文件，可被 C/C++ 包含）。
 *
 *   - HELIOSVIEW_EXPORTS  仅由 DLL 自身构建定义（见 src/CMakeLists.txt），
 *                         展开为 dllexport（MSVC）/ visibility("default")（GCC/Clang）
 *   - 使用者（未定义 HELIOSVIEW_EXPORTS）展开为 dllimport（MSVC）/ 空
 *   - 若以静态库链接，定义 HELIOSVIEW_STATIC 屏蔽两者
 */

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(HELIOSVIEW_STATIC)
#    define HELIOSVIEW_API
#  elif defined(HELIOSVIEW_EXPORTS)
#    define HELIOSVIEW_API __declspec(dllexport)
#  else
#    define HELIOSVIEW_API __declspec(dllimport)
#  endif
#  define HELIOSVIEW_NO_EXPORT
#else /* GCC / Clang */
#  if defined(HELIOSVIEW_EXPORTS)
#    define HELIOSVIEW_API __attribute__((visibility("default")))
#  else
#    define HELIOSVIEW_API
#  endif
#  define HELIOSVIEW_NO_EXPORT __attribute__((visibility("hidden")))
#endif

#endif /* HELIOSVIEW_HELIOSVIEW_EXPORT_H */
