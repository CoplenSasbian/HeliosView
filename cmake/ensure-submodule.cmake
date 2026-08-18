# ensure-submodule.cmake
# 极致精简版，完全依赖 git submodule update 的幂等性。
# 所有 git 命令执行前打印命令，失败时输出详细的 stderr 信息。

get_filename_component(HELIOSVIEW_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(EXISTS "${HELIOSVIEW_PROJECT_ROOT}/.gitmodules")
    find_package(Git QUIET)
    if(Git_FOUND)
        # 1. 初始化直接子模块 + Boost 超级项目
        set(_cmd_list ${GIT_EXECUTABLE} submodule update --init --depth 1
                third_party/stdexec third_party/boost)
        list(JOIN _cmd_list " " _cmd_str)
        message(STATUS "HeliosView: Running: ${_cmd_str}")
        execute_process(
                COMMAND ${_cmd_list}
                WORKING_DIRECTORY ${HELIOSVIEW_PROJECT_ROOT}
                RESULT_VARIABLE _init_result
                ERROR_VARIABLE _init_error
        )
        if(NOT _init_result EQUAL 0)
            message(FATAL_ERROR "Failed to init submodules: ${_init_error}")
        endif()
        message(STATUS "HeliosView: Direct submodules and Boost superproject initialized.")

        # 2. 初始化所需的 Boost 库子模块（无条件执行，幂等）
        set(HELIOSVIEW_BOOST_ROOT "${HELIOSVIEW_PROJECT_ROOT}/third_party/boost")
        set(HELIOSVIEW_BOOST_LIBS
                asio beast json container endian variant2 compat system config core assert throw_exception static_assert
                type_traits utility detail winapi move
                align mp11 predef optional
                smart_ptr bind intrusive logic static_string container_hash describe io
        )
        message(STATUS "HeliosView: Ensuring ${HELIOSVIEW_BOOST_LIBS} Boost libs (idempotent)...")
        foreach(lib ${HELIOSVIEW_BOOST_LIBS})
            set(_cmd_list ${GIT_EXECUTABLE} submodule update --init --depth 1 -- libs/${lib})
            list(JOIN _cmd_list " " _cmd_str)
            message(STATUS "HeliosView: Running: ${_cmd_str}")

            execute_process(
                    COMMAND ${_cmd_list}
                    WORKING_DIRECTORY "${HELIOSVIEW_BOOST_ROOT}"
                    RESULT_VARIABLE _r
                    ERROR_VARIABLE _err
            )
            if(_r EQUAL 0)
                message(STATUS "HeliosView: Boost/${lib} OK.")
            else()
                message(FATAL_ERROR "HeliosView: Failed to initialize Boost/${lib}: ${_err}")
            endif()
        endforeach()
        message(STATUS "HeliosView: All Boost libs ready.")

        # 3. 最终检查（仅检查核心文件，确保编译不会立刻报错）
        set(still_missing "")
        if(NOT EXISTS "${HELIOSVIEW_PROJECT_ROOT}/third_party/stdexec/include/exec/asio/asio_config.hpp.in")
            list(APPEND still_missing stdexec)
        endif()
        if(still_missing)
            message(FATAL_ERROR "Direct submodules still missing: ${still_missing}.")
        endif()
    else()
        message(WARNING "Git not found; skipping submodule initialization.")
    endif()
else()
    message(STATUS "HeliosView: .gitmodules not found; assuming submodules are pre-populated.")
endif()