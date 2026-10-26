# 「J2」平台集成层（platform）
#
# 依赖方向：platform -> core（允许）；platform -> ui（禁止）。
# 每个平台一个实现文件，共同实现 platform.h 中的接口。

add_library(jie2_platform STATIC
    factory.cpp
    hotkey.cpp
)

target_include_directories(jie2_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(jie2_platform PUBLIC jie2_core)
find_package(Threads REQUIRED)
target_link_libraries(jie2_platform PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(jie2_platform PRIVATE /W4)
else()
    target_compile_options(jie2_platform PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    # Windows（Win32）实现：GDI 抓屏、RegisterHotKey、窗口枚举。
    # 已通过 mingw-w64 交叉编译验证；实机运行验证待 Windows 环境
    # （见 windows_platform.cpp 头部的状态声明与验证清单）。
    target_sources(jie2_platform PRIVATE windows/windows_platform.cpp)
    target_link_libraries(jie2_platform PRIVATE user32 gdi32 shcore)
    message(STATUS "jie2: Windows platform backend enabled (compile-verified via mingw)")
elseif(APPLE)
    target_sources(jie2_platform PRIVATE macos/macos_platform.mm)
    target_link_libraries(jie2_platform PRIVATE
        "-framework Carbon"
        "-framework Cocoa"
        "-framework ApplicationServices"
        # Vision：系统 OCR（M5 的"系统能力"后端）。离线、零配置。
        "-framework Vision"
    )
elseif(UNIX)
    target_sources(jie2_platform PRIVATE linux/linux_platform.cpp)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(jie2_platform PRIVATE JIE2_HAVE_X11=1)
        target_link_libraries(jie2_platform PRIVATE X11::X11)
        message(STATUS "jie2: X11 found (${X11_VERSION}) — X11 hotkey backend enabled")

        # XRandR 为可选：仅用于显示器枚举。缺省时退回"整块虚拟桌面"单屏报告。
        if(X11_Xrandr_FOUND)
            target_compile_definitions(jie2_platform PRIVATE JIE2_HAVE_XRANDR=1)
            target_link_libraries(jie2_platform PRIVATE X11::Xrandr)
            message(STATUS "jie2: XRandR found — multi-display enumeration enabled")
        else()
            message(STATUS "jie2: XRandR not found — single virtual-desktop display only")
        endif()
    else()
        message(WARNING "jie2: X11 not found — Linux builds without X11 backend")
    endif()

    # libdbus-1：Wayland 全局热键（portal GlobalShortcuts）。
    # 可选依赖 —— 缺失时 Wayland 热键如实报告不可用，X11 后端不受影响。
    find_package(PkgConfig QUIET)
    set(JIE2_DBUS_FOUND FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(DBUS QUIET dbus-1)
        if(DBUS_FOUND)
            set(JIE2_DBUS_FOUND TRUE)
        endif()
    endif()
    if(JIE2_DBUS_FOUND)
        target_sources(jie2_platform PRIVATE linux/wayland_portal.cpp)
        target_compile_definitions(jie2_platform PRIVATE JIE2_HAVE_DBUS=1)
        target_link_libraries(jie2_platform PRIVATE ${DBUS_LIBRARIES})
        target_include_directories(jie2_platform PRIVATE ${DBUS_INCLUDE_DIRS})
        message(STATUS "jie2: libdbus-1 found (${DBUS_VERSION}) — Wayland GlobalShortcuts backend enabled")

        # 截图门户（Screenshot）额外需要 zlib：门户返回 PNG 文件，需在
        # 平台层解码为 RGBA（core 层不得依赖 zlib，见 core/CMakeLists.txt）。
        find_package(ZLIB QUIET)
        if(ZLIB_FOUND)
            target_sources(jie2_platform PRIVATE
                linux/wayland_screenshot.cpp
                linux/png_decode.cpp)
            target_link_libraries(jie2_platform PRIVATE ZLIB::ZLIB)
            target_compile_definitions(jie2_platform PRIVATE JIE2_HAVE_PORTAL_SCREENSHOT=1)
            message(STATUS "jie2: zlib found (${ZLIB_VERSION_STRING}) — Wayland portal screenshot enabled")
        else()
            message(STATUS "jie2: zlib not found — Wayland portal screenshot disabled")
        endif()
    else()
        message(STATUS "jie2: libdbus-1 not found — Wayland hotkeys disabled in this build")
    endif()

    # 注意：Wayland layer-shell（衣架顶部条）**不在这里**实现。
    # 架构决策（见 src/tools/jie2_line.cpp 头部注释）：Qt6 不暴露底层
    # wl_surface，无法给 Qt 窗口设置 layer 角色 —— 因此走**独立进程**
    # jie2-line（纯 wayland-client 渲染器），其构建配置在 src/tools/CMakeLists.txt。
endif()
