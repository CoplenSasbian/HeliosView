# HeliosView - platform system libraries.
#
# HeliosView exposes one cross-platform C API (include/HeliosView/heliosview.h);
# each OS backend lives under src/<os>/ and links whatever SYSTEM libraries that
# OS provides. Rather than scattering raw library names in src/CMakeLists.txt,
# every backend links through the single macro below. When porting to a new OS:
#
#   1. add the backend sources under src/<os>/,
#   2. add an `elseif(...)` branch here with that OS's system libraries,
#   3. that's it -- src/CMakeLists.txt stays untouched.
#
# Non-OS dependencies (WebView2, vendored http-parser / nlohmann / stdexec) are
# linked separately and are NOT part of this macro.

# heliosview_link_platform_system_libs(<target>) --
# Link the OS-specific system libraries the target's backend needs. PRIVATE,
# because these are implementation details of the DLL, not part of its public
# C API.
macro(heliosview_link_platform_system_libs target)
  if(WIN32)
    # Windows backend (src/win32/):
    #   ws2_32   - Winsock async socket/file I/O (IOCP thread pool)
    #   crypt32  - certificate chain building / verification (system stores)
    #   secur32  - SChannel (SSPI) TLS for the HTTPS client
    #   user32   - window/UI: message loop, tray, opacity, dialogs
    #   shell32  - shell tray icon (Shell_NotifyIcon) + file picker
    #   ole32    - COM (WebView2 integration, IFileOpenDialog)
    target_link_libraries(${target} PRIVATE
      ws2_32 crypt32 secur32 user32 shell32 ole32)

  elseif(APPLE)
    # macOS backend (src/macos/), for example:
    #   Cocoa    - window/UI + native menu/tray
    #   WebKit   - embedded WebView
    #   Security - TLS (Secure Transport / Security framework)
    target_link_libraries(${target} PRIVATE
      "-framework Cocoa"
      "-framework WebKit"
      "-framework Security")

  elseif(UNIX)
    # Linux/BSD backend (src/unix/), for example — GTK/Qt for windows plus the
    # platform TLS and dynamic loader. Adjust to the actual backend.
    target_link_libraries(${target} PRIVATE
      dl m pthread)

  endif()
endmacro()
