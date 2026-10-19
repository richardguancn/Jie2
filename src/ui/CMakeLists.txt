# 「J2」UI 层（ui）
#
# 依赖方向：ui -> platform -> core（允许）；ui 不得被下层反向依赖。
# 本层是唯一允许使用 Qt 的地方。替换 UI 技术（Widgets <-> QML）
# 只应影响本目录，不应波及 core 与 platform。

add_executable(jie2 WIN32 MACOSX_BUNDLE
    ../../assets/brand/brand.qrc
    annotation_render.cpp
    app_paths.cpp
    clothesline.cpp
    flight_overlay.cpp
    main.cpp
    ocr_runner.cpp
    pin_window.cpp
    selection_overlay.cpp
    settings_dialog.cpp
    single_instance.cpp
    theme.cpp
    tray.cpp
)

# 注意：Wayland 下衣架不由 Qt 窗口实现，也不在 UI 层做 layer-shell 适配 ——
# 走独立进程 jie2-line（架构决策见 src/tools/jie2_line.cpp），
# 本层只通过 tray.cpp 的 updateWaylandLine() 写 items 文件并拉起它。

target_include_directories(jie2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(jie2 PRIVATE
    jie2_platform
    jie2_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)



if(APPLE)
    set(JIE2_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/brand/AppIcon.icns")
    target_sources(jie2 PRIVATE "${JIE2_APP_ICON}")
    set_source_files_properties("${JIE2_APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(jie2 PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        MACOSX_BUNDLE_BUNDLE_NAME "J2"
        MACOSX_BUNDLE_GUI_IDENTIFIER "app.jie2.desktop"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

if(MSVC)
    target_compile_options(jie2 PRIVATE /W4)
else()
    target_compile_options(jie2 PRIVATE -Wall -Wextra)
endif()
