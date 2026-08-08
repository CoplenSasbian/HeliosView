#ifndef HELIOSVIEW_HELIOSVIEW_EXPORT_H
#define HELIOSVIEW_HELIOSVIEW_EXPORT_H

/**
 * Export macros for HeliosView.dll's C interface (a pure C header, includable
 * from C/C++).
 *
 *   - HELIOSVIEW_EXPORTS  defined only by the DLL's own build (see src/CMakeLists.txt);
 *                         expands to dllexport (MSVC) / visibility("default") (GCC/Clang)
 *   - Consumers (HELIOSVIEW_EXPORTS undefined) expand to dllimport (MSVC) / empty
 *   - When linking against a static library, define HELIOSVIEW_STATIC to disable both
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
