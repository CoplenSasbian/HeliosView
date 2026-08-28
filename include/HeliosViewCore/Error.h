#pragma once

/**
 * HeliosView.Core -- Error helpers: turn a failed C call's recorded reason
 * (heliosview_last_error / heliosview_last_error_string) into an exception
 * with a concatenated, human-readable message. Every C call that fails records
 * its reason at the failure site (see "Error reporting" in heliosview.h); these
 * helpers read it back right after the call.
 */

#include <HeliosView/heliosview.h>

#include <stdexcept>
#include <string>

namespace helios {

/* Build "<operation>: <reason recorded at the failure site> (error <code>)"
 * from the C layer's thread-local last error. Call right after a C call failed
 * on this thread; falls back to the bare operation when nothing was recorded. */
inline std::string lastErrorDescription(const char* operation)
{
    std::string msg = operation && *operation ? operation : "heliosview call failed";
    const int code = heliosview_last_error();
    if (code != 0) {
        char buf[256];
        if (heliosview_last_error_string(buf, sizeof(buf)) == 0 && buf[0])
            msg += ": " + std::string(buf);
        msg += " (error " + std::to_string(code) + ")";
    }
    return msg;
}

/* Throw lastErrorDescription(operation) as Ex (default std::runtime_error);
 * e.g. throwLastError<std::invalid_argument>("bind") where the failure is an
 * argument-validation error. */
template <class Ex = std::runtime_error>
[[noreturn]] inline void throwLastError(const char* operation)
{
    throw Ex(lastErrorDescription(operation));
}

} // namespace helios