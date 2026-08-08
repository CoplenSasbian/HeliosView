#pragma once

/**
 * HeliosView.Core -- namespace compat layer for C++26 <execution> (P2300 senders/receivers).
 *
 * Business code writes async logic in the std::execution namespace; this header provides it:
 *
 *   - If the standard library implements P2300 (__cpp_lib_execution >= 202406L)
 *     or HELIOSVIEW_USE_STD_EXECUTION is defined manually:
 *       just #include <execution> and use the standard std::execution
 *   - Otherwise (default: C++23 + stdexec):
 *       define STDEXEC_NAMESPACE=std::execution via stdexec's official migration
 *       mechanism, placing the whole stdexec implementation in std::execution
 *
 * Upgrading to C++26 switches to the stdlib implementation automatically; business code unchanged.
 * Note: stdexec is only a transitional implementation; do not use the stdexec:: prefix in business code.
 */

#include <version> /* __cpp_lib_execution feature-test macro (202406L for P2300) */

#if defined(HELIOSVIEW_USE_STD_EXECUTION) || (defined(__cpp_lib_execution) && __cpp_lib_execution >= 202406L)
#  define HELIOSVIEW_HAVE_STD_EXECUTION 1
#  include <execution>
#else
#  define STDEXEC_NAMESPACE std::execution /* stdexec's official migration macro: the implementation lands directly in std::execution */
#  include <stdexec/execution.hpp>
#endif
