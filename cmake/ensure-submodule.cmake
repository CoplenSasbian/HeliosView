# ensure-submodule.cmake
#
# Ensures the pinned git submodules are checked out before the build graph is
# configured. `git submodule update` is idempotent, so this is safe on every
# configure; the cheap probe below skips git entirely on a populated tree.
#
# Exactly two git invocations are needed (no per-library loop):
#   1. HeliosView's own submodules: stdexec, the Boost superproject and the
#      blend2d + asmjit pair that the canvas layer's BUILTIN engine is built on.
#   2. Every required Boost library in ONE call, run inside third_party/boost.
#      Nested pathspecs are relative to the Boost superproject — from the
#      HeliosView root git rejects them ("pathspec 'third_party/boost/libs/asio'
#      did not match any file(s) known to git") — so the Boost libs need their
#      own call, but all of them go into a single `git submodule update`, which
#      clones them in parallel via --jobs.
#
# git's output is deliberately NOT captured: a silent multi-minute clone looks
# like a hang. --progress forces the clone progress bars even though git's
# stderr is not a terminal here, and each call is timed so the log shows it is
# alive. Failures therefore also print git's own message, in place.

get_filename_component(HELIOSVIEW_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(HELIOSVIEW_BOOST_ROOT "${HELIOSVIEW_PROJECT_ROOT}/third_party/boost")

# Boost libraries HeliosView uses. Also drives the include directories and the
# Boost::* aliases in CMakeLists.txt.
set(HELIOSVIEW_BOOST_LIBS
        asio beast json container endian variant2 compat system config core assert throw_exception static_assert
        type_traits utility detail winapi move
        align mp11 predef optional
        smart_ptr bind intrusive logic static_string container_hash describe io
)

# Parallel clones for `git submodule update --jobs` (~30 small repositories;
# fetching them one after another is the slow part of a fresh clone).
set(HELIOSVIEW_SUBMODULE_JOBS 4 CACHE STRING
        "Parallel git clone jobs used when initializing HeliosView submodules")

# One `.git` marker per submodule: git writes that gitfile when a submodule is
# initialized, so it is an exact "is it there?" test. Deliberately not a content
# file — `git submodule update` does not restore files deleted inside an already
# initialized submodule, so a content probe would turn a locally modified
# submodule into a hard configure failure.
set(_heliosview_submodule_markers
        third_party/stdexec/.git
        third_party/boost/.git
        third_party/blend2d/.git
        third_party/asmjit/.git
)
foreach(_lib IN LISTS HELIOSVIEW_BOOST_LIBS)
    list(APPEND _heliosview_submodule_markers "third_party/boost/libs/${_lib}/.git")
endforeach()

function(_heliosview_uninitialized_submodules out_var)
    set(_missing "")
    foreach(_rel IN LISTS _heliosview_submodule_markers)
        if(NOT EXISTS "${HELIOSVIEW_PROJECT_ROOT}/${_rel}")
            list(APPEND _missing "${_rel}")
        endif()
    endforeach()
    set(${out_var} "${_missing}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${HELIOSVIEW_PROJECT_ROOT}/.gitmodules")
    message(STATUS "HeliosView: .gitmodules not found; assuming submodules are pre-populated.")
    return()
endif()

_heliosview_uninitialized_submodules(_missing)
if(_missing)
    list(LENGTH _missing _missing_count)
    set(_missing_hint "${_missing}")
    if(_missing_count GREATER 8)
        list(SUBLIST _missing 0 8 _missing_hint)
        list(APPEND _missing_hint "... (${_missing_count} total)")
    endif()
    message(FATAL_ERROR
        "\n======================================================================\n"
        "HeliosView: Git submodules are missing or uninitialized (${_missing_count} missing):\n"
        "  ${_missing_hint}\n\n"
        "CMake no longer downloads or clones submodules automatically during configure\n"
        "to prevent network hangs and repeated re-downloads.\n\n"
        "Please run the setup script to initialize submodules and dependencies:\n"
        "  PowerShell:  .\\scripts\\setup-dependencies.ps1\n"
        "  Or Python:   python scripts/setup-dependencies.py\n"
        "======================================================================\n")
endif()

message(STATUS "HeliosView: Submodules verified.")
