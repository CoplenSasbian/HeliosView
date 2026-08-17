
# ensure-submodule.cmake
#
# Ensures HeliosView's vendored third_party/ git submodules are present and
# healthILY checked out at configure time. It is safe to run on every
# configure: checks that already look good are no-ops, and broken/empty/half-
# cloned checkouts (empty worktree from a failed shallow fetch, dangling
# `refs/heads/.invalid` HEAD, missing gitlink in the index, etc.) are
# detected and repaired automatically. A true failure (e.g. no network on a
# brand-new clone) produces a clear FATAL_ERROR instead of a cryptic build
# failure later.
#
# Three layers are handled:
#   1. The two DIRECT submodules HeliosView vendors: stdexec (P2300
#      senders/receivers, the async backend) and nlohmann/json (single
#      header). These are header-only and their gitlinks live in the
#      superproject index.
#   2. The Boost SUPERPROJECT (third_party/boost), which is itself a git
#      submodule. Only the library subset HeliosView needs is initialized
#      (HELIOSVIEW_BOOST_LIBS, below) — never `--recursive`, which would
#      fetch all ~160 Boost libraries.
#   3. The nested Boost-library submodules under third_party/boost/libs/*.
#
# NOTE on network speed: every `git submodule update` used here passes
# `--depth 1`. A full clone of stdexec (very large history) — or the whole
# Boost superproject + its libs — routinely stalls through slow GitHub
# mirrors (this project's global `url.https://ghproxy.net/...` insteadof
# rewrite routes GitHub through ghproxy.net). A shallow clone of the pinned
# commit completes quickly and is all we ever need, because the recorded
# commit is reachable as the gitlink target.
#
# NOTE on pathspecs: `git submodule update` requires full repo-relative paths
# as pathspecs. Bare names like `stdexec` (without `third_party/` prefix) are
# rejected by git with "pathspec did not match any file(s) known to git", so
# the update commands below use HELIOSVIEW_PARENT_DIR_SUBMODULES (the
# `third_party/...` form), while marker checks and the repair loop iterate
# over the bare-name list HELIOSVIEW_DIRECT_SUBMODULES.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules")
    find_package(Git QUIET)
    if(Git_FOUND)
        set(HELIOSVIEW_DIRECT_SUBMODULES stdexec json)
        # `git submodule update` requires full repo-relative paths as
        # pathspecs (bare names like `stdexec` fail with "pathspec did not
        # match any file(s)"); kept separate from the bare-name list above,
        # which the marker checks and repair loop iterate over.
        set(HELIOSVIEW_PARENT_DIR_SUBMODULES third_party/stdexec third_party/json)
        set(_json_ok FALSE)
        set(_stdexec_ok FALSE)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/exec/asio/asio_config.hpp.in")
            set(_stdexec_ok TRUE)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include/nlohmann/json.hpp")
            set(_json_ok TRUE)
        endif()

        # If a direct submodule's gitlink has vanished from the index (staged
        # as deleted, e.g. after an accidental `git rm` or a botched earlier
        # restore, which breaks `git submodule update ... <path>` with "pathspec
        # did not match any file(s) known to git"), restore it from the
        # superproject HEAD before anything that needs it. Uses `git reset --
        # <path>`, which copies the gitlink from HEAD into the index without
        # touching the working tree.
        function(_heliosview_sub_restore_gitlinks)
            set(_need_restore "")
            foreach(_sm IN LISTS HELIOSVIEW_DIRECT_SUBMODULES)
                execute_process(
                        COMMAND ${GIT_EXECUTABLE} ls-files --error-unmatch -- third_party/${_sm}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE _in_index)
                if(NOT _in_index EQUAL 0)
                    list(APPEND _need_restore third_party/${_sm})
                endif()
            endforeach()
            if(_need_restore)
                message(STATUS "HeliosView: Restoring missing gitlinks in index for: ${_need_restore}")
                execute_process(
                        COMMAND ${GIT_EXECUTABLE} reset -q -- ${_need_restore}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE _rst_result
                        ERROR_QUIET)
            endif()
        endfunction()

        function(_heliosview_sub_init)
            # A submodule whose gitlink is missing from the index makes
            # `submodule update` fail with "pathspec did not match any
            # file(s)"; restore gitlinks first so the update can find them.
            _heliosview_sub_restore_gitlinks()
            # --depth 1: a full clone of stdexec (large history) routinely
            # stalls through slow GitHub mirrors (like this repo's ghproxy
            # insteadof rewrite), while a shallow clone of the pinned commit
            # completes quickly. Mirrors the shallow-fetch strategy the Boost
            # block below uses. The recorded commit is reachable as the
            # gitlink target, so shallow is correct (we never need history).
            message(STATUS "HeliosView: fetching direct submodules ${HELIOSVIEW_DIRECT_SUBMODULES} (shallow; may take a while on first clone)...")
            set(_sub_init_epoch "")
            string(TIMESTAMP _sub_init_epoch "%s")
            execute_process(
                    COMMAND ${GIT_EXECUTABLE} submodule update --init --depth 1 ${HELIOSVIEW_PARENT_DIR_SUBMODULES}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    RESULT_VARIABLE _init_result
                    ERROR_VARIABLE _init_error)
            string(TIMESTAMP _sub_init_done "%s")
            math(EXPR _sub_init_secs "${_sub_init_done} - ${_sub_init_epoch}")
            if(_init_result EQUAL 0)
                message(STATUS "HeliosView: direct submodules OK (${_sub_init_secs}s)")
            else()
                message(STATUS "HeliosView: direct submodule update finished with status ${_init_result} after ${_sub_init_secs}s")
            endif()
            set(_init_result "${_init_result}" PARENT_SCOPE)
            set(_init_error "${_init_error}" PARENT_SCOPE)
        endfunction()

        # Normalize each direct submodule's git HEAD to the commit the
        # superproject's tree records for it, then materialize its worktree.
        # A repeated `submodule update` normally does both, but a failed
        # shallow fetch can leave a submodule with a dangling
        # `refs/heads/.invalid` HEAD (which makes `git submodule update`
        # itself fail with "Unable to find current revision"), or an empty
        # index. Reading the recorded commit from the superproject and
        # checking it out explicitly fixes both.
        #
        # Returns the recorded commit for a submodule in PARENT_SCOPE var
        # <_sm>_commit ("" if it cannot be read).
        function(_heliosview_sub_recorded_commit _sm)
            execute_process(
                    COMMAND ${GIT_EXECUTABLE} ls-tree HEAD -- third_party/${_sm}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE _ls
                    ERROR_QUIET)
            # 160000 commit <hash>\tthird_party/<name>  (CMake regex does not
            # support {40} brace quantifiers, so use + and rely on the hash
            # being the only long hex run in a 160000 gitlink line)
            string(REGEX MATCH "160000 commit ([0-9a-f]+)" _m "${_ls}")
            if(_m)
                set(${_sm}_commit "${CMAKE_MATCH_1}" PARENT_SCOPE)
            else()
                set(${_sm}_commit "" PARENT_SCOPE)
            endif()
        endfunction()

        function(_heliosview_sub_repair)
            foreach(_sm IN LISTS HELIOSVIEW_DIRECT_SUBMODULES)
                message(STATUS "HeliosView: repairing ${_sm} checkout...")
                set(_sm_path "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_sm}")
                _heliosview_sub_recorded_commit(${_sm})
                if(NOT ${_sm}_commit)
                    continue()
                endif()
                # Point HEAD at the recorded commit even if the current HEAD is
                # a dangling symref; -f tolerates a dirty/non-empty worktree.
                execute_process(
                        COMMAND ${GIT_EXECUTABLE} -C "${_sm_path}" checkout --detach --force ${${_sm}_commit}
                        RESULT_VARIABLE _co_result
                        OUTPUT_QUIET ERROR_QUIET)
                if(NOT _co_result EQUAL 0)
                    # checkout may fail if the local repo lacks the object
                    # (shallow); materialize it from the submodule's origin.
                    execute_process(
                            COMMAND ${GIT_EXECUTABLE} -C "${_sm_path}" fetch --depth 1 origin ${${_sm}_commit}
                            OUTPUT_QUIET ERROR_QUIET)
                    execute_process(
                            COMMAND ${GIT_EXECUTABLE} -C "${_sm_path}" checkout --detach --force ${${_sm}_commit}
                            OUTPUT_QUIET ERROR_QUIET)
                endif()
                execute_process(
                        COMMAND ${GIT_EXECUTABLE} -C "${_sm_path}" reset --hard --force HEAD
                        OUTPUT_QUIET ERROR_QUIET)
            endforeach()
        endfunction()

        if(NOT _stdexec_ok OR NOT _json_ok)
            message(STATUS "HeliosView: Direct submodule checkouts are broken/missing; initializing...")
        else()
            message(STATUS "HeliosView: Direct submodules look healthy; verifying...")
        endif()

        _heliosview_sub_init()

        # Re-check after init; if a marker is still missing, try to repair the
        # worktree (covers the "init succeeded but left an empty checkout" case
        # and the half-cloned state this project hit on the json submodule).
        set(missing "")
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/exec/asio/asio_config.hpp.in")
            list(APPEND missing stdexec)
        endif()
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include/nlohmann/json.hpp")
            list(APPEND missing json)
        endif()
        if(missing)
            message(STATUS "HeliosView: Repairing empty/broken direct submodule checkouts: ${missing}")
            _heliosview_sub_repair()
            # After a repair, re-run submodule update once so superproject
            # gitlinks and the repaired worktree are consistent.
            _heliosview_sub_init()
        endif()

        # Final gate: every marker must exist now, otherwise the dependency is
        # genuinely unavailable (e.g. no network on first configure) — say so.
        set(still_missing "")
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/exec/asio/asio_config.hpp.in")
            list(APPEND still_missing stdexec)
        endif()
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include/nlohmann/json.hpp")
            list(APPEND still_missing json)
        endif()
        if(still_missing)
            message(FATAL_ERROR
                    "HeliosView direct submodules could not be initialized or repaired: ${still_missing}. "
                    "Marker files still missing under third_party/. "
                    "Network access is needed for first-time builds (or run "
                    "`git submodule update --init third_party/stdexec third_party/json` and re-run CMake).")
        endif()
    else()
        message(WARNING "Git not found; skipping submodule initialization. Ensure submodules are manually populated.")
    endif()
else()
    message(STATUS "HeliosView: .gitmodules not found; assuming submodules are pre-populated.")
endif()


# Boost: the superproject only contains lib submodules; initialize the subset
# HeliosView needs at configure time (a no-op once they are present). Append
# new libraries here when more of Boost is used (e.g. libs/url for HTTP).
# Do NOT run `git submodule update --init --recursive` on HeliosView — it
# would fetch all ~160 Boost libraries.
set(HELIOSVIEW_BOOST_ROOT "${CMAKE_SOURCE_DIR}/third_party/boost")
# asio + beast (HTTP) and their header-only dependency closure. A shallow
# fetch can leave a lib with an empty include/ (git dir present, index empty);
# heliosview_ensure_boost_libs detects and repairs that.
# Each lib here is verified against the #include closure of HeliosView's
# public headers; the following were 0-reference and are intentionally NOT
# initialized/installed (saves ~11 MB in the SDK):
#   preprocessor (~10 MB)  date_time  compat  variant2  integer
# Note: include/boost/functional/ ships inside container_hash (historic
# namespace) — not a separate lib, do not try to drop it independently.
set(HELIOSVIEW_BOOST_LIBS
        asio beast system config core assert throw_exception static_assert
        type_traits utility detail winapi move
        align mp11 predef optional
        smart_ptr bind intrusive logic static_string container_hash describe io)


function(_heliosview_boost_lib_ok lib out_var)
    file(GLOB_RECURSE _f "${HELIOSVIEW_BOOST_ROOT}/libs/${lib}/include/*")
    if(_f)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()
function(heliosview_ensure_boost_libs)
    string(TIMESTAMP _boost_all_epoch "%s")
    list(LENGTH HELIOSVIEW_BOOST_LIBS _boost_libs_count)
    message(STATUS "HeliosView: ensuring ${_boost_libs_count} Boost submodules...")
    # The boost superproject itself is a (shallow-cloned) direct submodule.
    # If it is not checked out (e.g. its gitlink was also removed/re-staged
    # as deleted, or this is a brand-new checkout), `libs/...` submodule
    # updates below would have no boost repo to live in — bring it up first.
    # NB: a shallow submodule's .git is a *file* (a `gitdir:` pointer), not a
    # directory, so use EXISTS, not IS_DIRECTORY.
    if(NOT EXISTS "${HELIOSVIEW_BOOST_ROOT}/.git")
        message(STATUS "HeliosView: Initializing Boost superproject (third_party/boost)... this clones the ~1-2 GB repository shallowly and can take several minutes on a slow mirror; be patient.")
        execute_process(
                COMMAND git submodule update --init --depth 1 third_party/boost
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                RESULT_VARIABLE _boost_root_result)
        if(_boost_root_result EQUAL 0)
            message(STATUS "HeliosView: Boost superproject OK.")
        else()
            message(FATAL_ERROR
                    "Failed to initialize the Boost superproject (third_party/boost). "
                    "Network access is needed at configure time for first-time builds.")
        endif()
    endif()

    set(missing "")
    foreach(lib ${HELIOSVIEW_BOOST_LIBS})
        _heliosview_boost_lib_ok(${lib} _ok)
        if(NOT _ok)
            list(APPEND missing "libs/${lib}")
        endif()
    endforeach()

    # Initialize each missing Boost library individually. A single batch
    # `git submodule update --init --depth 1 -- libs/asio;libs/beast;...`
    # routinely stalls through slow GitHub mirrors (this repo's ghproxy
    # insteadof), whereas per-library shallow clones complete reliably.
    if(missing)
        message(STATUS "HeliosView: Initializing ${missing} Boost libraries (one at a time; each is a small shallow clone).")
        string(TIMESTAMP _boost_libs_epoch "%s")
    endif()
    list(LENGTH missing _boost_libs_total)
    set(_boost_libs_idx 0)
    foreach(p ${missing})
        math(EXPR _boost_libs_idx "${_boost_libs_idx} + 1")
        string(REPLACE "libs/" "" lib "${p}")
        message(STATUS "   [${_boost_libs_idx}/${_boost_libs_total}] fetching Boost ${lib}...")
        set(_lib_ok FALSE)
        foreach(_try RANGE 1 4)
            execute_process(
                    COMMAND git submodule update --init --depth 1 -- ${p}
                    WORKING_DIRECTORY "${HELIOSVIEW_BOOST_ROOT}"
                    RESULT_VARIABLE _r
                    OUTPUT_QUIET ERROR_QUIET)
            _heliosview_boost_lib_ok(${lib} _lib_ok)
            if(_lib_ok)
                break()
            endif()
            if(_try LESS 4)
                message(STATUS "   [${_boost_libs_idx}/${_boost_libs_total}] ${lib} fetch incomplete; retrying (attempt ${_try})...")
            endif()
        endforeach()
        if(_lib_ok)
            message(STATUS "   [${_boost_libs_idx}/${_boost_libs_total}] ${lib} OK.")
        else()
            message(FATAL_ERROR
                    "Failed to initialize Boost library ${p} (network needed on first build).")
        endif()
    endforeach()
    if(missing)
        string(TIMESTAMP _boost_libs_done "%s")
        math(EXPR _boost_libs_secs "${_boost_libs_done} - ${_boost_libs_epoch}")
        message(STATUS "HeliosView: ${_boost_libs_total} Boost libraries ready (${_boost_libs_secs}s).")
    endif()

    # A shallow fetch can leave an empty checkout (git dir present, index
    # empty); materialize any such library from its pinned commit.
    set(broken "")
    foreach(lib ${HELIOSVIEW_BOOST_LIBS})
        _heliosview_boost_lib_ok(${lib} _ok)
        if(NOT _ok)
            list(APPEND broken "${lib}")
        endif()
    endforeach()
    if(broken)
        message(STATUS "HeliosView: repairing empty Boost checkouts: ${broken}")
        set(_repair_idx 0)
        list(LENGTH broken _repair_total)
        foreach(lib ${broken})
            math(EXPR _repair_idx "${_repair_idx} + 1")
            message(STATUS "   repairing Boost ${lib} [${_repair_idx}/${_repair_total}]...")
            execute_process(
                    COMMAND git -C "${HELIOSVIEW_BOOST_ROOT}/libs/${lib}" checkout --detach --force HEAD
                    COMMAND git -C "${HELIOSVIEW_BOOST_ROOT}/libs/${lib}" reset --hard HEAD
                    WORKING_DIRECTORY "${HELIOSVIEW_BOOST_ROOT}")
        endforeach()
        message(STATUS "HeliosView: Boost repair complete.")
    endif()
    set(still_missing "")
    foreach(lib ${HELIOSVIEW_BOOST_LIBS})
        _heliosview_boost_lib_ok(${lib} _ok)
        if(NOT _ok)
            list(APPEND still_missing "${lib}")
        endif()
    endforeach()
    if(still_missing)
        message(FATAL_ERROR
                "Boost libraries still missing after init/repair: ${still_missing}")
    endif()
    string(TIMESTAMP _boost_all_done "%s")
    math(EXPR _boost_all_secs "${_boost_all_done} - ${_boost_all_epoch}")
    message(STATUS "HeliosView: all ${_boost_libs_count} Boost libs OK (Boost total ${_boost_all_secs}s).")
endfunction()
heliosview_ensure_boost_libs()
