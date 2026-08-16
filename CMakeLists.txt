cmake_minimum_required(VERSION 4.3)

project(HeliosView
        VERSION 1.0.1
        DESCRIPTION "HeliosView - a C++ WebView windowing library"
        LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

if(MSVC)
    # beast/stdexec template-heavy headers exceed the default COMDAT section limit
    add_compile_options(/bigobj)
endif()

# DLLs and demo exes land in the same directory, so no manual PATH setup at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------- Third-party dependencies ----------
#
# Git submodules (third_party/, fetched with `git submodule update --init`
# — NOT --recursive, see the Boost note below; header-only, nothing to compile):
#   - stdexec:     P2300 senders/receivers reference implementation, pinned to
#                  the latest origin/main commit (0.11.0+) HeliosView.Core is
#                  built and tested against.
#   - Boost:       Boost superproject submodule (third_party/boost), pinned to
#                  boost-1.92.0. Only the libraries HeliosView needs are
#                  initialized (HELIOSVIEW_BOOST_LIBS) — Boost.Asio for the
#                  background scheduler (Async.h) and Boost.Beast for HTTP.
#                  Do NOT run `git submodule update --init --recursive` on
#                  HeliosView: it would fetch all ~160 Boost libraries; the
#                  selective init runs automatically at configure time.
#   - nlohmann/json (single header, exposed below as nlohmann_json::nlohmann_json).
#
# WebView2 SDK: downloaded from NuGet at configure time (a .nupkg is just a
# zip of headers + the WebView2Loader loader library/dll), so consumers do not
# need it from vcpkg.
#
# Everything else comes from the OS: windowing, dialogs, taskbar, toasts
# (WinRT via the Windows SDK), DWM backdrop and TLS-free by design. No vcpkg.

# nlohmann/json is header-only; expose it directly instead of running its build.
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include>
    $<INSTALL_INTERFACE:include>)

# stdexec: P2300 senders/receivers reference implementation (header-only),
# pinned to origin/main 758f41f4 (2026-08-15) — vendored, not downloaded.
add_library(stdexec INTERFACE)
# stdexec's exec::asio adapters require a generated asio_config.hpp selecting
# standalone vs Boost asio. HeliosView does not run stdexec's own CMake, so we
# generate it here (mirroring stdexec's ConfigureASIO.cmake). Boost.Asio is
# the configured backend.
set(STDEXEC_ASIO_USES_STANDALONE 0)
set(STDEXEC_ASIO_USES_BOOST 1)
set(STDEXEC_ASIO_CONFIG_HPP "${CMAKE_CURRENT_BINARY_DIR}/generated/exec/asio/asio_config.hpp")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/exec/asio/asio_config.hpp.in
    ${STDEXEC_ASIO_CONFIG_HPP})
target_include_directories(stdexec INTERFACE
    # generated/ first, so <exec/asio/asio_config.hpp> resolves to the generated file
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    # stdexec requires MSVC's conforming __cplusplus / preprocessor
    target_compile_options(stdexec INTERFACE /Zc:__cplusplus /Zc:preprocessor)
endif()

# Boost: the superproject only contains lib submodules; initialize the subset
# HeliosView needs at configure time (a no-op once they are present). Append
# new libraries here when more of Boost is used (e.g. libs/url for HTTP).
# Do NOT run `git submodule update --init --recursive` on HeliosView — it
# would fetch all ~160 Boost libraries.
set(HELIOSVIEW_BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")
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
    if(NOT EXISTS "${HELIOSVIEW_BOOST_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "Boost submodule missing at ${HELIOSVIEW_BOOST_ROOT}.\n"
            "Run `git submodule update --init` (HeliosView's own submodules).")
    endif()
    set(missing "")
    foreach(lib ${HELIOSVIEW_BOOST_LIBS})
        _heliosview_boost_lib_ok(${lib} _ok)
        if(NOT _ok)
            list(APPEND missing "libs/${lib}")
        endif()
    endforeach()
    if(missing)
        message(STATUS "Initializing Boost libraries: ${missing}")
        execute_process(
            COMMAND git submodule update --init --depth 1 -- ${missing}
            WORKING_DIRECTORY "${HELIOSVIEW_BOOST_ROOT}"
            RESULT_VARIABLE _boost_init_result)
        if(NOT _boost_init_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to initialize Boost submodules (${missing}).\n"
                "Network access is needed at configure time for first-time builds.")
        endif()
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
        message(STATUS "Repairing empty Boost checkouts: ${broken}")
        foreach(lib ${broken})
            execute_process(
                COMMAND git -C "${HELIOSVIEW_BOOST_ROOT}/libs/${lib}" checkout --detach --force HEAD
                COMMAND git -C "${HELIOSVIEW_BOOST_ROOT}/libs/${lib}" reset --hard HEAD
                WORKING_DIRECTORY "${HELIOSVIEW_BOOST_ROOT}")
        endforeach()
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
endfunction()
heliosview_ensure_boost_libs()

# Boost.Asio/Beast: header-only for our use (asio + system are header-only
# since Boost 1.69); nothing to compile or link except ws2_32 on Windows for
# sockets. Backs the library's background scheduler (HeliosViewCore/Async.h
# via stdexec's exec::asio adapters) and Boost.Beast for HTTP.
add_library(boost INTERFACE)
foreach(lib ${HELIOSVIEW_BOOST_LIBS})
    target_include_directories(boost INTERFACE
        $<BUILD_INTERFACE:${HELIOSVIEW_BOOST_ROOT}/libs/${lib}/include>
        $<INSTALL_INTERFACE:include>)
endforeach()
if(WIN32)
    # Windows sockets (asio/beast) + explicit Win10+ target (asio defaults to
    # 0x0601 with a message otherwise; WebView2 requires Win10 anyway).
    target_link_libraries(boost INTERFACE ws2_32 mswsock)
    target_compile_definitions(boost INTERFACE _WIN32_WINNT=0x0A00)
endif()

# OpenSSL: prebuilt x64 from the openssl.vcpkg nuget package, downloaded at
# configure time (like the WebView2 SDK) and cached in the build dir. Needed
# by boost::asio::ssl for https (Http.h) — Windows has no system OpenSSL.
if(WIN32)
    set(HELIOSVIEW_OPENSSL_VERSION "3.5.2")
    set(HELIOSVIEW_OPENSSL_ARCHIVE "${CMAKE_BINARY_DIR}/openssl.vcpkg.${HELIOSVIEW_OPENSSL_VERSION}.nupkg")
    set(HELIOSVIEW_OPENSSL_ROOT "${CMAKE_BINARY_DIR}/openssl")
    if(NOT EXISTS "${HELIOSVIEW_OPENSSL_ROOT}/build/native/include/openssl/ssl.h")
        if(NOT EXISTS "${HELIOSVIEW_OPENSSL_ARCHIVE}")
            message(STATUS "Downloading OpenSSL ${HELIOSVIEW_OPENSSL_VERSION} (x64, ~22 MB, one-time)...")
            file(DOWNLOAD
                "https://api.nuget.org/v3-flatcontainer/openssl.vcpkg/${HELIOSVIEW_OPENSSL_VERSION}/openssl.vcpkg.${HELIOSVIEW_OPENSSL_VERSION}.nupkg"
                "${HELIOSVIEW_OPENSSL_ARCHIVE}"
                STATUS _ossl_status)
            list(GET _ossl_status 0 _ossl_code)
            if(NOT _ossl_code EQUAL 0)
                list(GET _ossl_status 1 _ossl_err)
                message(FATAL_ERROR "Failed to download OpenSSL: ${_ossl_err}")
            endif()
        endif()
        file(ARCHIVE_EXTRACT INPUT "${HELIOSVIEW_OPENSSL_ARCHIVE}" DESTINATION "${HELIOSVIEW_OPENSSL_ROOT}")
    endif()
    add_library(openssl INTERFACE)
    target_include_directories(openssl INTERFACE
        $<BUILD_INTERFACE:${HELIOSVIEW_OPENSSL_ROOT}/build/native/include>
        $<INSTALL_INTERFACE:include>)
    target_link_libraries(openssl INTERFACE
        $<$<CONFIG:Debug>:$<BUILD_INTERFACE:${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Debug/libssl.lib>>
        $<$<CONFIG:Debug>:$<BUILD_INTERFACE:${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Debug/libcrypto.lib>>
        $<$<NOT:$<CONFIG:Debug>>:$<BUILD_INTERFACE:${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Release/libssl.lib>>
        $<$<NOT:$<CONFIG:Debug>>:$<BUILD_INTERFACE:${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Release/libcrypto.lib>>
        $<$<CONFIG:Debug>:$<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/openssl/Debug/libssl.lib>>
        $<$<CONFIG:Debug>:$<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/openssl/Debug/libcrypto.lib>>
        $<$<NOT:$<CONFIG:Debug>>:$<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/openssl/Release/libssl.lib>>
        $<$<NOT:$<CONFIG:Debug>>:$<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/openssl/Release/libcrypto.lib>>)
    # Runtime DLLs next to the demo exes
    set(HELIOSVIEW_OPENSSL_DLLS
        "${HELIOSVIEW_OPENSSL_ROOT}/build/native/bin/x64/Release/libssl-3-x64.dll"
        "${HELIOSVIEW_OPENSSL_ROOT}/build/native/bin/x64/Release/libcrypto-3-x64.dll")
    file(COPY ${HELIOSVIEW_OPENSSL_DLLS} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    # CA bundle for TLS verification (curl.se; OpenSSL on Windows does not use
    # the system certificate store). TLS-verified download, cached.
    set(HELIOSVIEW_CACERT_PEM "${CMAKE_BINARY_DIR}/cacert.pem")
    if(NOT EXISTS "${HELIOSVIEW_CACERT_PEM}")
        file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${HELIOSVIEW_CACERT_PEM}"
             STATUS _cacert_status)
        list(GET _cacert_status 0 _cacert_code)
        if(NOT _cacert_code EQUAL 0)
            list(GET _cacert_status 1 _cacert_err)
            message(FATAL_ERROR "Failed to download cacert.pem: ${_cacert_err}")
        endif()
    endif()
    file(COPY "${HELIOSVIEW_CACERT_PEM}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# heliosview_link_platform_system_libs(): links each backend's OS-specific
# system libraries (Win32 user32/shell32/ole32/dwmapi/runtimeobject/propsys,
# etc.) through one macro, so moving to another platform only means adding a
# branch there.
include(cmake/PlatformLibs.cmake)

if(WIN32)
    # WebView2 SDK: download the Microsoft.Web.WebView2 NuGet package at
    # configure time and extract the native headers + WebView2Loader lib/dll.
    set(HELIOSVIEW_WEBVIEW2_VERSION "1.0.4129.50")
    set(HELIOSVIEW_WEBVIEW2_ARCHIVE "${CMAKE_BINARY_DIR}/webview2.nupkg")
    set(HELIOSVIEW_WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2-sdk")
    if(NOT EXISTS "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${HELIOSVIEW_WEBVIEW2_VERSION}"
            "${HELIOSVIEW_WEBVIEW2_ARCHIVE}"
            STATUS _wv2_status)
        list(GET _wv2_status 0 _wv2_code)
        if(NOT _wv2_code EQUAL 0)
            list(GET _wv2_status 1 _wv2_err)
            message(FATAL_ERROR "Failed to download WebView2 SDK: ${_wv2_err}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${HELIOSVIEW_WEBVIEW2_ARCHIVE}" DESTINATION "${HELIOSVIEW_WEBVIEW2_ROOT}")
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(HELIOSVIEW_WEBVIEW2_ARCH x64)
    else()
        set(HELIOSVIEW_WEBVIEW2_ARCH x86)
    endif()
    add_library(WebView2::SDK INTERFACE IMPORTED)
    set_target_properties(WebView2::SDK PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/include"
        INTERFACE_LINK_LIBRARIES "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/${HELIOSVIEW_WEBVIEW2_ARCH}/WebView2Loader.dll.lib"
    )
    set(HELIOSVIEW_WEBVIEW2_LOADER_DLL
        "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/${HELIOSVIEW_WEBVIEW2_ARCH}/WebView2Loader.dll")
endif()

add_subdirectory(src)

# HeliosView.Core: header-only C++ wrapper (INTERFACE library; propagates only
# include paths and link dependencies). Consumers just target_link_libraries(
# ... HeliosView::Core) to get the C++ headers, C API header paths, and the
# HeliosView.lib import library.
add_library(HeliosView.Core INTERFACE)
add_library(HeliosView::Core ALIAS HeliosView.Core)
target_include_directories(HeliosView.Core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# stdexec + boost + openssl + nlohmann propagate via INTERFACE: targets linking
# HeliosView::Core (e.g. the examples) automatically get their include paths
# and the C++23 requirement
target_link_libraries(HeliosView.Core INTERFACE HeliosView stdexec boost openssl nlohmann_json::nlohmann_json)
# exported to consumers: any target linking HeliosView::Core is compiled as C++23
target_compile_features(HeliosView.Core INTERFACE cxx_std_23)

option(HELIOSVIEW_BUILD_EXAMPLES "Build the example executables" ON)
if(HELIOSVIEW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------- Install / packaging ----------
#
# Consumers (including FFI wrappers in other languages) link the built DLL/import
# lib and include the C header directly; C++ consumers use the exported
# HeliosView::Core INTERFACE target.

install(TARGETS HeliosView
        EXPORT HeliosViewTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# HeliosView.Core (INTERFACE) and its vendored header-only deps are exported too,
# so C++ consumers get a working HeliosView::Core from the installed config.
# stdexec/nlohmann keep build-tree include paths (the source is vendored under
# third_party/); this is a self-hosting install.
install(TARGETS HeliosView.Core stdexec boost openssl nlohmann_json EXPORT HeliosViewTargets)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/HeliosView
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/HeliosViewCore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Vendored header-only deps of HeliosView.Core (stdexec, boost, nlohmann/json):
# install them into the same include tree so exported HeliosView::Core resolves.
# Only the initialized Boost libs (HELIOSVIEW_BOOST_LIBS) are installed.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "._clangd_helper_file.cpp" EXCLUDE)
foreach(lib ${HELIOSVIEW_BOOST_LIBS})
    install(DIRECTORY ${HELIOSVIEW_BOOST_ROOT}/libs/${lib}/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endforeach()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Generated stdexec asio config: installed consumers resolve
# <exec/asio/asio_config.hpp> to this Boost-asio configuration.
install(FILES ${STDEXEC_ASIO_CONFIG_HPP}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/exec/asio)

if(WIN32)
    # OpenSSL headers/import libs (self-hosting install) + runtime DLLs and
    # the CA bundle (both next to the exes).
    install(DIRECTORY ${HELIOSVIEW_OPENSSL_ROOT}/build/native/include/openssl
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES ${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Debug/libssl.lib
                  ${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Debug/libcrypto.lib
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/openssl/Debug)
    install(FILES ${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Release/libssl.lib
                  ${HELIOSVIEW_OPENSSL_ROOT}/build/native/lib/x64/Release/libcrypto.lib
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/openssl/Release)
    install(FILES ${HELIOSVIEW_OPENSSL_DLLS} ${HELIOSVIEW_CACERT_PEM}
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WIN32)
    # WebView2Loader.dll must sit next to the app exe at runtime.
    install(FILES "${HELIOSVIEW_WEBVIEW2_LOADER_DLL}"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT HeliosViewTargets
        FILE HeliosViewTargets.cmake
        NAMESPACE HeliosView::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeliosView)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/HeliosViewConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeliosView)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/HeliosViewConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeliosView)
