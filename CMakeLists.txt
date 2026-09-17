cmake_minimum_required(VERSION 4.3)

project(HeliosView
        VERSION 1.0.0
        DESCRIPTION "HeliosView - a C++ WebView windowing library"
        LANGUAGES CXX C)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

if(MSVC)
    # beast/stdexec template-heavy headers exceed the default COMDAT section limit.
    # /EHsc enables standard C++ stack unwinding and exception handling (required by Boost/STL).
    # /utf-8 ensures UTF-8 source and execution character sets.
    add_compile_options(/bigobj /EHsc /utf-8)
endif()

# DLLs and demo exes land in the same directory, so no manual PATH setup at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(cmake/ensure-submodule.cmake)

# ---------- Third-party dependencies ----------
#
# Git submodules (third_party/, fetched with `git submodule update --init`
# — NOT --recursive, see the Boost note below; stdexec and Boost are header-only,
# blend2d/asmjit are compiled by this build):
#   - stdexec:     P2300 senders/receivers reference implementation, pinned to
#                  the latest origin/main commit (0.11.0+) HeliosView.Core is
#                  built and tested against.
#   - Boost:       Boost superproject submodule (third_party/boost), pinned to
#                  boost-1.92.0. Only the libraries HeliosView needs are
#                  initialized (HELIOSVIEW_BOOST_LIBS) — Boost.Asio for the
#                  background scheduler (Async.h), Boost.Beast for HTTP, and
#                  Boost.JSON for the WebView bridge auto-binding (WebViewJson.h).
#                  Do NOT run `git submodule update --init --recursive` on
#                  HeliosView: it would fetch all ~160 Boost libraries; the
#                  selective init runs automatically at configure time
#                  (cmake/ensure-submodule.cmake issues one `git submodule
#                  update` call with every lib in HELIOSVIEW_BOOST_LIBS).
#   - blend2d:     high-performance 2D vector rasterizer with a JIT pipeline,
#                  pinned to v0.21.3. It is the BUILTIN canvas engine
#                  (src/heliosview_canvas_blend2d.cpp): the same pixels on every
#                  platform, with no OS 2D API involved.
#   - asmjit:      the x86/ARM assembler blend2d's JIT is built on, pinned to the
#                  commit that blend2d v0.21.3 is tested against.
#
# WebView2 SDK: downloaded from NuGet at configure time (a .nupkg is just a
# zip of headers + the WebView2Loader loader library/dll), so consumers do not
# need it from vcpkg.
#
# Everything else comes from the OS: windowing, dialogs, taskbar, toasts
# (WinRT via the Windows SDK), DWM backdrop and TLS-free by design. No vcpkg.

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



# Boost.Asio/Beast: header-only for our use (asio + system are header-only
# since Boost 1.69); nothing to compile or link except ws2_32 on Windows for
# sockets. Backs the library's background scheduler (HeliosViewCore/Async.h
# via stdexec's exec::asio adapters) and Boost.Beast for HTTP.
#
# Boost.JSON is NOT header-only (parse/serialize/value live in a compiled
# part). Its compiled part is built as a normal static library (the
# `boost_json` target from libs/json's own CMakeLists, see below), which
# HeliosView.Core links and the SDK installs. BOOST_ALL_NO_LIB suppresses the
# auto-link pragmas (#pragma comment(lib)) that several vendored boost libs
# emit (boost_json, boost_container, ...) — with our own boost_json library
# on the link line, the linker must not also hunt for libboost_*-*.lib.
add_library(boost INTERFACE)
target_compile_definitions(boost INTERFACE BOOST_ALL_NO_LIB)
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

# Boost.JSON compiled library: built with Boost's own CMake (libs/json),
# which compiles boost/json/src.cpp (= src.hpp: parse/serialize/value/...).
# Its CMakeLists links Boost::<dep> targets for its dependencies; provide
# those as INTERFACE aliases over the `boost` target above (the superproject's
# BoostRoot.cmake would normally do this, but its tools/cmake submodule is not
# part of the selective init). After the subdirectory is processed, the
# dependency graph is flattened onto `boost` so the installed export only
# references exported targets.
foreach(_bjdep IN ITEMS assert config container container_hash core describe endian mp11 system throw_exception)
    if(NOT TARGET Boost::${_bjdep})
        add_library(Boost::${_bjdep} INTERFACE IMPORTED)
        set_target_properties(Boost::${_bjdep} PROPERTIES INTERFACE_LINK_LIBRARIES boost)
    endif()
endforeach()
add_subdirectory(${HELIOSVIEW_BOOST_ROOT}/libs/json)
set_target_properties(boost_json PROPERTIES INTERFACE_LINK_LIBRARIES "boost")

# OpenSSL: prebuilt x64 from the openssl.vcpkg nuget package, managed outside
# CMake by scripts/setup-dependencies. Needed by boost::asio::ssl for https
# (Http.h) — Windows has no system OpenSSL.
if(WIN32)
    if(NOT DEFINED HELIOSVIEW_OPENSSL_ROOT)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl/build/native/include/openssl/ssl.h")
            set(HELIOSVIEW_OPENSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl")
        elseif(EXISTS "${CMAKE_BINARY_DIR}/openssl/build/native/include/openssl/ssl.h")
            set(HELIOSVIEW_OPENSSL_ROOT "${CMAKE_BINARY_DIR}/openssl")
        else()
            set(HELIOSVIEW_OPENSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl")
        endif()
    endif()

    if(NOT EXISTS "${HELIOSVIEW_OPENSSL_ROOT}/build/native/include/openssl/ssl.h")
        message(FATAL_ERROR
            "\n======================================================================\n"
            "HeliosView: OpenSSL was not found at '${HELIOSVIEW_OPENSSL_ROOT}'.\n"
            "Please run the setup script to download and extract dependencies:\n"
            "  PowerShell:  .\\scripts\\setup-dependencies.ps1\n"
            "  Or Python:   python scripts/setup-dependencies.py\n"
            "======================================================================\n")
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

    # CA bundle for TLS verification (OpenSSL on Windows does not use the
    # system certificate store). Managed by scripts/setup-dependencies.
    if(NOT DEFINED HELIOSVIEW_CACERT_PEM)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cacert.pem")
            set(HELIOSVIEW_CACERT_PEM "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cacert.pem")
        elseif(EXISTS "${CMAKE_BINARY_DIR}/cacert.pem")
            set(HELIOSVIEW_CACERT_PEM "${CMAKE_BINARY_DIR}/cacert.pem")
        else()
            set(HELIOSVIEW_CACERT_PEM "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cacert.pem")
        endif()
    endif()

    if(NOT EXISTS "${HELIOSVIEW_CACERT_PEM}")
        message(FATAL_ERROR
            "\n======================================================================\n"
            "HeliosView: CA certificate bundle was not found at '${HELIOSVIEW_CACERT_PEM}'.\n"
            "Please run the setup script to download dependencies:\n"
            "  PowerShell:  .\\scripts\\setup-dependencies.ps1\n"
            "  Or Python:   python scripts/setup-dependencies.py\n"
            "======================================================================\n")
    endif()
    file(COPY "${HELIOSVIEW_CACERT_PEM}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# heliosview_link_platform_system_libs(): links each backend's OS-specific
# system libraries (Win32 user32/shell32/ole32/dwmapi/runtimeobject/propsys,
# etc.) through one macro, so moving to another platform only means adding a
# branch there.
include(cmake/PlatformLibs.cmake)

if(WIN32)
    # WebView2 SDK: managed outside CMake by scripts/setup-dependencies.
    if(NOT DEFINED HELIOSVIEW_WEBVIEW2_ROOT)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-sdk/build/native/include/WebView2.h")
            set(HELIOSVIEW_WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-sdk")
        elseif(EXISTS "${CMAKE_BINARY_DIR}/webview2-sdk/build/native/include/WebView2.h")
            set(HELIOSVIEW_WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2-sdk")
        else()
            set(HELIOSVIEW_WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-sdk")
        endif()
    endif()

    if(NOT EXISTS "${HELIOSVIEW_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
        message(FATAL_ERROR
            "\n======================================================================\n"
            "HeliosView: Microsoft WebView2 SDK was not found at '${HELIOSVIEW_WEBVIEW2_ROOT}'.\n"
            "Please run the setup script to download dependencies:\n"
            "  PowerShell:  .\\scripts\\setup-dependencies.ps1\n"
            "  Or Python:   python scripts/setup-dependencies.py\n"
            "======================================================================\n")
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

# Blend2D: unified 2D vector graphics engine (software rasterizer with JIT)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    set(BLEND2D_TARGET_ARCH "X86_64" CACHE STRING "" FORCE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(BLEND2D_TARGET_ARCH "AArch64" CACHE STRING "" FORCE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "X86|i[3-6]86")
    set(BLEND2D_TARGET_ARCH "X86" CACHE STRING "" FORCE)
endif()
set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
set(BLEND2D_TEST OFF CACHE BOOL "" FORCE)
set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/blend2d)

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
# stdexec + boost + boost_json propagate via INTERFACE: targets linking
# HeliosView::Core (e.g. the examples) automatically get their include paths
# and the C++23 requirement
target_link_libraries(HeliosView.Core INTERFACE HeliosView stdexec boost boost_json)
if(WIN32)
    # OpenSSL is vendored from NuGet above. Another platform provides it itself
    # (HeliosViewCore/Http.h needs it for https): add find_package(OpenSSL) and
    # link OpenSSL::SSL/OpenSSL::Crypto here when that backend lands.
    target_link_libraries(HeliosView.Core INTERFACE openssl)
endif()
# exported to consumers: any target linking HeliosView::Core is compiled as C++23
target_compile_features(HeliosView.Core INTERFACE cxx_std_23)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_heliosview_is_toplevel ON)
else()
    set(_heliosview_is_toplevel OFF)
endif()

option(HELIOSVIEW_BUILD_EXAMPLES "Build the example executables" ${_heliosview_is_toplevel})
if(HELIOSVIEW_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

option(HELIOSVIEW_BUILD_TESTS "Build the test executables" ${_heliosview_is_toplevel})
if(HELIOSVIEW_BUILD_TESTS)
    add_subdirectory(tests)
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
# stdexec keeps build-tree include paths (the source is vendored under
# third_party/); this is a self-hosting install.
install(TARGETS HeliosView.Core stdexec boost openssl EXPORT HeliosViewTargets)

# boost_json: the compiled Boost.JSON library (static), built from Boost's own
# CMake — exported alongside HeliosView::Core so consumers link it through the
# package config, and installed into lib/ next to HeliosView.lib.
install(TARGETS boost_json EXPORT HeliosViewTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/HeliosView
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/HeliosViewCore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Vendored header-only deps of HeliosView.Core (stdexec, boost):
# install them into the same include tree so exported HeliosView::Core resolves.
# Only the initialized Boost libs (HELIOSVIEW_BOOST_LIBS) are installed.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stdexec/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "._clangd_helper_file.cpp" EXCLUDE)
foreach(lib ${HELIOSVIEW_BOOST_LIBS})
    install(DIRECTORY ${HELIOSVIEW_BOOST_ROOT}/libs/${lib}/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endforeach()
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
