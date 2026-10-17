cmake_minimum_required(VERSION 3.21)

# 必须在 project() 之前设置。取值 14.0 是两条约束的交集：
#   1) Qt 6.11 的二进制构建于 macOS 14.0，部署目标低于它会产生链接告警与运行风险；
#   2) CGDisplayCreateImage 系列在 macOS 15.0 被标为 obsoleted——若部署目标
#      ≥15.0 会变成编译期硬错误。Snipaste 2.x 至今仍在用同一组 API，
#      说明其运行时符号在现行系统上仍然存在。计划在 M7 迁移到 ScreenCaptureKit。
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# 语言包含 C：jie2-line（Wayland layer-shell 顶部条）需要编译
# wayland-scanner 生成的 .c 协议代码（interface 表符号）。
# 版本号在这里是唯一来源：打包脚本（deb / rpm / AppImage / macOS）都从本行
# 解析版本，发布 tag 必须与它一致（.github/workflows/release.yml 会校验）。
project(jie2
    VERSION 0.0.1
    DESCRIPTION "截2 — 跨平台截图 / 标注 / 贴图工具"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_definitions(JIE2_VERSION="${PROJECT_VERSION}")

option(JIE2_BUILD_TESTS "Build jie2 unit tests" ON)

# ---------------------------------------------------------------------------
# 三层解耦（评估报告 3.5.3）：
#   core     — 图像内核，纯 C++，不依赖任何 UI 框架与平台 API
#   platform — 平台集成层，依赖 core，不依赖 UI 层
#   ui       — UI 层，依赖 core 与 platform；可整体替换（Widgets <-> QML）
# 依赖方向单向向下，禁止反向依赖。
# ---------------------------------------------------------------------------

add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/tools)

# UI 层需要 Qt 6。若环境没有 Qt6（例如纯内核 CI 容器），允许只构建下两层。
# Network 供 M5 的 OpenAI 兼容端点使用。
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network)
if(Qt6_FOUND)
    # Q_OBJECT 需要 moc；AUTOMOC 必须在创建目标之前启用。
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    add_subdirectory(src/ui)
    message(STATUS "jie2: Qt ${Qt6_VERSION} found — building UI layer")
else()
    message(WARNING "jie2: Qt6 not found — building core/platform only")
endif()

if(JIE2_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
