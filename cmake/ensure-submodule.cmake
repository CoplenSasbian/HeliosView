# ensure-submodule.cmake
#
# Ensures the pinned git submodules are checked out before the build graph is
# configured. `git submodule update` is idempotent, so this is safe on every
# configure; the cheap probe below skips git entirely on a populated tree.
#
# Exactly two git invocations are needed (no per-library loop):
#   1. HeliosView's own submodules: stdexec + the Boost superproject.
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

# Runs git with the remaining arguments, streaming its output. `cmake -E time`
# prefixes the elapsed time so long clones visibly make progress.
function(_heliosview_git description workdir)
    list(JOIN ARGN " " _cmd_str)
    message(STATUS "HeliosView: ${description}")
    message(STATUS "HeliosView:   git ${_cmd_str}   [in ${workdir}]")
    execute_process(
            COMMAND "${CMAKE_COMMAND}" -E time "${GIT_EXECUTABLE}" ${ARGN}
            WORKING_DIRECTORY "${workdir}"
            RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
                "HeliosView: 'git ${_cmd_str}' failed (exit ${_result}) in ${workdir}.\n"
                "git's own error is printed above — fix it (network/proxy/credentials) and re-run cmake.")
    endif()
endfunction()

if(NOT EXISTS "${HELIOSVIEW_PROJECT_ROOT}/.gitmodules")
    message(STATUS "HeliosView: .gitmodules not found; assuming submodules are pre-populated.")
    return()
endif()

_heliosview_uninitialized_submodules(_missing)
if(NOT _missing)
    message(STATUS "HeliosView: Submodules are already populated.")
    return()
endif()

find_package(Git QUIET)
if(NOT Git_FOUND)
    message(WARNING "HeliosView: git not found; cannot initialize submodules.")
else()
    list(LENGTH HELIOSVIEW_BOOST_LIBS _boost_lib_count)
    list(LENGTH _missing _missing_count)
    set(_missing_hint "${_missing}")
    if(_missing_count GREATER 8)
        list(SUBLIST _missing 0 8 _missing_hint)
        list(APPEND _missing_hint "... (${_missing_count} probe files total)")
    endif()
    message(STATUS "HeliosView: Uninitialized submodules (${_missing_count}): ${_missing_hint}")
    message(STATUS "HeliosView: Initializing submodules — this fetches the Boost superproject "
            "plus ${_boost_lib_count} Boost libraries and can take a while on a fresh clone.")

    # `--jobs` (git >= 2.30, like --progress) parallelizes the clones; drop it if
    # the cache variable was set to something unusable.
    set(_jobs_args --jobs ${HELIOSVIEW_SUBMODULE_JOBS})
    if(NOT HELIOSVIEW_SUBMODULE_JOBS MATCHES "^[1-9][0-9]*$")
        message(STATUS "HeliosView: HELIOSVIEW_SUBMODULE_JOBS='${HELIOSVIEW_SUBMODULE_JOBS}' "
                "is not a positive integer; cloning serially.")
        set(_jobs_args "")
    endif()

    # 1. HeliosView's own submodules: stdexec and the Boost superproject.
    _heliosview_git("Fetching HeliosView submodules (stdexec, Boost superproject)..."
            "${HELIOSVIEW_PROJECT_ROOT}"
            submodule update --init --depth 1 --progress ${_jobs_args}
            -- third_party/stdexec third_party/boost)

    # 2. All required Boost libraries in a single call.
    set(_boost_lib_paths "")
    foreach(_lib IN LISTS HELIOSVIEW_BOOST_LIBS)
        list(APPEND _boost_lib_paths "libs/${_lib}")
    endforeach()
    _heliosview_git("Fetching ${_boost_lib_count} Boost libraries in one call..."
            "${HELIOSVIEW_BOOST_ROOT}"
            submodule update --init --depth 1 --progress ${_jobs_args}
            -- ${_boost_lib_paths})
endif()

# Final check: a configure that continues here would fail later with a confusing
# "file not found" while compiling, so fail fast instead.
_heliosview_uninitialized_submodules(_still_missing)
if(_still_missing)
    message(FATAL_ERROR "HeliosView: submodules still not initialized: ${_still_missing}.")
endif()
message(STATUS "HeliosView: Submodules ready.")
