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
# Non-OS dependencies (WebView2, vendored nlohmann / stdexec) are linked
# separately and are NOT part of this macro.

# heliosview_link_platform_system_libs(<target>) --
# Link the OS-specific system libraries the target's backend needs. PRIVATE,
# because these are implementation details of the DLL, not part of its public
# C API.
macro(heliosview_link_platform_system_libs target)
  if(WIN32)
    # Windows backend (src/win32/):
    #   user32       - window/UI: message loop, tray, message box, clipboard
    #   shell32      - shell tray icon (Shell_NotifyIcon), file pickers, ShellExecute
    #   ole32        - COM (WebView2, IFileDialog, taskbar ITaskbarList)
    #   dwmapi       - DWM window backdrop / dark mode (DwmSetWindowAttribute)
    #   comctl32     - InitCommonControlsEx (common controls v6 init)
    #   runtimeobject- WinRT Ro* (toast notifications)
    #   propsys      - IPropertyStore (toast AppUserModelID shortcut)
    target_link_libraries(${target} PRIVATE
      user32 shell32 ole32 dwmapi comctl32 runtimeobject propsys)

  elseif(APPLE)
    # macOS backend (src/macos/), for example:
    #   Cocoa    - window/UI + native menu/tray
    #   WebKit   - embedded WebView
    target_link_libraries(${target} PRIVATE
      "-framework Cocoa"
      "-framework WebKit")

  elseif(UNIX)
    # Linux/BSD backend (src/unix/), for example — GTK/Qt for windows plus the
    # platform dynamic loader. Adjust to the actual backend.
    target_link_libraries(${target} PRIVATE
      dl m pthread)

  endif()
endmacro()
